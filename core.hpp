#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace smh {

inline constexpr wchar_t version[] = L"0.4.0";

struct Options {
    std::wstring title;
    bool help = false;
    bool versionOnly = false;
    bool vsync = false;
    bool tearing = false;
    bool waitable = false;
    bool scalePoint = false;
    bool debugDevice = false;
    bool stats = true;
};

inline Options ParseOptions(const std::vector<std::wstring>& args) {
    Options out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") out.help = true;
        else if (arg == L"--version") out.versionOnly = true;
        else if (arg == L"--title") {
            if (++i == args.size() || args[i].empty() || args[i].starts_with(L"--"))
                throw std::invalid_argument("--title requires a nonempty window title substring.");
            out.title = args[i];
        } else if (arg == L"--vsync") out.vsync = true;
        else if (arg == L"--tearing") out.tearing = true;
        else if (arg == L"--waitable") out.waitable = true;
        else if (arg == L"--debug-device") out.debugDevice = true;
        else if (arg == L"--no-stats") out.stats = false;
        else if (arg == L"--scale") {
            if (++i == args.size() || args[i] != L"point")
                throw std::invalid_argument("The only scaling option is --scale point.");
            out.scalePoint = true;
        } else {
            throw std::invalid_argument("Unknown option. Run SmoothMotionHost.exe --help.");
        }
    }
    if (out.vsync && out.tearing)
        throw std::invalid_argument("--vsync and --tearing cannot be combined.");
    return out;
}

struct Size {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    constexpr bool operator==(const Size&) const = default;
    constexpr bool Valid() const noexcept { return width > 0 && height > 0; }
};

struct TextureShape {
    Size size;
    std::uint32_t format = 0;
    std::uint32_t mipLevels = 1;
    std::uint32_t arraySize = 1;
    std::uint32_t samples = 1;
    std::uint32_t sampleQuality = 0;
};

enum class CopyPath { invalid, wholeResource, contentRegion, scale };

// A WGC texture can be larger than ContentSize during a resize. Never copy
// undefined padding, stretch a clipped frame, or compare dimensions alone.
inline CopyPath ChoosePath(Size content, const TextureShape& source,
                           const TextureShape& destination) noexcept {
    if (!content.Valid() || !destination.size.Valid() ||
        content.width > source.size.width || content.height > source.size.height ||
        source.format != destination.format || source.samples != 1 ||
        destination.samples != 1 || source.sampleQuality != 0 ||
        destination.sampleQuality != 0 || source.arraySize != 1 ||
        destination.arraySize != 1 || source.mipLevels != 1 || destination.mipLevels != 1)
        return CopyPath::invalid;
    if (content != destination.size) return CopyPath::scale;
    return source.size == destination.size ? CopyPath::wholeResource : CopyPath::contentRegion;
}

struct Viewport {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

inline Viewport FitViewport(Size source, Size destination) {
    if (!source.Valid() || !destination.Valid())
        throw std::invalid_argument("Cannot fit a zero-sized image.");
    const float scale = std::min(static_cast<float>(destination.width) / static_cast<float>(source.width),
                                 static_cast<float>(destination.height) / static_cast<float>(source.height));
    const float width = static_cast<float>(source.width) * scale;
    const float height = static_cast<float>(source.height) * scale;
    return {(static_cast<float>(destination.width) - width) * 0.5f,
            (static_cast<float>(destination.height) - height) * 0.5f, width, height};
}

inline std::uint64_t PackSize(Size value) noexcept {
    return (static_cast<std::uint64_t>(value.width) << 32) | value.height;
}
inline Size UnpackSize(std::uint64_t value) noexcept {
    return {static_cast<std::uint32_t>(value >> 32), static_cast<std::uint32_t>(value)};
}

} // namespace smh
