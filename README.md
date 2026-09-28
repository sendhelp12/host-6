# SmoothMotionHost 0.4.0

A small experimental **SDR D3D11 presentation host** for testing your existing
SM86 installation on an RTX 3080:

`game HWND -> Windows Graphics Capture -> GPU copy -> flip-model Present -> external SM86`

The host does not implement frame generation, modify the game, or install SM86.
Its overhead and SM86 compatibility still need measurement on your PC.

## Upload/build correction

This updated ZIP puts all C++ files at the repository root, so there are no
`src` or `tests` folders to miss during upload. Replace the old CMakeLists.txt and
workflow as well as uploading the source files. Uploading just the ZIP to GitHub
does not extract it for the build. If `.github` is hidden in your file picker,
open the existing `.github/workflows/build.yml` on GitHub and replace its contents
with the visible `build-workflow.yml` copy at the ZIP root. That root copy is
only for convenient copying; GitHub runs the copy under `.github/workflows/`.

The configure step now explicitly uses `cmd` and quotes the entire SDK argument.
An early check identifies missing uploaded files before CMake runs.

## Get the executable without installing Visual Studio

1. Extract the source ZIP. Put its **contents at your GitHub repository root**:
   `CMakeLists.txt`, `host.cpp`, `core.hpp`, `core_tests.cpp`, and the supplied
   `.github/workflows/build.yml` must all be included. Avoid an extra enclosing `SmoothMotionHost` directory.
2. Open **Actions -> Build SmoothMotionHost -> Run workflow**. A push also builds it.
3. After a successful run, download **SmoothMotionHost-v0.4.0-win-x64** under
   Artifacts and extract it. This is the ZIP containing the executable.

The workflow uses Windows 2022, VS2022 x64 and SDK 10.0.26100.0, builds Release and
Debug, and runs tests that require no GPU. The supplied source ZIP is not a prebuilt
executable. See [VALIDATION.md](VALIDATION.md) for what was actually checked.

## First run: without Smooth Motion

1. Use Windows 11 (Windows 10 2004+ is the minimum target), with HDR disabled in
   Windows and the game for this SDR test.
2. Start the game in **borderless mode at the display's native resolution**.
   On your 4K display, the captured window must be 3840 x 2160, not just its
   internal render resolution. Set display refresh to 240 Hz before starting.
3. Run `run_without_sm86.cmd` and choose the game number.
4. Confirm the console reports **Direct Copy**. Compare the same game scene to
   the game alone and Magpie, using the same settings and measurement method.

**Ctrl+Shift+Q** quits. **Ctrl+Shift+H** hides/shows the output so you can access
the game or console. The host attempts to leave the source focused.

The output stays an ordinary, non-layered flip-model window. Cross-process mouse
click-through is not guaranteed. Start with a controller or a game using raw
mouse input; hide the output for menus. Input forwarding is outside this prototype.

## Then run through SM86

Quit the first host, then run `launch_with_sm86.cmd`. It uses:

```text
%LOCALAPPDATA%\Programs\SmoothMotionSM86\sm86.exe
```

Set the `SM86_EXE` environment variable if yours is elsewhere. The script passes
the host executable and its directory to the launcher; the game remains the capture
source. It uses the launcher syntax supplied in the project brief.

## Optional comparisons

```bat
SmoothMotionHost.exe --title "Sekiro"
SmoothMotionHost.exe --title "Sekiro" --vsync
SmoothMotionHost.exe --title "Sekiro" --waitable
SmoothMotionHost.exe --title "Sekiro" --tearing
SmoothMotionHost.exe --title "Sekiro" --scale point
```

Change one option at a time. Default is **Present(0,0), two buffers, maximum frame
latency 1, no scaling**. `--vsync` uses Present(1,0); `--waitable` enables an explicit
DXGI availability wait; `--tearing` is an alternative to VSync. With `--scale point`,
equal-size frames still use Direct Copy; unequal sizes use a GPU copy plus one draw.
Without it, size mismatches produce a clear error rather than changing the benchmark.

These are **host** arguments. The SM86 script runs the default baseline. To pass
host arguments through SM86, consult your installed `sm86.exe launch --help`;
argument forwarding varies and is not guessed by this project.

`--no-stats` removes the statistics thread. `--debug-device` enables the D3D11
debug layer if Windows Graphics Tools is installed; use it for diagnostics, not FPS.

## Read the numbers correctly

- **Capture FPS:** frames obtained from WGC, not the game's internal FPS.
- **Present FPS:** successful host Present calls, not generated or displayed FPS.
- **Host drops:** acquired frames discarded by this host; WGC/driver/display drops
  that are not exposed to the host are not included.
- **Present CPU ms:** time spent inside Present, including any hook/driver blocking;
  it is not GPU copy time or end-to-end latency.

The host does not verify that SM86 attaches or that 120 input frames become 240
distinct displayed frames. Use an independent display/presentation measurement.

Source resizing recreates the capture pool after returning all frames. A changed
display mode exits the host; restart it. Minimized sources must be restored before
restarting. Static or occluded games may stop delivering frames or throttle themselves.

Local build, if desired: `build_vs2022.cmd`. Implementation decisions, the v0.3
audit, and the VSync trade-offs are in [DEVELOPMENT.md](DEVELOPMENT.md).
