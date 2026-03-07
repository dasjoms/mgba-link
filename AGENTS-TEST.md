# AGENTS-TEST.md

## Why CMake failed previously in this environment
The initial Qt configure failed for environment reasons, not because of the `TcpSession` code:

1. `libzip-dev` was installed, but command-line tools referenced by its exported CMake targets were missing (`/usr/bin/zipcmp`, `/usr/bin/zipmerge`).
2. Qt dev packages were not installed, so `find_package(Qt6 ... )` could not resolve.
3. In this repo, `src/platform/qt/CMakeLists.txt` appstream generation assumes git tags are present; in shallow/tagless environments, configure can fail unless `-DSKIP_GIT=ON` is passed.

## Why `ctest --test-dir build --output-on-failure` often reports no runnable tests
There are two common agent-environment pitfalls:

1. **No `build/` directory exists yet**
   - `ctest --test-dir build --output-on-failure` fails immediately with `Failed to change working directory`.
   - Fix: run CMake configure/build first.

2. **`BUILD_SUITE` is not enabled because `cmocka` is missing**
   - By default, this repo sets `BUILD_SUITE=OFF`, and only enables CTest entries when suite/test targets are on.
   - Even when `-DBUILD_SUITE=ON` is requested, CMake silently forces it back `OFF` if `cmocka` is not found.
   - Symptom: `ctest` runs but prints `No tests were found!!!`.
   - Fix: install `libcmocka-dev`, then configure with `-DBUILD_SUITE=ON`.

## Baseline packages for full Qt + GBA v1 link-net work
For Ubuntu/Debian agent images, install at least:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config git \
  libcmocka-dev \
  qt6-base-dev qt6-multimedia-dev qt6-tools-dev qt6-tools-dev-tools libqt6opengl6-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libegl1-mesa-dev libgl1-mesa-dev \
  libzip-dev zipcmp zipmerge ziptool
```

Recommended optional deps to reduce feature-disable noise during configure:

```bash
sudo apt-get install -y libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

## Configure/build commands that work in agent environments
Use `SKIP_GIT=ON` to avoid tag parsing failures in detached/shallow environments.

For running repository tests through `ctest`, you must explicitly enable the suite:

```bash
cmake -S . -B build -DBUILD_QT=ON -DBUILD_SDL=OFF -DBUILD_SUITE=ON -DSKIP_GIT=ON
cmake --build build -j"$(nproc)"
```

If you only need test binaries (and want to avoid Qt dependency setup), use:

```bash
cmake -S . -B build -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_SUITE=ON -DSKIP_GIT=ON
cmake --build build -j"$(nproc)"
```

If you need to validate without system zip toolchain, fallback configure is:

```bash
cmake -S . -B build -DBUILD_QT=ON -DBUILD_SDL=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF
```

## What must be testable for `docs/link-net-v1.md`
`link-net-v1` scope says v1 is **GBA core + Qt frontend + TCP transport**, so these areas should be build- and runtime-testable before merging:

1. **GBA link lockstep core path**
   - Build includes `src/gba/sio/lockstep.c` and related SIO code.
   - Ensure `libmgba` and `mgba-qt` both build successfully.

2. **Qt frontend multiplayer/session wiring surface**
   - Build includes `src/platform/qt/MultiplayerController.cpp` and `src/platform/qt/netplay/*`.
   - For transport changes, verify the new sources are included in Qt target build output.

3. **TCP transport behavior checks (manual/smoke until automated tests exist)**
   - `TcpSession` connect/disconnect lifecycle.
   - Length-prefixed frame encode/decode (4-byte big-endian + JSON payload).
   - Callback dispatch for parsed server events.
   - Heartbeat send/watchdog timeout behavior.

## Suggested repeatable verification checklist for agents
Run these in order and include outputs in PR notes:

```bash
# 1) Install test dependency (required so BUILD_SUITE stays ON)
sudo apt-get update && sudo apt-get install -y libcmocka-dev

# 2) Clean configure
cmake -S . -B build -DBUILD_QT=ON -DBUILD_SDL=OFF -DBUILD_SUITE=ON -DSKIP_GIT=ON

# 3) Full compile (validates Qt + GBA integration)
cmake --build build -j"$(nproc)"

# 4) Run tests (Execute the fix as instructed in the above section about the 'ctest --test-dir build --output-on-failure' command before running it to ensure tests are found)
ctest --test-dir build --output-on-failure
```

Expected behavior:
- If `BUILD_SUITE=OFF` (or forced off by missing `cmocka`), `ctest` reports no tests.
- If `BUILD_SUITE=ON` and build succeeds, `ctest` discovers the unit test executables.

And for netplay transport changes, add a small local smoke harness or targeted unit test where possible (then keep it in-tree if generally useful).
