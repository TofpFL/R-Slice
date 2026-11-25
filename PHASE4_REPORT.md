# Phase 4 Report — V-Slice Framework Compatibility Build (C++20)

**Date:** 2026-11-17
**Result: SUCCESS** — the full V-Slice 0.8.6 desktop build compiles end-to-end through
Haxe → hxcpp (FunkinCrew fork `0376e9b`) → MSVC `/std:c++20` and produces a working
`Funkin.exe` that launches and runs the game's real preloader pipeline.

---

## 1. Environment

| Component | Version / Revision |
|---|---|
| Haxe | 4.3.7 (`C:\haxe\haxe_20250509143529_e0b355c`) |
| hxcpp | FunkinCrew/hxcpp @ `0376e9b19af9a4d91f9d9afc131a358467856a10` (exact pin) |
| lime | @ `ad0080e7e1df97d06060ea6cf5ce8f8088c3b872` (hmm pin) |
| openfl | @ `2e5fc2062fb07b40f81b45268085a2efdb2eb9ae` |
| flixel | @ `141f23c400c0508c76d5a09a143f5ce6790f8122` |
| C++ standard | `HXCPP_CXX_STANDARD=20` → MSVC `/std:c++20` (verified: fork honors it via `BuildTool.hx` env→defines→`Compiler.hx` `/std:c++N`) |
| Compiler | MSVC 14.44 (VS2022 BuildTools), x64 |
| Workspace | `F:\vslice-lime-build` (haxelib repo: `F:\vslice-lime-build\.haxelib`) |
| Build flag | `-DNO_FEATURE_VIDEO_PLAYBACK` (see §4.3) |

Haxelib repo was switched with `haxelib setup F:/vslice-lime-build/.haxelib`.
`HAXE_STD_PATH=C:/haxe/haxe_20250509143529_e0b355c/std` must be exported — lime
hardcodes `C:\HaxeToolkit\haxe\std\` on Windows when unset.

## 2. Stage results

| Stage | Project | Result |
|---|---|---|
| 1 | `fw-tests/lime-min` | ✅ builds + runs under C++20 (3 frames, clean exit) |
| 2 | `fw-tests/openfl-min` | ✅ builds + runs (sprite on stage, 5 frames, exit 0) |
| 3 | `fw-tests/flixel-min` | ✅ builds + runs (FlxState created, 30 frames, exit 0) |
| 4 | **V-Slice 0.8.6 (`project.hxp`)** | ✅ **builds + launches** |

### Stage 4 numbers

- Generated C++ groups: `haxe` **3904 files**, `__resources__` **189 files**, `__main__`, `runtime` (25), `rc`, `hxcpp_regexp`, `pcre2-8`/`pcre2-16` (27 each), `hxcpp_std` (5), `hxcpp_ssl`, `mbedtls` (96), `external-windows`, `hxcpp_zlib_impl`, `zlib_sources` (15), `hxcpp_zip_api`, `discord-rpc` (5)
- **~4,200 translation units, zero C++ errors**
- Warnings: **10 total, all `C5232`** in `VirtualArray.h` (known harmless C++20 recursive-comparison warning, already documented in Phase 2/3)
- `Funkin.exe`: **72,984,064 bytes (~69.6 MiB)**
- Object dir: ~3.9 GB; incremental relink ~2m45s (clean full build ≈ 35–45 min)
- Runtime: exe launches, runs `FunkinPreloader` end-to-end (DownloadingAssets → PreloadingPlayAssets → InitializingScripts → CachingGraphics/Audio/Data → ParsingSpritesheets → ParsingStages) before timeout kill. **Gameplay NOT verified** — assets are stubs (see §4).

## 3. C++20 source fixes required (hxcpp fork, 2 total)

Both are minimal, semantics-preserving, and confined to the pinned fork's runtime:

1. **`src/hx/libs/std/Sys.cpp:102`** (Phase 2 carry-over): `cond ? String : ""` →
   `cond ? String : String("")` — C++20 conditional-operator ambiguity.
2. **`include/hxcpp.h`** (NEW this phase): added `#undef min` / `#undef max` after
   `#define NOMINMAX`. Root cause: several runtime TUs (e.g. `src/hx/libs/ssl/SSL.cpp`)
   include `winsock2.h`/`windows.h` **before** `hxcpp.h`, so `min`/`max` function-like
   macros are already defined when `NOMINMAX` is processed. Under C++14 MSVC tolerated
   `std::numeric_limits<T>::max()` expanding through the broken macro (warning C4003
   only); under `/std:c++20` the empty-argument expansion becomes a hard error
   (C2589/C3878/C2760 cascade in `Array.h:119`). Verified by direct `cl` repro:
   `/std:c++14` compiles with warnings, `/std:c++20` fails pre-fix, both pass post-fix.
   No bare `min(`/`max(` macro uses exist in `src/hx`, so the undef is safe.

No generated-file patches were needed. No warning suppression was added.

## 4. Non-code blockers hit and resolved (build-environment only)

| # | Blocker | Resolution |
|---|---|---|
| 4.1 | `lime.ndll` missing — pinned lime git is **newer than haxelib 8.3.2** (`lime_bytes_write_file` primitive absent) | Ran `haxelib run lime rebuild cpp` — built `lime.ndll`/`lime-debug.ndll` from pinned source (all native deps: cairo, harfbuzz, sdl, sdl_image, freetype, openal, ogg/vorbis/opus, zlib, pixman, lzma, efsw, dr_libs, glad — **all compile clean**) |
| 4.2 | Lime native submodules empty/partial after disk-full clone | `git submodule update --init --recursive` at pinned commits; manually repaired `ogg`, `lzma`, `opusfile` worktrees (`git checkout HEAD -- .`) |
| 4.3 | `hxvlc` needs `project/vlc/lib/Windows` DLLs (not in git install) | Built with `-DNO_FEATURE_VIDEO_PLAYBACK` (FeatureFlag inverse `NO_` prefix). **Video playback untested.** |
| 4.4 | `project.hxp` requires git repo (branch + HEAD) | `git init` + empty commit in the F: working copy (original C: source untouched) |
| 4.5 | `HAXE_STD_PATH` unset → lime defaults to nonexistent `C:\HaxeToolkit\haxe\std\` | Export correct std path per build |
| 4.6 | Missing asset dirs → `Could not find asset path` | Created skeleton `assets/{preload,fonts,songs,shared,videos,tutorial,week1-7,weekend1,sserafim}` + registry data dirs (`assets/preload/data/{songs,stages,players,levels,notestyles,stickerpacks,dialogue/*,ui/freeplay/*}`) |
| 4.7 | `@:build(ComponentMacros/ComponentBuilder.build(...))` needs real haxeui XML | Downloaded all 43 files under `exclude/data/ui/` + `exclude/data/credits.json` + `art/icons/*.png` from `FunkinCrew/Funkin.assets` / `Funkin.art` `main` branches (small text/UI files only — **no song/audio assets**) |
| 4.8 | `ApplicationMain.rc` → missing `ApplicationMain.ico` | Same icon PNGs from Funkin.art |

## 5. What was NOT exercised

- **Gameplay correctness** — songs, sprites, audio, charts are absent (empty asset
  skeleton). The exe proves compile+link+engine-init, not playability.
- **`Reflect.hasField` divergence** — Phase 3.5 shim exists (`Compat.hasField`) but was
  not needed for compilation; V-Slice reflection sites target anonymous structures
  which already work on hxcpp.
- **FEATURE_VIDEO_PLAYBACK / VIDEO_SUBTITLES** — disabled (hxvlc native VLC libs absent).
- **Mobile/IAP/ads features** — already disabled by platform defaults.
- **`main` branch asset drift** — UI XMLs/icons fetched from `main`; the true 0.8.6
  submodule pin may differ slightly. Compile succeeded, so API surface matched.

## 6. Reproduce

```bat
set PATH=C:\haxe\haxe_20250509143529_e0b355c;%PATH%
set HAXE_STD_PATH=C:/haxe/haxe_20250509143529_e0b355c/std
set HXCPP_CXX_STANDARD=20
cd /d F:\vslice-lime-build
haxelib run lime build windows -DNO_FEATURE_VIDEO_PLAYBACK
```

Logs: `F:\vslice-lime-build\logs\` (`vslice-build.log`, `lime-rebuild.log`,
`lime-min-build*.log`, `openfl-min-build.log`, `flixel-min-build.log`).

## 7. Recommendation

**A — continue with the hxcpp-backed C++20 engine as the intermediate implementation.**

The entire real dependency stack (lime + openfl + flixel + haxeui + polymod/hscript +
discord-rpc + astc + ~50 haxelibs) compiles under `/std:c++20` with only two small,
documented runtime patches. The remaining work is asset completion and runtime
behavior verification — not toolchain fighting. Phase 5 (incremental native
migration) can now proceed subsystem-by-subsystem against a buildable baseline,
keeping hxcpp as the reference backend per the original plan.

Native-runtime replacement (option B/C) remains a Phase 6 concern; Immix GC is the
known hard dependency (see Phase 3.5 report).

## 8. Issue list

**Resolved:** lime.ndll ABI mismatch, submodule completeness, HAXE_STD_PATH,
git-repo requirement, asset skeleton, haxeui XML markup, credits.json, icons,
video-playback native dep, `min`/`max` macro C++20 break, Sys.cpp ternary.

**Open / documented limitations:**
- `Reflect.hasField` hxcpp divergence — shim available, unexercised by build.
- `Context.getBuildFields()` outside `@:build` — std API contract, not a bug.
- `VirtualArray.h` C5232 warnings (10×) — harmless, tracked separately from errors.
- Video playback disabled pending VLC binary deps.
- Real gameplay/assets not present — separate content problem, not a toolchain one.
- `main`-branch UI assets may drift from the 0.8.6 submodule pin.
- `.haxelib` inside workspace is ~2 GB+; sdl_image recursive externals partially
  cloned (unused by lime's files.xml — compile proved it).

**Files modified (all outside original V-Slice source):**
- `F:\vslice-lime-build\.haxelib\hxcpp\git\src\hx\libs\std\Sys.cpp` (1 line)
- `F:\vslice-lime-build\.haxelib\hxcpp\git\include\hxcpp.h` (+7 lines)
- `F:\vslice-lime-build\.haxelib\lime\git\ndll\Windows64\*` (built artifacts)
- `F:\vslice-lime-build\assets\`, `art\` (asset skeleton + fetched UI/icon files)
- `F:\vslice-lime-build\.git` (empty snapshot commit for project.hxp)
- `F:\vslice-lime-build\fw-tests\*` (minimal projects)
- Original `C:\...\Funkin-0.8.6` tree: **untouched**.

---

## 9. R-Slice splash screen (added 2026-11-18)

`RaidSplashState` (`source/funkin/ui/RaidSplashState.hx`) replaces the stock
startup beat with the R-Slice boot animation. It runs **after** `setupShit()`
(preloader + asset caching) and **before** `TitleState`.

### Lifecycle

```
Main.hx (skipSplash=true → no FlxIntroSplash)
  → InitState.create → setupShit() → FunkinPreloader (cache)
  → startGame() → startGameNormally()
      ├─ #if SONG/LEVEL/FREEPLAY/DIALOGUE/CHARTING/STAGE_EDITOR/... → debug direct-start (splash bypassed)
      ├─ #if RAID && (RAID_CHART|RAID_PLAYSTATE) → RaidDebugState (splash bypassed)
      └─ else → FlxG.switchState(() -> new RaidSplashState())   ← every terminal branch
            → plays raidSplash{0..3} atlases (106 frames @ 24 fps, cover-fit, centered)
            → plays music/raidSplash/raidSplash.ogg
            → ENTER / SPACE / ESC / click skips; end-of-sheet-4 or asset failure → TitleState
```

`FlxG.switchState` is deferred: `state.startOutro(cb)` fires synchronously on
`InitState` (no outro override), setting `FlxGame._nextState`, consumed on the
next `FlxGame.update` — old state destroyed first, then `create()` runs. Splash
therefore never shares a frame with init.

### Assets

- `assets/preload/images/raidSplash{0..3}.png/.xml` — four Sparrow atlases,
  frames `f0000`–`f0105` (28/28/28/22), 640×360 cells, cover-scaled ×2.
- `assets/preload/music/raidSplash/raidSplash.ogg` — extracted startup audio.
- Source: `assets/videos/videos/startup.mp4` pre-rendered via ffmpeg
  (`eq=brightness=0.3:contrast=1.35:saturation=1.5` — the source video is very dark).

### Failure handling

Missing atlas/frame → that sheet is skipped; zero playable sheets → straight to
`TitleState`. Audio failure is caught and logged. `leaving` flag + `sfx.stop()`
+ `FlxG.sound.destroy()` on state switch prevent leaks/duplicate transitions.

### Approach rationale

Custom `FlxState` is correct: the splash must appear **after** preloading
(`FlxBasePreloader` would run *during* load — wrong phase), and `FlxSplash`/
`FlxIntroSplash` is the engine's own pre-init branding (`skipSplash=true`
already disables it). Four sprites + manual sequencing is required because one
`FlxAnimation` cannot span multiple `FlxAtlasFrames`.

### Verified (Windows, `-DNO_FEATURE_VIDEO_PLAYBACK -DRAID`)

- Boot → splash → title, exactly once; no RAIDDBG/diagnostic output remains.
- All 4 atlases load (28/28/28/22 frames), animation advances sheet 0→3,
  audio plays and finishes with the video, skip inputs wired.
- Debug defines bypass the splash; JSON/RAID chart paths untouched.

### Remaining limitation

True `startup.mp4` playback needs `FEATURE_VIDEO_PLAYBACK` (hxvlc), which is
blocked on missing `project/vlc/lib/Windows` libVLC binaries in the local
haxelib. The sprite-sheet path is the shipping fallback and works on all
targets.
