#include "core.hpp"

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

// Namespace aliases always start at the projected root. The native interop
// namespace ::Windows is used only for IDirect3DDxgiInterfaceAccess below.
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace wf = winrt::Windows::Foundation;
using winrt::check_hresult;
using winrt::com_ptr;

namespace {
constexpr wchar_t kWindowClass[] = L"SmoothMotionHostOutput04";
constexpr UINT kShowOutput = WM_APP + 1;
constexpr int kQuitHotkey = 1;
constexpr int kHideHotkey = 2;
constexpr int kCaptureBuffers = 2;
constexpr auto kCaptureFormat =
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
using Clock = std::chrono::steady_clock;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_) CloseHandle(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_ = nullptr;
};

Handle MakeEvent(bool manualReset) {
    Handle result(CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, nullptr));
    if (!result.get()) winrt::throw_last_error();
    return result;
}

struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

struct Shared {
    Handle stop = MakeEvent(true);
    Handle frameReady = MakeEvent(false);
    Handle outputShown = MakeEvent(true);
    Handle done = MakeEvent(true);
    std::atomic<bool> visible{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> acquired{0};
    std::atomic<std::uint64_t> presented{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> duplicates{0};
    std::atomic<std::uint64_t> sourceSize{0};
    std::atomic<std::uint64_t> presentNs{0};
    std::atomic<smh::CopyPath> path{smh::CopyPath::invalid};
    std::mutex errorMutex;
    std::wstring error;

    bool Stopping() const noexcept { return WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0; }
    void Stop() const noexcept { SetEvent(stop.get()); }
    void Fail(std::wstring text) {
        { std::scoped_lock lock(errorMutex); if (error.empty()) error = std::move(text); }
        Stop();
    }
};

std::wstring HResultText(const winrt::hresult_error& error) {
    std::wostringstream text;
    text << L"HRESULT 0x" << std::hex << std::uppercase
         << static_cast<std::uint32_t>(error.code().value) << L": " << error.message().c_str();
    return text.str();
}

class ConsoleMode {
public:
    ConsoleMode() {
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        if (GetConsoleMode(input_, &original_)) {
            changed_ = SetConsoleMode(input_, (original_ | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE) != FALSE;
        }
    }
    ~ConsoleMode() { if (changed_) SetConsoleMode(input_, original_); }
private:
    HANDLE input_ = nullptr;
    DWORD original_ = 0;
    bool changed_ = false;
};

struct WindowInfo { HWND hwnd = nullptr; std::wstring title; };
struct WindowList { std::vector<WindowInfo> values; std::exception_ptr error; };

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

BOOL CALLBACK EnumerateWindow(HWND hwnd, LPARAM value) noexcept {
    auto& list = *reinterpret_cast<WindowList*>(value);
    try {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == GetConsoleWindow()) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            return TRUE;
        wchar_t className[128]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (std::wstring_view(className).starts_with(L"SmoothMotionHostOutput")) return TRUE;
        const int length = GetWindowTextLengthW(hwnd);
        if (length <= 0) return TRUE;
        std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
        if (copied <= 0) return TRUE;
        title.resize(static_cast<std::size_t>(copied));
        list.values.push_back({hwnd, std::move(title)});
        return TRUE;
    } catch (...) {
        list.error = std::current_exception();
        return FALSE;
    }
}

WindowInfo PickWindow(const std::wstring& needle) {
    WindowList list;
    EnumWindows(EnumerateWindow, reinterpret_cast<LPARAM>(&list));
    if (list.error) std::rethrow_exception(list.error);
    if (!needle.empty()) {
        const auto lower = Lower(needle);
        std::erase_if(list.values, [&](const auto& entry) {
            return Lower(entry.title).find(lower) == std::wstring::npos;
        });
    }
    if (list.values.empty()) throw std::runtime_error("No matching visible window. Start the game in borderless mode first.");
    if (!needle.empty() && list.values.size() == 1) return list.values.front();
    if (!needle.empty()) std::wcout << L"More than one window matches. Choose the intended source:\n";
    for (std::size_t i = 0; i < list.values.size(); ++i)
        std::wcout << L"  " << i + 1 << L". " << list.values[i].title << L'\n';
    std::wcout << L"Window number: " << std::flush;
    std::wstring input;
    if (!std::getline(std::wcin, input)) throw std::runtime_error("No window selection was received.");
    std::wistringstream parser(input);
    std::size_t selected = 0;
    std::wstring extra;
    if (!(parser >> selected) || (parser >> extra) || selected == 0 || selected > list.values.size())
        throw std::runtime_error("Invalid window number.");
    return list.values[selected - 1];
}

struct UiState { std::shared_ptr<Shared> shared; HWND source = nullptr; bool hidden = false; };

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* state = reinterpret_cast<UiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<UiState*>(creation->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_NCHITTEST:
        // Best effort only: HTTRANSPARENT is NOT a cross-process click-through
        // guarantee. See README for the controller/raw-input baseline limitation.
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case kShowOutput:
        if (!state->shared->Stopping()) {
            SetForegroundWindow(state->source); // Best effort; never steals focus repeatedly.
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            state->shared->visible.store(true);
        }
        SetEvent(state->shared->outputShown.get());
        return 0;
    case WM_HOTKEY:
        if (wparam == kQuitHotkey) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (wparam == kHideHotkey && !state->shared->Stopping()) {
            state->hidden = !state->hidden;
            state->shared->visible.store(!state->hidden);
            ShowWindow(hwnd, state->hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
            if (!state->hidden) {
                SetForegroundWindow(state->source);
                SetEvent(state->shared->frameReady.get());
            }
        }
        return 0;
    case WM_DISPLAYCHANGE:
        // Output resolution is fixed for this benchmark. Do not resize a
        // swapchain on the message thread while a hook is presenting it.
        state->shared->Stop();
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_CLOSE:
        state->shared->Stop();
        ShowWindow(hwnd, SW_HIDE);
        return 0; // Keep HWND and its message pump alive until the worker exits.
    case WM_DESTROY:
        state->shared->Stop();
        return 0;
    default: return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

class OutputWindow {
public:
    OutputWindow(UiState& state, smh::Size& outputSize) {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = WindowProc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            winrt::throw_last_error();
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(MonitorFromWindow(state.source, MONITOR_DEFAULTTONEAREST), &info))
            winrt::throw_last_error();
        const auto& rect = info.rcMonitor;
        outputSize = {static_cast<UINT>(rect.right - rect.left), static_cast<UINT>(rect.bottom - rect.top)};
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            kWindowClass, L"SmoothMotionHost output", WS_POPUP,
            rect.left, rect.top, static_cast<int>(outputSize.width), static_cast<int>(outputSize.height),
            nullptr, nullptr, instance, &state);
        if (!hwnd_) winrt::throw_last_error();
        // Capture is scoped to another HWND, so no display-affinity exclusion is
        // needed. Keep this output visible to external recording/measurement.
        if (!RegisterHotKey(hwnd_, kQuitHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Q') ||
            !RegisterHotKey(hwnd_, kHideHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'H')) {
            UnregisterHotKey(hwnd_, kQuitHotkey);
            UnregisterHotKey(hwnd_, kHideHotkey);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            throw std::runtime_error("Ctrl+Shift+Q or Ctrl+Shift+H is already registered by another application.");
        }
    }
    ~OutputWindow() {
        if (hwnd_) {
            UnregisterHotKey(hwnd_, kQuitHotkey);
            UnregisterHotKey(hwnd_, kHideHotkey);
            DestroyWindow(hwnd_);
        }
    }
    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;
    HWND get() const noexcept { return hwnd_; }
private:
    HWND hwnd_ = nullptr;
};

wgd::IDirect3DDevice WrapDevice(ID3D11Device* device) {
    com_ptr<IDXGIDevice> dxgi;
    check_hresult(device->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()));
    com_ptr<::IInspectable> abiDevice;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), abiDevice.put()));
    return abiDevice.as<wgd::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem ItemForWindow(HWND hwnd) {
    const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem result{nullptr};
    check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(result)));
    return result;
}

com_ptr<ID3D11Texture2D> FrameTexture(const wgc::Direct3D11CaptureFrame& frame) {
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    return texture;
}

smh::TextureShape Shape(const D3D11_TEXTURE2D_DESC& value) {
    return {{value.Width, value.Height}, static_cast<std::uint32_t>(value.Format),
            value.MipLevels, value.ArraySize, value.SampleDesc.Count, value.SampleDesc.Quality};
}

class Renderer {
public:
    void Initialize(HWND hwnd, smh::Size size, const smh::Options& options) {
        size_ = size;
        options_ = options;
        com_ptr<IDXGIFactory6> factory;
        check_hresult(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), factory.put_void()));
        com_ptr<IDXGIAdapter1> selected;
        com_ptr<IDXGIAdapter1> fallback;
        for (UINT i = 0;; ++i) {
            com_ptr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                __uuidof(IDXGIAdapter1), candidate.put_void());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            check_hresult(hr);
            DXGI_ADAPTER_DESC1 desc{};
            check_hresult(candidate->GetDesc1(&desc));
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (!fallback) fallback = candidate;
            if (desc.VendorId == 0x10DE) { selected = candidate; break; }
        }
        if (!selected) selected = fallback;
        if (!selected) throw std::runtime_error("No hardware DXGI adapter. WARP is deliberately disabled for this performance benchmark.");
        DXGI_ADAPTER_DESC1 adapterDesc{};
        check_hresult(selected->GetDesc1(&adapterDesc));
        std::wcout << L"Adapter: " << adapterDesc.Description << L'\n';

        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (options.debugDevice) deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
        const HRESULT createResult = D3D11CreateDevice(selected.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            deviceFlags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            device_.put(), nullptr, context_.put());
        if (createResult == DXGI_ERROR_SDK_COMPONENT_MISSING)
            throw std::runtime_error("--debug-device needs Windows Graphics Tools. Omit the flag for normal testing.");
        check_hresult(createResult);
        // WGC may use this device internally. Only our worker calls the context,
        // but the runtime's per-call protection also covers shared-device use.
        // Never explicitly hold an Enter/Leave region around Present or waits.
        auto multithread = context_.as<ID3D11Multithread>();
        multithread->SetMultithreadProtected(TRUE);
        winrtDevice_ = WrapDevice(device_.get());

        if (options.tearing) {
            BOOL supported = FALSE;
            check_hresult(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported)));
            if (!supported) throw std::runtime_error("DXGI tearing is unavailable. Run without --tearing.");
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = size.width;
        desc.Height = size.height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (options.tearing) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        if (options.waitable) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        check_hresult(factory->CreateSwapChainForHwnd(device_.get(), hwnd, &desc, nullptr, nullptr, chain_.put()));
        check_hresult(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
        if (options.waitable) {
            const auto chain2 = chain_.as<IDXGISwapChain2>();
            check_hresult(chain2->SetMaximumFrameLatency(1));
            latency_ = Handle(chain2->GetFrameLatencyWaitableObject());
            if (!latency_.get()) throw std::runtime_error("DXGI returned no frame-latency handle. Run without --waitable.");
        } else {
            check_hresult(device_.as<IDXGIDevice1>()->SetMaximumFrameLatency(1));
        }
        check_hresult(chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer_.put_void()));
        backBuffer_->GetDesc(&backDesc_);
        // D3D11 maintains the logical back-buffer-zero mapping across flips.
        // There is no per-frame GetBuffer, allocation, RTV, shader or staging copy
        // in the equal-size path. Scaling resources are created only if requested.
    }

    const wgd::IDirect3DDevice& WinRTDevice() const noexcept { return winrtDevice_; }

    bool WaitReady(const Shared& shared) const {
        if (!latency_.get()) return !shared.Stopping();
        const HANDLE handles[]{shared.stop.get(), latency_.get()};
        for (int tries = 0; tries < 5; ++tries) {
            const DWORD result = WaitForMultipleObjects(2, handles, FALSE, 1000);
            if (result == WAIT_OBJECT_0) return false;
            if (result == WAIT_OBJECT_0 + 1) return true;
            if (result == WAIT_FAILED) winrt::throw_last_error();
        }
        throw std::runtime_error("The DXGI latency handle did not signal for 5 seconds. Retry without --waitable.");
    }

    smh::CopyPath Copy(const wgc::Direct3D11CaptureFrame& frame, smh::Size content) {
        const auto source = FrameTexture(frame); // Never saved after frame.Close().
        D3D11_TEXTURE2D_DESC srcDesc{};
        source->GetDesc(&srcDesc);
        const auto path = smh::ChoosePath(content, Shape(srcDesc), Shape(backDesc_));
        if (path == smh::CopyPath::invalid)
            throw std::runtime_error("Capture texture has an unexpected size, format, or sample layout. No unsafe copy was submitted.");
        if (path == smh::CopyPath::wholeResource) {
            context_->CopyResource(backBuffer_.get(), source.get());
        } else if (path == smh::CopyPath::contentRegion) {
            const D3D11_BOX box{0, 0, 0, content.width, content.height, 1};
            context_->CopySubresourceRegion(backBuffer_.get(), 0, 0, 0, 0, source.get(), 0, &box);
        } else {
            if (!options_.scalePoint) {
                std::ostringstream error;
                error << "Copy-only mode: capture is " << content.width << 'x' << content.height
                      << " but output is " << size_.width << 'x' << size_.height
                      << ". Set the game to native-resolution borderless mode, or explicitly use --scale point.";
                throw std::runtime_error(error.str());
            }
            Scale(source.get(), content);
        }
        return path;
    }

    HRESULT Present() {
        const UINT flags = options_.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        const HRESULT result = chain_->Present(options_.vsync ? 1u : 0u, flags);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            const HRESULT reason = device_->GetDeviceRemovedReason();
            throw winrt::hresult_error(FAILED(reason) ? reason : result,
                L"The D3D11 device was removed/reset. Restart the host; compare without SM86.");
        }
        check_hresult(result);
        return result;
    }

private:
    static com_ptr<ID3DBlob> Compile(const char* text, std::size_t length, const char* profile) {
        com_ptr<ID3DBlob> blob;
        com_ptr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(text, length, "SmoothMotionHost point scaler", nullptr, nullptr,
            "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put());
        if (FAILED(result) && errors) {
            throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        }
        check_hresult(result);
        return blob;
    }

    void EnsureScaler(smh::Size content) {
        if (!vertexShader_) {
            constexpr char vertex[] = R"(
struct Varying { float4 position : SV_Position; float2 uv : TEXCOORD; };
Varying main(uint index : SV_VertexID) {
    Varying v;
    v.uv = float2((index << 1) & 2, index & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
})";
            constexpr char pixel[] = R"(
Texture2D image : register(t0);
SamplerState nearestSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    return image.Sample(nearestSampler, uv);
})";
            const auto vs = Compile(vertex, sizeof(vertex) - 1, "vs_5_0");
            const auto ps = Compile(pixel, sizeof(pixel) - 1, "ps_5_0");
            check_hresult(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertexShader_.put()));
            check_hresult(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixelShader_.put()));
            check_hresult(device_->CreateRenderTargetView(backBuffer_.get(), nullptr, target_.put()));
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
            check_hresult(device_->CreateSamplerState(&sampler, sampler_.put()));
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            check_hresult(device_->CreateRasterizerState(&raster, raster_.put()));
        }
        if (scaleSize_ != content) {
            scaleView_ = nullptr;
            scaleInput_ = nullptr;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = content.width;
            desc.Height = content.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            check_hresult(device_->CreateTexture2D(&desc, nullptr, scaleInput_.put()));
            check_hresult(device_->CreateShaderResourceView(scaleInput_.get(), nullptr, scaleView_.put()));
            scaleSize_ = content;
        }
    }

    void Scale(ID3D11Texture2D* source, smh::Size content) {
        EnsureScaler(content);
        const D3D11_BOX box{0, 0, 0, content.width, content.height, 1};
        context_->CopySubresourceRegion(scaleInput_.get(), 0, 0, 0, 0, source, 0, &box);
        const auto fit = smh::FitViewport(content, size_);
        if (fit.x > 0 || fit.y > 0) {
            constexpr float black[]{0, 0, 0, 1};
            context_->ClearRenderTargetView(target_.get(), black);
        }
        const D3D11_VIEWPORT viewport{fit.x, fit.y, fit.width, fit.height, 0, 1};
        ID3D11RenderTargetView* target = target_.get();
        ID3D11ShaderResourceView* view = scaleView_.get();
        ID3D11SamplerState* sampler = sampler_.get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(raster_.get());
        context_->RSSetViewports(1, &viewport);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.get(), nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->PSSetShader(pixelShader_.get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, &view);
        context_->PSSetSamplers(0, 1, &sampler);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }

    smh::Options options_;
    smh::Size size_;
    smh::Size scaleSize_;
    com_ptr<ID3D11Device> device_;
    com_ptr<ID3D11DeviceContext> context_;
    wgd::IDirect3DDevice winrtDevice_{nullptr};
    com_ptr<IDXGISwapChain1> chain_;
    Handle latency_;
    com_ptr<ID3D11Texture2D> backBuffer_;
    D3D11_TEXTURE2D_DESC backDesc_{};
    com_ptr<ID3D11Texture2D> scaleInput_;
    com_ptr<ID3D11ShaderResourceView> scaleView_;
    com_ptr<ID3D11RenderTargetView> target_;
    com_ptr<ID3D11VertexShader> vertexShader_;
    com_ptr<ID3D11PixelShader> pixelShader_;
    com_ptr<ID3D11SamplerState> sampler_;
    com_ptr<ID3D11RasterizerState> raster_;
};

struct FrameLease {
    wgc::Direct3D11CaptureFrame value{nullptr};
    ~FrameLease() { Reset(); }
    FrameLease() = default;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    void Reset(wgc::Direct3D11CaptureFrame next = wgc::Direct3D11CaptureFrame{nullptr}) noexcept {
        if (value) { try { value.Close(); } catch (...) {} }
        value = std::move(next);
    }
};

class Capture {
public:
    ~Capture() {
        // Revocation does not require a callback to use this Capture/Renderer:
        // callbacks own only Shared, so late dispatch cannot use a dead object.
        arrived_.revoke();
        closed_.revoke();
        if (session_) { try { session_.Close(); } catch (...) {} }
        if (pool_) { try { pool_.Close(); } catch (...) {} }
    }
    void Initialize(HWND source, const wgd::IDirect3DDevice& device, const std::shared_ptr<Shared>& shared) {
        item_ = ItemForWindow(source);
        const auto size = item_.Size();
        if (size.Width <= 0 || size.Height <= 0)
            throw std::runtime_error("The selected window has no capturable area.");
        size_ = {static_cast<UINT>(size.Width), static_cast<UINT>(size.Height)};
        device_ = device;
        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, kCaptureFormat, kCaptureBuffers, size);
        session_ = pool_.CreateCaptureSession(item_);
        if (auto cursorControl = session_.try_as<wgc::IGraphicsCaptureSession2>())
            cursorControl.IsCursorCaptureEnabled(false);
        arrived_ = pool_.FrameArrived(winrt::auto_revoke,
            [shared](const wgc::Direct3D11CaptureFramePool&, const wf::IInspectable&) noexcept {
                SetEvent(shared->frameReady.get());
            });
        closed_ = item_.Closed(winrt::auto_revoke,
            [shared](const wgc::GraphicsCaptureItem&, const wf::IInspectable&) noexcept {
                shared->Stop();
            });
        session_.StartCapture();
    }
    smh::Size Size() const noexcept { return size_; }
    void DrainLatest(FrameLease& latest, Shared& shared) {
        // Bounded by the pool capacity: continuous capture cannot starve Present.
        // There is no second FIFO or texture mailbox retaining stale surfaces.
        for (int i = 0; i < kCaptureBuffers; ++i) {
            auto frame = pool_.TryGetNextFrame();
            if (!frame) break;
            shared.acquired.fetch_add(1, std::memory_order_relaxed);
            if (latest.value) shared.dropped.fetch_add(1, std::memory_order_relaxed);
            latest.Reset(std::move(frame));
        }
    }
    void Recreate(smh::Size size) {
        // Called only by the owner thread, with every checked-out frame closed.
        pool_.Recreate(device_, kCaptureFormat, kCaptureBuffers,
            winrt::Windows::Graphics::SizeInt32{static_cast<int>(size.width), static_cast<int>(size.height)});
        size_ = size;
    }
private:
    smh::Size size_;
    wgd::IDirect3DDevice device_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived_;
    wgc::GraphicsCaptureItem::Closed_revoker closed_;
};

void RenderLoop(HWND source, HWND output, smh::Size outputSize, const smh::Options& options,
                const std::shared_ptr<Shared>& shared) {
    Apartment apartment;
    if (!wgc::GraphicsCaptureSession::IsSupported())
        throw std::runtime_error("Windows Graphics Capture is unavailable on this desktop.");
    Renderer renderer;
    renderer.Initialize(output, outputSize, options);
    // Setup is the only explicit wait for the UI. During capture the UI never
    // waits for this worker, and callbacks never wait for either thread.
    if (!PostMessageW(output, kShowOutput, 0, 0)) winrt::throw_last_error();
    const HANDLE readyHandles[]{shared->stop.get(), shared->outputShown.get()};
    const DWORD shown = WaitForMultipleObjects(2, readyHandles, FALSE, INFINITE);
    if (shown == WAIT_FAILED) winrt::throw_last_error();
    if (shown != WAIT_OBJECT_0 + 1) return;
    Capture capture;
    capture.Initialize(source, renderer.WinRTDevice(), shared);
    std::wcout << L"Output: " << outputSize.width << L'x' << outputSize.height
               << L" | Present(" << (options.vsync ? 1 : 0) << L", "
               << (options.tearing ? L"ALLOW_TEARING" : L"0") << L")"
               << L" | waitable " << (options.waitable ? L"ON" : L"OFF")
               << L" | " << (options.scalePoint ? L"point fallback allowed" : L"copy-only") << L'\n'
               << L"Ctrl+Shift+Q: quit. Ctrl+Shift+H: hide/show output for game menus.\n" << std::flush;
    shared->started.store(true);
    const HANDLE frameHandles[]{shared->stop.get(), shared->frameReady.get()};
    std::int64_t lastTimestamp = 0;
    bool haveTimestamp = false;
    bool latencyGranted = false;
    while (!shared->Stopping()) {
        const DWORD wait = WaitForMultipleObjects(2, frameHandles, FALSE, 1000);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) winrt::throw_last_error();
        if (!IsWindow(source)) break;
        if (IsIconic(source))
            throw std::runtime_error("The source was minimized. Restore it and restart the host.");
        if (wait == WAIT_TIMEOUT) continue; // Static content need not generate frames.
        if (!shared->visible.load()) {
            // Hidden output is an explicit pause, not an FPS limiter.
            if (WaitForSingleObject(shared->stop.get(), 100) == WAIT_OBJECT_0) break;
            SetEvent(shared->frameReady.get());
            continue;
        }
        // Acquire availability before taking the newest image. Retain the grant
        // if a stale wake, resize or duplicate means no Present is submitted.
        if (!latencyGranted) {
            if (!renderer.WaitReady(*shared)) break;
            latencyGranted = true;
        }
        FrameLease frame;
        capture.DrainLatest(frame, *shared);
        if (!frame.value) continue;
        const auto size = frame.value.ContentSize();
        if (size.Width <= 0 || size.Height <= 0) {
            shared->dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const smh::Size content{static_cast<UINT>(size.Width), static_cast<UINT>(size.Height)};
        shared->sourceSize.store(smh::PackSize(content), std::memory_order_relaxed);
        if (content != capture.Size()) {
            shared->dropped.fetch_add(1, std::memory_order_relaxed);
            frame.Reset();
            capture.Recreate(content);
            haveTimestamp = false;
            continue;
        }
        const auto timestamp = frame.value.SystemRelativeTime().count();
        if (haveTimestamp && timestamp <= lastTimestamp) {
            shared->duplicates.fetch_add(1, std::memory_order_relaxed);
            shared->dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        lastTimestamp = timestamp;
        haveTimestamp = true;
        if (shared->Stopping()) break;
        shared->path.store(renderer.Copy(frame.value, content), std::memory_order_relaxed);
        const auto before = Clock::now();
        // NO host mutex, WGC callback, UI operation or explicit context lock
        // encloses this call. A driver/hook can still block inside Present.
        const HRESULT result = renderer.Present();
        latencyGranted = false;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - before).count();
        // Keep the WGC frame checked out until its GPU use has been submitted by
        // Present. Do not keep only its texture and return the frame prematurely.
        frame.Reset();
        if (result == DXGI_STATUS_OCCLUDED) {
            shared->dropped.fetch_add(1, std::memory_order_relaxed);
            if (WaitForSingleObject(shared->stop.get(), 100) == WAIT_OBJECT_0) break;
        } else {
            shared->presentNs.fetch_add(static_cast<std::uint64_t>(elapsed), std::memory_order_relaxed);
            shared->presented.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Capture is destroyed before renderer, and both before apartment teardown.
}

void RenderThread(HWND source, HWND output, smh::Size size, smh::Options options,
                  const std::shared_ptr<Shared>& shared) noexcept {
    try {
        RenderLoop(source, output, size, options, shared);
    } catch (const winrt::hresult_error& error) {
        shared->Fail(HResultText(error));
    } catch (const std::exception& error) {
        shared->Fail(winrt::to_hstring(error.what()).c_str());
    } catch (...) {
        shared->Fail(L"Unexpected worker failure.");
    }
    shared->Stop();
    SetEvent(shared->done.get()); // All graphics/capture cleanup has completed.
}

void StatsThread(const std::shared_ptr<Shared>& shared, smh::Size output) noexcept {
    try {
        auto last = Clock::now();
        std::uint64_t oldCaptured = 0, oldPresented = 0, oldNs = 0;
        while (WaitForSingleObject(shared->stop.get(), 1000) == WAIT_TIMEOUT) {
            const auto now = Clock::now();
            if (!shared->started.load()) { last = now; continue; }
            const double seconds = std::chrono::duration<double>(now - last).count();
            const auto captured = shared->acquired.load(std::memory_order_relaxed);
            const auto presented = shared->presented.load(std::memory_order_relaxed);
            const auto ns = shared->presentNs.load(std::memory_order_relaxed);
            const auto source = smh::UnpackSize(shared->sourceSize.load(std::memory_order_relaxed));
            const auto path = shared->path.load(std::memory_order_relaxed);
            const wchar_t* pathName = L"Waiting";
            if (path == smh::CopyPath::wholeResource) pathName = L"Direct Copy";
            else if (path == smh::CopyPath::contentRegion) pathName = L"Region Copy";
            else if (path == smh::CopyPath::scale) pathName = L"Point Scale";
            std::wostringstream line;
            line << std::fixed << std::setprecision(1)
                 << L"Capture " << static_cast<double>(captured - oldCaptured) / seconds
                 << L" fps | Present " << static_cast<double>(presented - oldPresented) / seconds
                 << L" fps | Host drops " << shared->dropped.load(std::memory_order_relaxed)
                 << L" (same/old timestamp " << shared->duplicates.load(std::memory_order_relaxed) << L")"
                 << L" | " << source.width << L'x' << source.height << L" -> " << output.width << L'x' << output.height
                 << L" | " << pathName;
            if (presented != oldPresented)
                line << std::setprecision(2) << L" | Present CPU "
                     << static_cast<double>(ns - oldNs) / 1e6 / static_cast<double>(presented - oldPresented) << L" ms";
            if (!shared->visible.load()) line << L" | HIDDEN";
            line << L'\n';
            std::wcout << line.str() << std::flush;
            oldCaptured = captured;
            oldPresented = presented;
            oldNs = ns;
            last = now;
        }
    } catch (...) {
        // Losing a diagnostic console must not tear down capture or block UI.
    }
}

void Usage() {
    std::wcout << L"SmoothMotionHost " << smh::version << L"\n"
        L"Minimal SDR Windows Graphics Capture -> D3D11 -> flip Present host.\n\n"
        L"  SmoothMotionHost.exe                     Pick a visible game window\n"
        L"  SmoothMotionHost.exe --title \"Sekiro\"    Match a window title\n\n"
        L"Default: native-size copy-only, Present(0,0), two buffers, frame latency 1.\n"
        L"Options (change one at a time for SM86 comparison):\n"
        L"  --vsync          Present(1,0)\n"
        L"  --tearing        Present(0,ALLOW_TEARING), if supported\n"
        L"  --waitable       Enable the DXGI frame-latency waitable object\n"
        L"  --scale point    Permit one-pass point scaling when sizes differ\n"
        L"  --debug-device   Request the D3D11 debug layer (not for FPS tests)\n"
        L"  --no-stats       Disable once-per-second console statistics\n"
        L"  --version        Print version without initializing graphics\n"
        L"  --help           Show this help without initializing graphics\n\n"
        L"Ctrl+Shift+Q quits; Ctrl+Shift+H hides/shows output.\n"
        L"SM86 is external. Reported Present FPS does not count generated frames.\n";
}

int Run(const smh::Options& options) {
    ConsoleMode consoleMode;
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        GetLastError() != ERROR_ACCESS_DENIED) winrt::throw_last_error();
    const auto source = PickWindow(options.title);
    if (!IsWindow(source.hwnd) || IsIconic(source.hwnd))
        throw std::runtime_error("Source window closed or minimized during selection.");
    std::wcout << L"Capturing: " << source.title << L'\n';
    const auto shared = std::make_shared<Shared>();
    UiState state{shared, source.hwnd};
    smh::Size outputSize;
    OutputWindow output(state, outputSize);
    std::thread reporter;
    if (options.stats) reporter = std::thread(StatsThread, shared, outputSize);
    std::thread worker;
    try {
        worker = std::thread(RenderThread, source.hwnd, output.get(), outputSize, options, shared);
    } catch (...) {
        shared->Stop();
        if (reporter.joinable()) reporter.join();
        throw;
    }

    // Pump even after Stop: DXGI/SM86 can wait for this thread during Present or
    // destruction. Joining the worker from WM_CLOSE could deadlock both threads.
    const HANDLE done = shared->done.get();
    bool quitSeen = false;
    bool waitFailed = false;
    for (;;) {
        if (WaitForSingleObject(done, 0) == WAIT_OBJECT_0) break;
        const DWORD result = waitFailed
            ? MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
            : MsgWaitForMultipleObjectsEx(1, &done, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (!waitFailed && result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) {
            // Even a failed combined wait must not lead to a blocking join with
            // the output HWND unserviced. Fall back to pumping during shutdown.
            waitFailed = true;
            shared->Stop();
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quitSeen = true;
                shared->Stop();
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    worker.join();
    shared->Stop();
    if (reporter.joinable()) reporter.join();
    if (!shared->error.empty()) {
        std::wcerr << L"Stopped: " << shared->error << L'\n';
        return 1;
    }
    if (waitFailed) {
        std::wcerr << L"Stopped after a Windows message-wait failure.\n";
        return 1;
    }
    std::wcout << L"Host stopped" << (quitSeen ? L" (quit requested)" : L"") << L".\n";
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::wstring> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        const auto options = smh::ParseOptions(arguments);
        if (options.help) { Usage(); return 0; }
        if (options.versionOnly) { std::wcout << smh::version << L'\n'; return 0; }
        std::wcout << L"SmoothMotionHost " << smh::version << L" (experimental)\n";
        return Run(options);
    } catch (const winrt::hresult_error& error) {
        std::wcerr << HResultText(error) << L'\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
