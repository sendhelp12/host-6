# Validation status for the prepared 0.4.0 source

## Upload/build correction

The supplied Windows log reached CMake generation, then reported missing
`src/host.cpp` and `tests/core_tests.cpp`; C++ compilation had not started.
The revised package moves these sources and core.hpp to the root and updates
CMake accordingly. The workflow now checks required files, explicitly uses cmd
for configuration, and quotes the SDK argument to prevent the observed split.
Windows compilation remains unverified until this corrected package is built.

## Completed locally

- Read every source/build/launcher/documentation file in the preceding v0.3 ZIP.
- Checked WinRT interop signatures, frame ownership, GPU-copy constraints and DXGI
  threading behavior against the primary references in DEVELOPMENT.md.
- Compiled and ran the platform-independent regression program with GCC 13.3,
  C++20, optimization and strict warnings. It passed.
- Regression coverage includes native 4K direct copy, stale allocation padding,
  resize clipping, incompatible formats/MSAA/arrays/mips, letterboxing and invalid
  command-line combinations.
- Parsed the workflow YAML and checked its runner, SDK, triggers and Release/Debug
  test steps; verified all staged package inputs and local documentation links.
  These are structural checks, not an executed GitHub Actions run.

## Not completed in this environment

This preparation environment is Linux, with no MSVC, Windows SDK, Windows desktop
or NVIDIA GPU access. **The Windows application has not been compiled or run here.**
Portable tests do not validate Windows headers, linker inputs, shader compilation,
live capture, Windows message behavior, game focus/input, SM86 attachment or FPS.
There is no prebuilt EXE in the source package and no measured performance result.

The included Actions workflow is the Windows build gate. A successful run should
produce Release and Debug builds, pass CLI/core tests, and attach the Release EXE
with a build-info.txt that identifies the exact source commit. A source review
or this validation note is not a substitute for that successful workflow run.

## Remaining runtime stages, in order

1. **Compile:** require a successful Actions run and executable artifact.
2. **Capture/presentation:** run directly without SM86. Check that the right game
   appears, remains focused, responds to your controls, and exits with Ctrl+Shift+Q.
   Ctrl+Shift+H should reveal the original game. Try a source resize with
   `--scale point`, then closing the source. The output display mode remains fixed.
3. **Performance:** use the same scene, game settings, output resolution, driver
   settings and measuring tool for game alone, Magpie Nearest, and this host.
   Confirm Direct Copy at 3840 x 2160. Compare frame times as well as average FPS.
4. **SM86:** launch the host through the existing launcher, select the same source,
   and check its own logs/measurement for actual attachment and generated output.
5. **Pacing:** compare default, --vsync, --waitable and --tearing separately only
   after the baseline is stable. A capture/Present mismatch is a clue, not proof
   of a specific driver or GPU bottleneck.

If a Windows build fails, retain the **first compiler diagnostic and full log**.
If runtime fails, retain the complete host error, options, source/output sizes,
driver and SM86 versions, and whether the same run works without SM86.
