# V-Slice → Native C++20: Phase 1 Audit & Pipeline Proposal

**Source audited:** `C:\Users\YalishannaZhou\Downloads\Funkin-0.8.6` (V-Slice 0.8.6, Haxe + HaxeFlixel + Lime/OpenFL). Read-only; nothing in the source tree was modified.

---

## 1. Codebase inventory

| Area | Files | ~LOC | Notes |
|---|---|---|---|
| `source/funkin/**` | ~640 | **~123,500** | All game code, `package funkin.*` |
| `funkin.ui` | 199 | 52.3k | Menus/states; ~35k is debug tooling (chart/stage editors) |
| `funkin.play` | ~65 | 21.7k | Rhythm gameplay; `PlayState.hx` alone = 4,137 lines |
| `funkin.util` | ~48 | 12.4k | Math/helpers + **compile-time macros** |
| `funkin.data` | ~48 | 9.1k | JSON registry/data layer + chart importers/migrators |
| `funkin.graphics` | ~39 | 4.6k | Sprite/camera/shader/framebuffer/video wrappers |
| `funkin.audio` | ~19 | 3.5k | Sound groups, streaming, FFT/waveform viz |
| `funkin.input` | 7 | ~3.0k | `Controls` (FlxActionSet), precise input, turbo |
| `funkin.modding` | ~22 | ~3.2k | Polymod loader + **hscript script VM glue** |
| `funkin.mobile` | ~17 | ~3.3k | `#if mobile` only — ignorable for desktop |
| `funkin.save` | 6 | ~2.1k | Save slots + data migrators |
| `funkin.api` | 7 | ~2.0k | Newgrounds + Discord RPC — leaf, platform-gated |
| `funkin.external` | ~16 | ~1.6k | JNI/Java/WinAPI externs — leaf |
| `funkin.effects`+`group` | 5 | ~1.2k | Tiny flixel helpers |
| top-level `funkin.*` | 9 | ~3.5k | `Conductor` (901), `InitState` (694), `Preferences` (664), `FunkinMemory`, `Paths`, `Assets`, `PlayerSettings`, `Highscore` |

**Entry point:** `source/Main.hx` → `FlxGame` → `funkin.InitState`. `assets/` is an unpopulated git submodule (irrelevant to code audit).

## 2. Build system & toolchain

- **Build:** Lime `project.hxp` (Haxe-scripted project file, not XML). Commands: `hmm install` → `lime test windows`.
- **Targets:** Windows/macOS/Linux/Android/iOS/HTML5 via `#if` flags; desktop is the relevant target.
- **Haxe deps:** pinned in `hmm.json` — **35 dependencies**, nearly all git-pinned to exact commits:
  - Engine stack: `lime`, `openfl`, `flixel`, `flixel-addons`, `flixel-animate` (git SHAs)
  - **`hxcpp` pinned to git `0376e9b`** — a FunkinCrew-patched fork, NOT stock hxcpp. CI (`build-game.yml`) compiles hxcpp's own `compile.hxml` from `.haxelib/hxcpp/git` before building.
  - Modding: `polymod` (git), which brings **hscript** (the script VM)
  - Data: `json2object`, `jsonpath`, `jsonpatch`, `thx.core`, `thx.semver`
  - Optional/platform: `haxeui-*`, `hxvlc`, `hxdiscord_rpc`, `newgrounds`, `funkin.vis`, `grig.audio`, `FlxPartialSound`, mobile extensions (admob/iap/iar/webview/agesignals/androidtools/haptics), `hxgamemode`, `astc-compressor`, `hxjsonast`, `dox`, `format`, `svg`, `tink_core`
- **C++ standard:** `project.hxp` sets `HXCPP_CHECK_POINTER`, `HXCPP_STACK_LINE`, `HXCPP_STACK_TRACE`, `hscriptPos`, telemetry/Tracy defines — **no `HXCPP_CXX_STANDARD` set**; generated code compiles at hxcpp's default (pre-C++20). Phase 2 must verify the pinned hxcpp accepts `HXCPP_CXX_STANDARD=20`.
- **Local toolchain:** none installed (no `haxe`/`haxelib`/`lime`, no `.haxelib`, no prior export output). Phase 2 starts from a clean toolchain install.
- **Existing C++:** only `source/cpp/vm/tracy/TracyProfiler.hx` + Tracy telemetry defines — no hand-written C++ engine code; the game is 100% Haxe above flixel/lime.

## 3. Major systems & dependency shape

Implicit coupling is the dominant feature: every file is `package funkin.*`, so `Conductor`, `Assets`, `Paths`, `Preferences`, `Save`, `Highscore`, `PlayerSettings`, `Constants` resolve **without imports** everywhere — they are the true hubs (~345 textual refs to `Conductor` alone). `import.hx` injects global `using` extensions (Lambda, StringTools, thx.Arrays, `funkin.util.tools.*`) — **no C++ equivalent; must become free functions or methods**.

**Hub → leaf ranking (inbound import sites):**

- **HUBS:** `funkin.data` (~403), `funkin.ui` (~310), `funkin.play` (~178), `funkin.util` (~225), `funkin.graphics` (~113), `funkin.modding` (~94 — every `Scripted*` class extends its bases), `funkin.audio` (~63); plus `flixel.*`/`FlxG` — the real coupling surface.
- **MID:** `funkin.input` (~30), `funkin.save` (~30)
- **LEAVES:** `funkin.effects`, `funkin.external`, `funkin.api`, `funkin.mobile` (`#if mobile`), `funkin.group`, `data/song/importer`+`migrator` (offline tools).

**Per-system conversion risk:**

| System | LOC | Haxe-risk | Verdict |
|---|---|---|---|
| `util` (minus `util/macro`) | ~12k | low-med | Good first target; macros must be dropped/codegen'd |
| `data` | ~9.1k | med | Mostly JSON→objects; needs JSON lib + registry rewrite; uses `Type`/`Reflect` (~120 sites total) |
| `graphics`/`audio`/`input`/`effects`/`group` | ~14k | med-high | Thin flixel wrappers — requires a flixel-subset port or abstraction layer |
| `play` | ~21.7k | high | Core gameplay; `PlayState` 4.1k-line god object; events/notes/scoring tightly coupled to flixel display list |
| `ui` | ~52.3k | high | Largest; ~35k is dev tooling (chart/stage editors) — **prime candidate to exclude/defer** |
| `modding` | ~3.2k | **very high** | hscript VM — no trivial C++ equivalent; needs embedded-script decision |
| `save`/`api`/`mobile`/`external`/top-level | ~11k | med | Singletons + platform gates; `Conductor` is the critical-path piece |

## 4. Haxe-runtime dependency catalog (quantified)

| Feature | Scale | Transpilation impact |
|---|---|---|
| `@:nullSafety` | 340 files | Mechanical → `std::optional`/references audit |
| `@:optional`, `@:default` | 233+161 | Argument defaults → C++ default args (mostly mechanical) |
| `@:privateAccess`, `@:access` | 117+133 | Access-control metadata — ignorable in native port |
| `@:hscriptClass` | **45** | Scripted-class stubs → polymod/hscript bridge; blocks naive conversion of every `Scripted*` class |
| `@:build`/macros | 43 + `util/macro/*` (12 files) | Compile-time codegen (registries, singletons, version strings) → replace with CMake/codegen or manual registries |
| `@:saveProperty` | 32 | Save-system property macros → explicit serialization |
| `@:native`, `@:jcustom*` etc. | ~80 | Platform externs — desktop-relevant subset is small |
| `dynamic`/`untyped`/`cast` | 12/8/few | Low count — good news |
| **Reflection** (`Type.getClass*`, `Reflect.field/setField`, `Type.ENUM/ClassType`) | **~120 call sites / ~34 files** | Concentrated in `data` registries, `SongEventRegistry/Schema`, `Save`, `modding`, debug editors — needs explicit registries/metadata |
| `#if` conditional compilation | ~700 sites, ~40 distinct flags | Map to CMake options; `mobile`/`html5`/`android`/`ios` branches deletable for desktop |
| `haxe.ds` (Map etc.) | 12 files | → `std::unordered_map` |
| `Null<T>` | **1,229 sites** | Ubiquitous — biggest mechanical burden; hxcpp `Null<T>` ≈ nullable ref |
| Exceptions (`throw`, try/catch) | 314 throws | C++ exceptions map OK; error-type conventions differ |
| Anonymous structures | ~358 | → structs/classes |
| `haxe.Timer`, `haxe.Json`, `EReg` | 3/5/13 files | Timer→`std::chrono`; Json→nlohmann/json or similar; EReg→`std::regex` |
| `using` extension methods | global (`import.hx`) | → free functions |
| **flixel/openfl/lime API surface** | hundreds of import sites | **The real dependency**: sprites, cameras, tweens, states, sound, input, text, filters, atlas frames, display list |

## 5. Dependency map — what converts independently

```
LEAVES (convert anytime, low risk)
  effects, external(desktop subset), api, mobile(skip), util(non-macro),
  data/importer+migrator (offline tools)
    │
MID (need flixel-subset shim first)
  graphics, audio, input, group, save
    │
HUB (order matters)
  data registries ──► play (needs Conductor+Song timing) ──► ui
    │
LATE/HARD
  modding (hscript VM decision), top-level singletons (Conductor last-ish)
```

**Critical path:** `Conductor` (music clock — everything times off it) and the flixel display-list abstraction are the two true blockers. Everything else is downstream of those two.

## 6. Phase 2 recon — hxcpp pipeline feasibility

- Pinned hxcpp is a **FunkinCrew fork** (`0376e9b`) — Phase 2 must clone that exact commit, not stock hxcpp.
- `HXCPP_CXX_STANDARD` is respected by modern hxcpp as an env/define; unverified against this pinned fork — first Phase 2 task.
- Expected incompatibilities (typical for hxcpp→C++20): `hxcpp.h`/`hxIMap` internals predate C++20; `Null<T>`/`Dynamic` codegen is verbose but compiles; biggest risks are **hxcpp runtime headers** (not game code) under `/std:c++20` `/Zc:` strictness, plus lime/openfl C++ interop bits.
- Debuggability: hxcpp embeds source paths; `-Dabsolute-paths` define already used for Tracy — reuse for source mapping.

## 7. Recommended pipeline architecture

```
V-Slice Haxe source (read-only reference)
        │
        ▼  haxe compile -cpp (lime/hxp build, pinned toolchain)
   hxcpp-generated C++  ──►  MSVC/Clang C++20  ──►  reference executable
        │                        (Phase 2-3: prove it builds & runs)
        ▼
   [Phase 4] native C++20 compatibility layer
        std::string/vector/unordered_map/optional/variant/function
        + explicit registries replacing Type/Reflect
        + flixel-subset abstraction (FunkinSprite→our renderer bridge)
        │
        ▼  per-subsystem ports, each independently buildable+testable
   native engine  ──►  R-Slice integration (Phase 7, selective reuse only)
```

## 8. Risks & open questions

1. **hscript/polymod** — modding is a product feature, not optional. Native port needs an embedded script decision (keep hscript via hxcpp shim? Lua? hscript-cpp?). Largest single risk.
2. **flixel display list** — sprites/states/tweens/cameras are the rendering model. Porting flixel's *behavior* (not just API) is the bulk of the work; consider a thin retained-mode scene graph in C++20 rather than pixel-perfect flixel semantics.
3. **`Null<T>` volume** (1,229 sites) — mechanical but pervasive; plan a transpiler-assisted pass or accept `T*`+null conventions.
4. **Reflection** (~120 sites) — contained enough for explicit registries; `SongEventSchema`/`RegistryMacro` are the templates to replicate.
5. **`PlayState.hx` (4.1k lines)** — do NOT port as-is; decompose during migration (same lesson as R-Slice's PlayState orchestrator rule).
6. **Debug tooling (~35k LOC)** — exclude from v1 native scope entirely.
7. **Toolchain weight** — lime/openfl/flixel/hxcpp git-pinned fork chain is fragile; pin everything, containerize the build (build/Dockerfile exists as a starting point).

## 9. Recommended next step (Phase 2 kickoff)

1. Install Haxe + haxelib + lime; `hmm install` against `hmm.json`.
2. Build hxcpp from pinned commit `0376e9b`; test `HXCPP_CXX_STANDARD=20` on a hello-world `.hxml` before touching the game.
3. `lime test windows -DNO_FEATURE_POLYMOD_MODS -DNO_FEATURE_DISCORD_RPC -DNO_FEATURE_NEWGROUNDS` minimal desktop build to shrink the dep surface.
4. Keep generated C++ under a `generated/` dir, never hand-edit; handwritten C++20 lives in a separate `native/` tree.
