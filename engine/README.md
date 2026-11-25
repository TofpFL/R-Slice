# RAID Engine (native core)

The C++20 engine behind R-Slice's `.raid` chart format and streaming
playback. This directory is the **single source of truth** for the engine —
the game build (`source/funkin/external/raid/project/Build.xml`) compiles
the thin `raid_bridge.cpp` shim against these sources and links the static
libraries built here.

## Layout

- `src/` — engine sources. `gh_core` is the portable core (chart formats,
  `.raid` codec, streaming session, Lua mod host, engine C ABI in
  `engine_abi.*`); `raid_cli.cpp` is the `raid_engine` CLI; `raidtool.*`
  is the Win32 GUI front-end.
- `tests/` — headless test suite (`gh_tests`).
- `docs/` — format/architecture notes (`ICHART_SOURCE.md`, `RENDERER.md`,
  `PERFORMANCE.md`, ...).
- `cmake/miniz_export.h` — miniz export header.
- `raid_engine.rc`, `RAID_File_Icon.ico` — CLI version resource/icon.

## Building

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --target gh_core gh_lua gh_miniz
```

Or use the repo-level helpers:

```sh
tools/build_engine.bat            # Windows: MSVC libs + Android ABIs
tools/build_engine.sh             # same, for git-bash/Linux/macOS
```

Outputs the game links against:

| Platform | Output |
|---|---|
| Windows desktop | `engine/build-msvc/Release/gh_{core,lua,miniz}.lib` |
| Android per ABI | `engine/build-android/<abi>/libgh_{core,lua,miniz}.a` |
| Other host | `engine/build/libgh_{core,lua,miniz}.a` |

Tools built alongside: `raid_engine` (CLI) and `gh_tests` (tests, run with
`ctest` or `./gh_tests --raid <chart> <out.raid>`).

## Third-party dependencies (FetchContent, pinned)

| Dep | Version | License | Used for |
|---|---|---|---|
| Lua | v5.4.7 (`lua/lua`) | MIT | mod scripting (`lua_host`, `hscript`) |
| miniz | 3.0.2 (`richgel999/miniz`) | MIT | `.raid`/`.rchr` deflate+CRC containers, mod zips |
| raylib | 5.5 (`raysan5/raylib`) | zlib | `raidtool` GUI only (`-DGH_BUILD_RADIOTOOL=OFF` to skip) |

Deps are downloaded on first configure; drop vendored copies into
`engine/third_party/<dep>` to build offline. Lua is compiled as C++ so
script errors unwind through C++ exceptions instead of `longjmp`.

## Platform notes

- `gh_core` is pure C++20 with no window/audio dependency — it builds on
  Windows (MSVC), Linux, macOS, and Android NDK.
- Android: `cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28`. The libs are
  static archives absorbed into the game's `.so` at link time.
- The game-facing C ABI is `engine_abi.h` (`gh_engine_*`, `extern "C"`).
