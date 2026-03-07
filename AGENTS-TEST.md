# AGENTS-TEST.md

## Why CMake failed previously in this environment
The initial Qt configure failed for environment reasons, not because of the `TcpSession` code:

1. `libzip-dev` was installed, but command-line tools referenced by its exported CMake targets were missing (`/usr/bin/zipcmp`, `/usr/bin/zipmerge`).
2. Qt dev packages were not installed, so `find_package(Qt6 ... )` could not resolve.
3. In this repo, `src/platform/qt/CMakeLists.txt` appstream generation assumes git tags are present; in shallow/tagless environments, configure can fail unless `-DSKIP_GIT=ON` is passed.

## Baseline packages for full Qt + GBA v1 link-net work
For Ubuntu/Debian agent images, install at least:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config git \
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

```bash
cmake -S . -B build -DBUILD_QT=ON -DBUILD_SDL=OFF -DSKIP_GIT=ON
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
# 1) Clean configure
cmake -S . -B build -DBUILD_QT=ON -DBUILD_SDL=OFF -DSKIP_GIT=ON

# 2) Full compile (validates Qt + GBA integration)
cmake --build build -j"$(nproc)"

# 3) Tests when available
ctest --test-dir build --output-on-failure
```

And for netplay transport changes, add a small local smoke harness or targeted unit test where possible (then keep it in-tree if generally useful).
