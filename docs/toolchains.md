# Toolchains

RAID Engine builds with any of these Windows compilers. The `.RAID` chart
format is compiler-independent — verified bit-identical output (same MD5)
from MSVC 2022, GCC 16.2, and Clang 23.1 on the same input.

## Recommended: MSVC 2022 (BuildTools)

```
cmake -B build_game -G "Visual Studio 17 2022" -A x64
cmake --build build_game --config Release
:: or directly:
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" build_game\RAIDEngine.sln /p:Configuration=Release /p:Platform=x64 /m
```

Output: `build_game/Release/raide.exe` (game) and `raid_engine.exe` (CLI).

## GCC (MinGW-w64)

Any recent MinGW-w64 GCC works (tested: WinLibs GCC 16.2, UCRT runtime).
CMake + Ninja or MinGW Make; raylib builds fine under MinGW.

```
cmake -B build_game -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_game
```

## Clang (LLVM-MinGW)

LLVM-MinGW (tested: 23.1.x UCRT) works for `raid_engine.exe`. The full
game build additionally needs raylib compiled for the same runtime —
unverified end-to-end.

## Portability notes

- `gh_miniz` is compiled as **C89** on non-MSVC builds
  (`-std=gnu89` in CMakeLists): `miniz_tdef.c` uses a tentative
  definition + later definition pattern that strict C99+/C++ rejects.
  Do not "fix" this by editing `_deps` — it's upstream miniz.
- Lua is deliberately compiled as **C++** (exceptions instead of longjmp)
  on all toolchains.
- Known-good standalone `raid_engine` link line for MinGW:
  `g++ -O2 -std=c++20 <gh_core sources + lua .c as C++> miniz*.o -lpsapi`
  where the four miniz objects are compiled separately as C (`-std=gnu89`).

## Runtime DLLs on Windows

- **MSVC**: binaries need the Visual C++ Redistributable (same as any
  MSVC app); `raide.exe`/`raid_engine.exe` built here are fine on machines
  with the redist installed.
- **GCC (MinGW-w64)**: links `libstdc++-6.dll`, `libgcc_s_seh-1.dll`,
  `libwinpthread-1.dll` unless statically linked. For a shippable
  single-file `raid_engine.exe`, link with
  `-static -static-libgcc -static-libstdc++` (NOT plain `-static`, which
  also statically links system DLLs you don't want).
- **Clang (LLVM-MinGW)**: links `libc++.dll` and `libunwind.dll` from the
  toolchain's `bin/`. Either ship those DLLs next to the exe, or link
  with `-static` (fully static CRT+libc++; acceptable for a CLI tool).
  Do NOT mix `-static` into shared builds.

## If .RAID files get "rejected"

It is almost never the compiler. Check, in order:

1. `raid_engine info file.raid` — `version` field. Format v2 files
   (contain op-3 group / op-4 burst ops, produced by `.flp`/`.fsc`
   converts) require a build from after the v2 bump; v1 files load on
   any build.
2. Stale binary — rebuild from current `main`.
3. Corrupt/truncated copy — `verify` reports checksum errors.
