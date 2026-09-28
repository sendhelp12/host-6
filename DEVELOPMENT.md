# Implementation and v0.3 audit

## What was inspected

The previous `SmoothMotionHost-v0.3-root.zip` contained `main.cpp`, CMakeLists,
the Actions workflow, README and three CMD scripts. All were reviewed. The v0.3
source already qualified its capture pixel-format enum; the earlier reported
`DirectXPixelFormat` error therefore cannot be attributed to an unqualified enum
at that location in this particular ZIP. No new complete Windows compiler log
accompanied the brief.

Remaining compile-risk areas were the event signatures using `winrt::IInspectable`
without a Foundation namespace import, and mixing native ABI capture declarations
with projected C++/WinRT declarations. The new event signatures use the actual
`winrt::Windows::Foundation::IInspectable` type through the `wf` alias. Every
projected graphics type is rooted in `winrt::Windows`; the native surface interop
interface is deliberately rooted in `::Windows`. `CreateForWindow` uses the
projected capture item's default-interface GUID with `put_abi`, avoiding a separate
`ABI::Windows::Graphics::Capture::IGraphicsCaptureItem` dependency.

These are source corrections, not a claim that an MSVC build was executed here.

## Changes that matter at runtime

| v0.3 issue | 0.4.0 behavior |
|---|---|
| Capture callback calls Render and Present under `_renderMutex` | Callback only signals an event; one worker owns WGC consumption, the immediate context and Present |
| Shutdown destroys the output HWND before graphics finishes | WM_CLOSE requests stop; the message thread pumps until graphics/capture cleanup finishes, then joins and destroys the HWND |
| Frame queue not drained | After output availability, a bounded drain keeps the newest acquired frame and returns older frames immediately |
| Copy path compares texture allocation dimensions alone | Checks ContentSize, formats, sample descriptions, array and mip layout; never treats stale pool padding as valid content |
| Source resizing disabled | Return current frames, recreate WGC pool on its owner thread, then accept complete frames of the new size |
| Waitable swapchain enabled, but its handle not used | Ordinary swapchain by default; optional waitable mode actually waits before acquiring the latest frame |
| Shaders and RTV built at every startup | Copy-only startup creates none; optional point scaler is lazy |
| Device selected implicitly | Prefer a high-performance NVIDIA hardware adapter; print the selected adapter; no silent software-rendered benchmark |
| DPI virtualization can distort the monitor dimensions | Set per-monitor-v2 DPI awareness before window creation |
| Callback exceptions logged repeatedly without stopping | Fatal errors stop once with an HRESULT/message; device removal includes its reason |
| Selection can include unrelated/own helper windows | Filter hidden, minimized, cloaked, tool, empty-title and host windows; ambiguous title matches require a numbered choice |
| Hardcoded account path in launcher | Use LOCALAPPDATA or SM86_EXE, preserve exit code, avoid a trailing-backslash quoting trap |

## Ownership and lifetime

The message thread owns the output window. It does no rendering or per-frame
work and never joins the render thread while DXGI can still need window messages.
Diagnostic output runs on a separate, optional once-per-second thread, so a slow
console does not hold up window messages. Console Quick Edit is disabled while
running and its previous setting is restored at exit.

The render thread initializes an MTA, creates the D3D11 device, wraps it for WGC,
and owns the capture session and pool. The free-threaded WGC events capture only
shared event handles with reference-counted lifetime. They cannot reference a
destroyed Renderer and do not call DXGI, window APIs, console I/O or frame-pool
recreation. Late callbacks remain harmless after revocation.

Frames stay checked out through texture access, GPU command submission and the
corresponding Present call. A frame's raw surface/texture is not retained after
Close. With two WGC buffers, the drain is capped at two attempts; it cannot loop
forever under continuous capture. At most one acquired frame is retained for
presentation outside that drain. There is no extra host FIFO or intermediate
render target in the matching-size path.

The immediate context has runtime multithread protection because WGC shares the
device. The application does not hold a context critical section or any host
mutex across Present. No CPU readback, busy-wait GPU completion check, or per-frame
Flush is added. A hook/driver can still block internally; this architecture cannot
guarantee that an external SM86 deadlock is impossible.

## GPU paths

For BGRA8, one mip, one array element, non-MSAA textures of matching dimensions:

```text
CopyResource(logical back buffer 0, captured texture)
Present(...)
```

Different bind flags alone do not require a shader. If a larger capture allocation
contains a valid output-sized content region, `CopySubresourceRegion` copies only
that region. A source that outgrows its pool is dropped and the pool recreated
before copying; undefined pixels are never deliberately stretched or presented.

With `--scale point`, a smaller/larger source is copied to one reusable
shader-readable texture of its content size, then drawn with one fullscreen
triangle into an aspect-preserving viewport. Letterbox areas are cleared only
when needed. This conservative fallback does not assume a WGC surface is SRV
bindable and does not retain it beyond its frame lifetime. It costs more than the
native-size path. Native-size frames continue to bypass it even after it has
been initialized.

D3D11 preserves the logical buffer-zero identity across flips, so GetBuffer is
performed once, not once per frame. That is specific to D3D11 and is not the
D3D12 back-buffer-index model.

## Present and pacing

| Mode | Behavior and reason to test it |
|---|---|
| Default: Present(0,0) | Conventional immediate submission for the initial hook compatibility baseline. Windows composition and driver behavior still affect output. It does not guarantee zero blocking or tearing-free scanout. |
| --vsync: Present(1,0) | Requests synchronization to a display interval. May improve unhooked output but can add backpressure; interaction with SM86 must be measured. |
| --tearing | Enables the matching swapchain and Present flags after a support query. Useful for a separate VRR/tearing comparison; mutually exclusive with --vsync. |
| --waitable | Uses a swapchain-specific latency of 1 and a stop-aware availability wait before acquiring the newest frame. Optional because hooks may interact differently with the waitable path. |

The ordinary swapchain uses device maximum frame latency 1; the waitable variant
uses swapchain maximum frame latency 1. These constrain different DXGI queues.
Neither setting guarantees a total end-to-end queue depth of one once WGC,
composition, a driver and an external hook are involved.

In waitable mode an availability grant is retained until a Present is actually
submitted. Stale wakeups, duplicate timestamps and resize transitions therefore
do not consume a grant and then wait forever for a submission that never happened.

There is no host FPS limiter. Static sources wait for capture notifications;
duplicate/non-increasing WGC timestamps are discarded. This is timestamp checking,
not expensive image hashing, so identical pixels with fresh timestamps may still
be presented. Timed waits are used only for shutdown checks, hidden output,
occlusion and waitable-handle diagnostics, not to pace normal frames.

For 120 real FPS and 240 Hz, the desired 8.33 ms real-frame interval and 4.17 ms
display interval remain a target for the complete SM86 pipeline. This host cannot
schedule or verify the externally generated frames.

## Practical limits

- WGC and desktop composition have their own cost. Eliminating Magpie's effect
  framework does not establish that all of the observed 120-to-97 FPS loss is
  avoidable. No 115-120 FPS result is claimed.
- A fully covered source may be throttled by the game or compositor even when
  focus is preserved. Low capture FPS alone does not identify the bottleneck.
- The output is deliberately non-layered. HTTRANSPARENT alone only has documented
  same-thread behavior; it is not a guarantee that all mouse clicks reach a game
  in another process. A layered overlay or general input-forwarding system would
  change the presentation experiment and is not silently added.
- Source-window capture may include decorations. Internal game render resolution
  is not necessarily WGC ContentSize. Use borderless native output for Direct Copy.
- SDR only; no HDR tone mapping, automatic device recovery or display-mode
  reconfiguration. A driver reset exits with a diagnostic rather than rebuilding
  the hook's swapchain behind its back.

## Primary API references

- [Win32 CreateForWindow](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)
- [C++/WinRT and ABI interop](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/interop-winrt-abi)
- [CreateDirect3D11DeviceFromDXGIDevice](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.directx.direct3d11.interop/nf-windows-graphics-directx-direct3d11-interop-createdirect3d11devicefromdxgidevice)
- [Free-threaded capture pool](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
- [Capture frame lifetime and ContentSize](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)
- [CopyResource restrictions](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
- [DXGI buffer identity and multithread considerations](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
- [Present semantics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
- [Waitable swapchains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)
- [Immediate-context multithread protection](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nn-d3d11_4-id3d11multithread)
- [Layered windows and input](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features)
- [Windows 2022 runner inventory](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)
