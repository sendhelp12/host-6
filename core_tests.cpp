#include "core.hpp"
#include <cmath>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void MustReject(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Invalid input was accepted");
}
}

int main() {
    try {
        using smh::CopyPath;
        const smh::TextureShape native{{3840, 2160}, 87, 1, 1, 1, 0};
        Check(smh::ChoosePath({3840, 2160}, native, native) == CopyPath::wholeResource,
              "4K equal-size path must use a single whole-resource copy");

        auto resized = native;
        resized.size = {4096, 2160};
        Check(smh::ChoosePath({3840, 2160}, resized, native) == CopyPath::contentRegion,
              "Padding outside ContentSize must not be copied");
        Check(smh::ChoosePath({4096, 2160}, native, native) == CopyPath::invalid,
              "A growing window must recreate capture before using a clipped texture");
        Check(smh::ChoosePath({1920, 1080}, native, native) == CopyPath::scale,
              "A stale large pool allocation must not disguise a smaller source as direct copy");
        Check(smh::ChoosePath({0, 2160}, native, native) == CopyPath::invalid,
              "Zero content size must not enter the copy/scaling path");
        auto mismatch = native;
        mismatch.format = 28;
        Check(smh::ChoosePath({3840, 2160}, mismatch, native) == CopyPath::invalid,
              "RGBA and BGRA are not interchangeable copy formats");
        mismatch = native;
        mismatch.samples = 4;
        Check(smh::ChoosePath({3840, 2160}, mismatch, native) == CopyPath::invalid,
              "Multisampling requires resolve, not this direct-copy path");
        mismatch = native;
        mismatch.arraySize = 2;
        Check(smh::ChoosePath({3840, 2160}, mismatch, native) == CopyPath::invalid,
              "Texture arrays must not enter CopyResource against a single-buffer texture");
        mismatch = native;
        mismatch.mipLevels = 2;
        Check(smh::ChoosePath({3840, 2160}, mismatch, native) == CopyPath::invalid,
              "Mip layouts must match");

        const auto pillarbox = smh::FitViewport({1600, 1200}, {3840, 2160});
        Check(pillarbox.x == 480 && pillarbox.y == 0 && pillarbox.width == 2880 && pillarbox.height == 2160,
              "4:3 source must keep aspect ratio on a 16:9 output");
        const auto letterbox = smh::FitViewport({3840, 1600}, {1920, 1080});
        Check(letterbox.x == 0 && letterbox.y == 140 && letterbox.width == 1920 && letterbox.height == 800,
              "Ultrawide source must be letterboxed");
        MustReject([] { (void)smh::FitViewport({0, 1080}, {3840, 2160}); });

        const auto baseline = smh::ParseOptions({});
        Check(!baseline.vsync && !baseline.tearing && !baseline.waitable && !baseline.scalePoint,
              "Baseline must be plain Present(0,0) and copy-only");
        const auto flags = smh::ParseOptions({L"--title", L"Sekiro", L"--scale", L"point", L"--waitable"});
        Check(flags.title == L"Sekiro" && flags.scalePoint && flags.waitable, "CLI option parsing failed");
        MustReject([] { (void)smh::ParseOptions({L"--title"}); });
        MustReject([] { (void)smh::ParseOptions({L"--title", L"--vsync"}); });
        MustReject([] { (void)smh::ParseOptions({L"--vsync", L"--tearing"}); });
        MustReject([] { (void)smh::ParseOptions({L"--scale", L"bilinear"}); });
        MustReject([] { (void)smh::ParseOptions({L"--typo"}); });
        Check(smh::UnpackSize(smh::PackSize({3840, 2160})) == smh::Size{3840, 2160}, "Stats size encoding failed");
        std::cout << "Core regression tests passed. These do not test Windows capture or SM86.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
