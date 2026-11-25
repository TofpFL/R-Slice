# IChartSource — dual playback engines, one gameplay layer

Status: **partially implemented** (steps 1–3 + asset resolver + `.rvfx`
sidecars done and verified — see §11). This document defines how R-Slice
(and eventually V-Slice) run **two playback engines behind one gameplay layer**:

- **JSON mode** — V-Slice's existing chart loading, note spawning, and event
  processing, kept completely intact.
- **RAID mode** — R-Slice's high-throughput streaming pipeline: windowed
  decoding, bounded memory, millions-of-events-per-second delivery.
- **Shared layer** — judgement, scoring, health, receptors, animations, and all
  gameplay presentation. Written once, format- and engine-agnostic.

The key rule: **RAID does not imitate JSON playback.** Each engine does what it
was built for. Format is detected at load time; the appropriate backend is
selected; neither gameplay nor presentation ever branches on chart format.

## 1. Goals

- **Dual-format support as a core requirement.** JSON-family charts (legacy/Psych,
  V-Slice `playData`) and binary `.raid` charts are first-class peers.
- **Each engine keeps its native strengths.** JSON mode reuses V-Slice's proven
  pipeline untouched. RAID mode keeps `ChartStream`/`PlaybackFeed` windowed
  decode — no full materialization, ever.
- **One gameplay layer.** Judgement, scoring, health, combo, receptors,
  characters, stage, camera, and rendering are shared. No per-format gameplay
  code.
- **Chart selection by metadata/extension.** The loader picks a backend from the
  file (magic bytes / extension / manifest), not from gameplay code.
- **V-Slice integration path.** The same boundary contract, expressed as a C ABI,
  is what a Haxe `cpp` extern binding calls — so RAID charts can play inside the
  V-Slice engine without V-Slice's chart pipeline dictating RAID's architecture.

## 2. Current state (what already exists)

| Piece | File | Role today |
|---|---|---|
| `gh::Chart` | `src/chart.hpp` | Materialized chart model: `notes`, `tempo`, `sections`, `events`, `visuals`, `modchart` |
| `gh::Note` | `src/chart.hpp` | One note: `time` (ms), `length` (ms), `lane`, `side`, `kind`, `flags`, `type`, `stack` |
| `psych::ChartEvent` | `src/psych.hpp` | Non-note chart event (camera, character, stage, etc.) |
| `parseChart` / `loadChartFile` / `streamChartSkeleton` | `src/chart.hpp`, `chart_stream.hpp` | JSON-family parsing; skeleton+stream split for big files |
| `gh::raid::Reader` | `src/raid.hpp` | `.raid` chunk reader: `range()`, LRU cache, prefetch, memory budget |
| `gh::raid::ChartStream` | `src/raid_chart.hpp` | `.raid` → `Chart` metadata + windowed `pump()` of `Event`s |
| `gh::raid::PlaybackFeed` | `src/raid_chart.hpp` | Playback coordinator: feed cursors, lookahead/prefetch policy, `Event`→`LaneNote` conversion, `Session` GC |
| `Session` | `src/session.hpp` | Note lifecycle owner (spawned/struck/resolved notes) |
| `PlayState::pumpNotes` | `src/state_play.cpp` | Per-frame `.raid` pump entry point |

The gap: `Chart`-materialized playback (JSON path) and `PlaybackFeed` streaming
playback (`.raid` path) are two parallel paths inside `PlayState`. They converge
only at `Session`/judgement — which is exactly the shared layer we want. The
refactor formalizes that convergence behind an interface instead of `if (isRaid)`
branches scattered through playstate code.

## 3. Architecture

```
                     ┌─────────────────────────────────────────┐
                     │            Gameplay Layer                │
                     │  Session · judgement · scoring · health  │
                     │  receptors · characters · stage · render │
                     └───────────────▲───────────────▲─────────┘
                                     │               │
                    gameplay-ready    │               │   gameplay-ready
                    notes + events    │               │   notes + events
                                     │               │
              ┌──────────────────────┴───┐   ┌───────┴──────────────────┐
              │   JSON Playback Engine    │   │   RAID Playback Engine    │
              │  V-Slice pipeline, intact │   │  ChartStream +            │
              │  load → spawn → events    │   │  PlaybackFeed, windowed   │
              └────────────▲─────────────┘   └──────────────▲───────────┘
                           │                                │
              JSON-family charts                    .raid
              (fnf, psych, vslice playData)        (bounded-RAM stream)
```

### The contract

Each engine is a **playback strategy**, not a chart parser. It owns everything
from "file on disk" to "notes/events entering the gameplay layer" — decode,
scheduling, lookahead, its own memory policy. The gameplay layer only ever sees
the output.

```cpp
namespace gh {

enum class PlaybackEngine : uint8_t {
    kJson,    // V-Slice pipeline: materialized/skeleton JSON charts
    kRaid,    // R-Slice pipeline: windowed .raid streaming
};

struct EngineInfo {
    PlaybackEngine engine;
    std::string format;          // "fnf-json", "psych-v1", "vslice", "raid", ...
    uint64_t logicalEvents;      // total note+event count if known, else 0
    uint64_t rawBytes;           // file size on disk
    bool seekable;
};

// Delivery contract — verified against state_play.cpp's actual touchpoints:
//
//   * Notes enter the gameplay layer THROUGH Session. PlaybackFeed::advance()
//     already writes LaneNotes into Session directly; JSON charts fill
//     Session at init() from materialized chart_.notes. The engine contract
//     mirrors this: `advance()` feeds Session, it does not return note
//     arrays. (state_play.cpp:636 is the only .raid pump call site.)
//   * Non-note chart content (psych::ChartEvent camera/character/stage,
//     visuals.json clips, modchart tracks) is NOT pumped — it lives on
//     Chart meta (chart_.events/.visuals/.modchart), merged at prepare time
//     for BOTH formats (library.cpp:163-189), and is read directly by the
//     gameplay layer. It stays a metadata channel, not a poll channel.
//   * Timing queries (bpmAt/beatAt/sectionAt/focusAt/keys/lengthMs) are
//     Chart-meta reads available identically for both engines.
//
// GameplayEvent remains the C-ABI boundary shape (§6) for the V-Slice side,
// where Haxe cannot hold a Session — there, poll() materializes events into
// a caller buffer instead.
struct GameplayEvent {
    double timeMs;
    EventKind kind;              // Note | Camera | Character | Stage | Health | Marker | ...
    int lane;                    // 0..keys-1, -1 for non-lane
    int side;                    // 0 opponent, 1 player, 2 gf
    // note payload
    float lengthMs;
    uint8_t noteKind;
    uint8_t noteFlags;
    uint16_t noteType;           // index into chart meta noteTypes
    uint16_t stack;
    // event payload
    int64_t aux;
    uint64_t id;                 // stable per-chart id (raid: global expanded index)
};

class IPlaybackEngine {
public:
    virtual ~IPlaybackEngine() = default;

    // Cheap open: header/metadata only. Fills `meta` with title, difficulty,
    // keys, tempo map, sections, note types, lengthMs, note totals, and merges
    // side-car tracks (events/visuals/modchart) exactly as library.cpp does
    // today. MUST NOT decode the full chart (RAID) or block on full parse
    // where avoidable (huge JSON may skeleton-parse).
    virtual bool open(const std::string& path, Chart& meta, std::string& err) = 0;

    virtual EngineInfo info() const = 0;

    // Called once per frame after Session::init(). Feeds due notes/events
    // into `session` up to the lookahead horizon, then GCs resolved notes.
    // `nowMs` is song time from the audio clock — passed IN, never owned by
    // the engine. Idempotent per nowMs.
    //
    // JSON engine: nearly a no-op — Session was filled at init() from
    // materialized notes; advance() only handles seek-back edge cases.
    // RAID engine: wraps PlaybackFeed::advance() verbatim (windowed pump,
    // lookahead/prefetch policy, Event->LaneNote conversion, session GC).
    virtual bool advance(double nowMs, Session& session, int keys,
                         bool mirrorSides, std::string& err) = 0;

    // Optional: drain non-note gameplay events due in (lastPoll, nowMs] —
    // used by the C ABI / V-Slice side where there is no Session. The
    // native gameplay layer does NOT need this (chart_.events/.visuals/
    // .modchart are read directly off Chart meta).
    virtual size_t pollEvents(GameplayEvent* buffer, size_t capacity,
                              double nowMs) { (void)buffer; (void)capacity; (void)nowMs; return 0; }

    // Rewind for restart / practice / editor scrub. False = unseekable live
    // stream. After success, the next advance(now) resumes from `t`.
    virtual bool seek(double tMs) = 0;

    // Optional decode warm-up for [tMs, tMs+horizon). RAID maps to
    // PlaybackFeed's 16 s prefetch; JSON may no-op.
    virtual void prefetch(double tMs, double horizonMs) {}

    // Optional cache drop (memory pressure / song end).
    virtual void releaseCache() {}
};

}  // namespace gh
```

### Time model

- **`nowMs` is milliseconds, matching `Chart`/`Note::time`.** `.raid` uses
  `int64_t` µs internally — conversion stays inside the RAID engine.
- **The engine is not the clock.** Song time comes from the audio engine
  (Conductor-equivalent). Passing `now` in keeps engines pure functions of
  `(position, time)`, makes seek trivial, and prevents clock drift — critical
  when the clock lives in Haxe-land on the V-Slice side.

## 4. The two engines

### JSON Playback Engine (`kJson`)

Wraps V-Slice's existing pipeline **completely intact** — this is an adapter,
not a rewrite:

- `open()` → existing `loadChartFile` / `parseChart` (or `streamChartSkeleton`
  for huge JSON — that path already exists and stays).
- `pollEvents()` → drains the materialized `chart_.notes` + `chart_.events` +
  `chart_.visuals` + `chart_.modchart` tracks, merged by time, converted to
  `GameplayEvent`. This is what `PlayState` already iterates today.
- `seek()` → cursor reset over materialized arrays (trivial).
- **No behavior change.** Stock V-Slice charts play exactly as they do now.
- Inside V-Slice proper, this engine *is* V-Slice's own `Conductor`/chart
  pipeline — the Haxe-side `IPlaybackEngine` implementation just exposes its
  output through the C ABI (§6).

### RAID Playback Engine (`kRaid`)

Wraps `raid::ChartStream` + `raid::PlaybackFeed` **as-is**:

- `open()` → `ChartStream::open()` — metadata only, `chart_.notes` stays empty.
- `pollEvents()` → `PlaybackFeed::advance(nowMs, session, ...)` drain, or a
  direct `ChartStream::pump()` window if `Session` ownership stays in the
  gameplay layer (see §5). Events convert `raid::Event` → `LaneNote` →
  `GameplayEvent` exactly as `PlaybackFeed` already does (mirror flips side,
  lanes ≥ keys dropped, stack ≥ 1, `id` = global expanded index).
- `seek()` → `PlaybackFeed::reset()` + `ChartStream::resetPrefetch()`.
- `prefetch()` → existing 16 s lookahead / worker-thread decode warming.
- **Bounded memory preserved**: `pollEvents` returns ≤ `capacity`; internal
  windows stay at `kLookaheadMs`/`kPrefetchMs` horizons. The engine never sees
  the whole chart. This is the non-negotiable rule.

### Streamed-JSON variant

Huge JSON charts that today use `streamChartSkeleton` are the `kJson` engine
with streaming internals — same contract, same bounded-window decode discipline
as RAID where the existing code already provides it. No third engine needed.

## 5. What the gameplay layer owns (unchanged)

`PlayState` (and its V-Slice counterpart) keeps:

- Audio clock / song position (`nowMs`)
- `Session` note lifecycle (spawned → struck/missed → resolved)
- Judgement, scoring, health, combo
- Note sprites, receptors, characters, stage, camera
- Chart-meta side channels: `chart_.events` (psych events → scripts +
  builtin handlers), `chart_.visuals`, `chart_.modchart`, timing queries —
  read directly, identical for both engines
- Dispatch of engine-emitted `GameplayEvent`s where a backend produces them
  (C ABI path; native side channels stay on Chart meta)

`PlayState` stops calling `loadChartFile`/`PlaybackFeed::advance` directly and
holds an `IPlaybackEngine*`. `pumpNotes()` becomes
`engine->advance(songMs_, session_, chart_.keys, streamMirror_, err)` —
literally today's `feed_.advance` call with `feed_` behind the interface.
An `if (engine == kRaid)` branch survives only where behavior *genuinely*
differs (debug overlays, pattern logging, `noteOrder_` script ids, feed
stats) — never in judgement or rendering.

**Session ownership — RESOLVED by the touchpoint audit:** engines feed
`Session` directly inside `advance()`, exactly as `PlaybackFeed` does today.
This is neither pure-(a) nor pure-(b): the gameplay layer owns the `Session`
object and its lifecycle (init/judgement/GC queries), while the engine owns
the *feeding* (decode → convert → append → resolved-note GC). That is
precisely the current `PlaybackFeed`/`Session` split, so step 2 is a wrapper,
not a re-plumb.

## 6. C ABI for the V-Slice boundary

Haxe can't use C++ classes/virtuals. The contract is exported flat; both engines
implement it (JSON engine on the Haxe side wraps V-Slice's pipeline; RAID engine
is native):

```c
typedef struct GhPlaybackEngine GhPlaybackEngine;   // opaque

typedef struct GhGameplayEvent {
    double timeMs;
    int32_t kind, lane, side;
    float lengthMs;
    uint8_t noteKind, noteFlags;
    uint16_t noteType, stack;
    int64_t aux;
    uint64_t id;
} GhGameplayEvent;

typedef struct GhEngineInfo {
    int32_t engine;          // 0 json, 1 raid
    char format[32];
    uint64_t logicalEvents, rawBytes;
    int32_t seekable;
} GhEngineInfo;

// Backend selection by file: magic bytes / extension decide the engine.
GhPlaybackEngine* gh_engine_open(const char* path, char* err, size_t errLen);
void gh_engine_close(GhPlaybackEngine* e);
int32_t gh_engine_info(GhPlaybackEngine* e, GhEngineInfo* out);
size_t gh_engine_poll(GhPlaybackEngine* e, GhGameplayEvent* buf, size_t cap,
                      double nowMs);
int32_t gh_engine_seek(GhPlaybackEngine* e, double tMs);
void gh_engine_prefetch(GhPlaybackEngine* e, double tMs, double horizonMs);
```

Boundary rules:

- **No pointers into Haxe objects cross the boundary.** Events are POD structs
  in a caller-owned buffer. Handles/IDs only.
- hxcpp's Immix GC is a *moving* collector — native code must never retain an
  `hx::Object*` across frames. The C ABI sidesteps this entirely.
- `poll` is pull-model: Haxe calls once per update tick, drains the buffer,
  converts `GhGameplayEvent` → V-Slice sprites/session entries. No
  C++→Haxe callbacks, no per-note crossings.

## 7. The asset layer — `.RCHR` and loose files

`.RCHR` is an **asset container, not a gameplay format**. It is fully independent
of the playback engines: either engine can draw characters/sprites from a `.rchr`
or from loose files interchangeably. The playback layer never knows which.

### Two asset backends, one resolver

```
Character / sprite request  ("characters/bob.json", "images/bob.png", ...)
              │
              ▼
   ┌─────────────────────┐
   │  AssetResolver      │  ← the only thing gameplay code calls
   │  (unified resolver) │
   └─────────┬───────────┘
             │
   ┌─────────┴──────────┐
   ▼                    ▼
LooseFsSource      RchrSource
(regular way)      (.RCHR archive)
   │                    │
 files on disk      mountDir() → cache dir
```

- **`LooseFsSource`** — the regular way: files on disk under the content root,
  exactly as today. Always available; zero overhead; the default.
- **`RchrSource`** — wraps the existing `gh::rchr` archive (`src/rchr.hpp`).
  The archive already provides: versioned header (`RCHR`, u16 version),
  CRC32-verified entries, store-or-deflate payloads, and `mountDir()` which
  extracts once into `<cacheRoot>/<crc32-hex>/` and returns a directory that
  every existing character/stage/script lookup can read unchanged.

### `IAssetSource` contract

```cpp
class IAssetSource {
public:
    virtual ~IAssetSource() = default;

    // Resolve an install-relative path ("characters/bob.json") to readable
    // bytes or a filesystem path the existing loaders can consume.
    // Returns false if the asset doesn't exist in this source.
    virtual bool resolve(const std::string& relPath,
                         std::string& outPathOrData, std::string& err) = 0;

    virtual bool exists(const std::string& relPath) const = 0;
    virtual void reload() {}        // hot reload: rebuild/re-extract
    virtual uint32_t version() const { return 0; }
};
```

`AssetResolver` tries sources in priority order (typically `.rchr` mounts first,
then loose files as fallback/override) and caches negative lookups.

### Responsibilities

| Concern | Owner | Status |
|---|---|---|
| `.RCHR` read/write, CRC, deflate, version | `gh::rchr` (`rchr.cpp`) | **exists** |
| Cache-mount extraction | `rchr::mountDir` | **exists** |
| Unified resolver + loose-files backend | `AssetResolver` / `LooseFsSource` | to build |
| Lazy loading | resolver loads on first `resolve()`, not at startup | to build |
| Texture caching | decode-once PNG→texture cache keyed by path+mtime/crc | to build |
| Hot reload | `reload()` → re-extract `.rchr` / re-stat loose files | to build |
| Validation | missing refs, malformed JSON/XML, bad texture dims | to build |
| Animation preservation | `.rchr` stores XML/JSON + PNGs verbatim under install-relative paths — already lossless | **exists** |

### Rules

1. **`.RCHR` stays an asset container.** No chart data, no gameplay logic —
   characters, sprites, scripts, icons, stage art only. Either playback engine
   consumes it identically.
2. **Loose files always work.** `.rchr` is additive — a mod/character pack can
   ship as either a directory or an archive; the resolver doesn't care.
3. **Extraction is cached, not repeated.** `mountDir` keys the cache by file
   CRC — rebuild the archive, get a new cache dir, old one is garbage-collected.
4. **Validation at pack time and load time.** `pack()` should reject archives
   with dangling references; `resolve()` reports missing assets with the
   requesting path so errors name the real culprit.
5. **Versioning is mandatory.** New `.rchr` revisions bump the u16 version;
   readers must reject unknown major versions, not silently misparse.
6. **`.raid` may embed `.rchr` packs** via the `RAIDASST` trailer
   (`src/raid.hpp`) — a self-contained chart+characters file. `AssetResolver`
   must treat embedded packs as just another `IAssetSource`, lowest or highest
   priority TBD.

### `.rvfx` — timed shader-effect scripts

**Implemented** (`src/rvfx.hpp/.cpp`, loaded in `library.cpp`): a `.rvfx` file
is a short line-based script scheduling shader clips at song times. Each line
compiles to one `VisualClip`, so the existing `visuals.json` runtime
(`play_visuals.cpp`) drives it unchanged — `.rvfx` is a *format*, not a new
pipeline.

```
# <startMs> [endMs|-] [@target] <fx> [k=v ...]
1200 glitch
3500 - @both shaders/pulse.frag uSpeed=2.5 uStrength=0.8
```

- `startMs` song ms (required); `endMs` omitted/`-` = until song end;
  `@target` = `game|hud|both` (default `game`); `fx` resolves through the same
  rules as `visuals.json` (built-in `gh::fx` name or `shaders/<name>.frag` in
  the asset roots — loose files or `.rchr` mounts alike); `k=v` float uniforms.
- `#` comments anywhere; parse errors name file:line and are non-fatal (the
  song still plays, the bad script is skipped — same policy as `visuals.json`).
- Lives beside the chart in the song folder (`*.rvfx` scanned in `dataDir` at
  prepare time). `.rvfx` inside `.rchr` packs is not scanned yet — the shader
  *files* resolve through mounted roots, but script discovery is dataDir-only
  (a possible follow-up).
- Presentation-only: never feeds judgement/scoring/health.

## 8. Critical design rules (non-negotiable)

1. **RAID never materializes to satisfy the interface.** Windowed decode only;
   enforced by keeping `ChartStream`/`PlaybackFeed` as the sole decode path.
2. **JSON mode is untouched.** V-Slice's chart loading, note spawning, and
   event processing keep working exactly as today — the engine adapter is a
   refactor of *who calls what*, not a rewrite of *what it does*.
3. **RAID doesn't imitate JSON.** The RAID engine emits through its own
   pipeline; normalization happens once at the `GameplayEvent` boundary, not
   by forcing RAID events into a JSON-note shape internally.
4. **One gameplay layer.** Judgement/scoring/health/receptors/rendering are
   written once and shared by both engines.
5. **Seek is explicit.** `seek(t)` works on every seekable engine
   (restart/practice/scrub); `seekable=false` is for true live streams.
6. **Time is passed in.** No engine owns the audio clock.
7. **Format selection is metadata-driven.** Extension/magic-bytes/manifest pick
   the engine at `open()`; gameplay never branches on format.
8. **Bounded buffers.** `pollEvents` returns ≤ `capacity`; callers own the
   buffer. No engine allocates unbounded event arrays per call.
9. **Assets are engine-agnostic.** `.RCHR` and loose files are consumed through
   `AssetResolver` only; playback engines and gameplay code never touch archive
   internals or hardcode asset paths differently per format.

## 9. Open questions (resolve before implementation)

- ~~**Session ownership**~~ — **RESOLVED** (§5): engines feed `Session` inside
  `advance()`, matching `PlaybackFeed` today. Gameplay layer owns the Session
  object; engine owns feeding.
- ~~**Modchart/visuals tracks**~~ — **RESOLVED** (§3): they stay on `Chart`
  meta as side channels merged at `open()`/prepare time; they do not ride
  `pollEvents`. `.rvfx` joins them as another side-car track.
- **Event payload breadth** — `GameplayEvent.aux: int64_t` vs. tagged union.
  Audit `psych::ChartEvent` (name/v1/v2 — small), `raid::Event` (note fields),
  and V-Slice `playData` event schemas to decide how much payload the shared
  layer actually needs. Only matters for the C ABI path.
- ~~**V-Slice JSON inside R-Slice**~~ — **RESOLVED**: `parseChart` already
  handles V-Slice `playData` natively; the ABI JSON engine uses it, with
  `*-metadata.json` sibling auto-discovery. Verified end-to-end (§11).
- **Difficulty/keys at open()** — partially resolved: the ABI falls back to
  the chart's first difficulty and auto-finds metadata; an explicit
  `(path, diff)` parameter is still needed before real gameplay use.

## 10. Implementation order (proposal)

1. **Extract `IPlaybackEngine` + JSON engine adapter** — pure refactor of the
   existing JSON path; `PlayState` holds `IPlaybackEngine*` instead of calling
   `loadChartFile`/iterating `chart_.notes` directly. No behavior change.
   *(Lowest risk, proves the seam.)*
2. **RAID engine adapter** — wrap `ChartStream`+`PlaybackFeed`; `pumpNotes()`
   becomes `engine->advance(...)`. Verify bounded memory + identical note
   delivery vs. today on a dense `.raid` chart.
3. **C ABI + Haxe binding** — export `gh_engine_*`, write the Haxe `cpp` extern
   shim, prove a `.raid` chart streams into the C++20 `Funkin.exe` via a debug
   state before touching V-Slice's `PlayState`.
4. **JSON engine inside V-Slice** — Haxe-side `IPlaybackEngine` wrapping
   V-Slice's own pipeline through the same C ABI. Enables mixed libraries
   (stock songs + RAID charts) in one V-Slice session.
5. **`AssetResolver` + `LooseFsSource`** — the missing half of the asset layer:
   unified resolve/exists/reload over `.rchr` mounts and loose files, with
   lazy loading, texture caching, and validation. `.rchr` read/write already
   exists in `rchr.cpp`.
6. **Shared-layer convergence** — once both engines feed `Session` through
   `advance()` on the native side (and `GhGameplayEvent` through the C ABI on
   the V-Slice side), the dual-engine architecture is complete and R-Slice's
   native gameplay layer can be retired or kept as the standalone player.

Each step is independently buildable and reversible. Steps 1–2 are pure
R-Slice refactors; step 3 is the first cross-engine work and should be gated on
the interop spike (compile RAID into the lime build, pull-model poll from Haxe).

## 11. Implementation status

**Steps 1–3: DONE and verified.**

- `src/playback_engine.hpp/.cpp` — `IPlaybackEngine` with `JsonPlaybackEngine`
  (no-op `advance`; `Session::init` already materializes) and
  `RaidPlaybackEngine` (wraps `ChartStream`+`PlaybackFeed` verbatim).
  `PlayState` owns `engine_`; `pumpNotes()` → `engine_->advance(...)`;
  `loopSong()` → `engine_->seek(...)`.
- `src/engine_abi.h/.cpp` — C ABI `gh_engine_open/close/info/poll/seek/prefetch`
  emitting POD `GhGameplayEvent`s. **Dual-format**: `.raid` streams via
  `ChartStream` (same slice/prefetch policy as `PlaybackFeed`); JSON-family
  charts (legacy/Psych/**V-Slice**/FLP) materialize via `loadChartFile` and emit
  in time order — V-Slice `-metadata.json` siblings are auto-discovered.
- V-Slice side (`F:\vslice-lime-build`, not the upstream repo):
  `source/funkin/external/raid/` — `RaidEngine.hx` externs +
  `project/Build.xml` (links `gh_core/gh_lua/gh_miniz.lib`, adds include
  paths) + `raid_bridge.cpp` shim (events via `rs_event_field` accessors — no
  native pointers cross into Haxe). `RaidDebugState` reachable with `-DRAID`;
  chart path via `RAID_CHART` env var.
- **Verified**: `gh_tests` 16202/0; `Funkin.exe -DRAID` streams `d10k.raid` at
  ~10K ev/s sustained (fed=240K by t=24s, stable ~384 MB) and `roses-chart.json`
  (V-Slice format + metadata) through the identical boundary. Immix GC safe:
  only an opaque `void*` handle crosses; events copy through a static buffer.

**Step 4: DONE and verified — real `PlayState` integration.**

- `GhEngineInfo` gained `bpm` (starting tempo via `Chart::bpmAt(0)`); exposed
  through `rs_engine_bpm` → `RaidEngine.bpm`.
- `source/funkin/external/raid/RaidChartStream.hx` — per-frame `pump()`
  polls the engine (4s lookahead, drains the 8192-event buffer in bounded
  rounds) and appends `SongNoteData` to the strumlines in time order via
  `addNoteData(note, false)`; `compact()` drops the consumed `noteData`
  prefix so resident data stays bounded on dense charts. Static
  `createSong(raidPath, donorId)` builds a playable `Song` via
  `Song.buildRaw` — the donor song id supplies audio/stage/characters,
  the raid chart supplies notes + tempo.
- `SongDifficulty.raidChartPath` marks streaming difficulties;
  `PlayState.regenNoteData` opens/seeks the stream instead of materializing
  notes, `PlayState.update` pumps before `super.update`, `performCleanup`
  closes the engine.
- `RaidDebugState`: ENTER (or `RAID_PLAYSTATE` env) launches real
  `PlayState`; `RAID_SONG_ID` picks the donor song (default `roses`).
- **Verified live**: `.raid` chart → `kind=1`, 20K logical events, real
  `NoteSprite`s spawning on both strumlines, hold-note judgement + score
  penalties firing, restart path re-seeking cleanly. JSON-family chart →
  `kind=0`, 313 events, same pipeline. Memory stayed bounded
  (`noteData` rolling window + sprite recycling).

**Known step-4 limitations:** gf notes (side=2) are dropped — V-Slice has
no third strumline; `noteKind` mines map to normal notes; mid-song tempo
changes are not yet mapped into `SongTimeChange`s (only starting bpm);
`.raid` camera/character/stage events aren't emitted by the ABI yet.

**Step 5: largely already existed — extended.** `scene::Files` **is** the
unified resolver (multi-root, case-insensitive, `..`-guarded, lazy dir
cache); `play_psych.cpp` already mounts `.rchr` archives into its roots and
`GraphicCache` is the texture cache. Added this pass: `Files::exists()`,
`Files::reload()`, `Files::version()` (hot reload), and `GraphicCache`
version-aware invalidation. Covered by `testFiles()` in `tests.cpp`.

**`.rvfx`: implemented.** `src/rvfx.hpp/.cpp` parse timed effect sidecars
(clip start/end, target, effect name, params), `library.cpp` discovers and
merges them into `chart.visuals`, tests + format doc included. Shader-folder
loading and `.frag` harvesting remain open.

**Still open:** `Aux` payload audit for non-note events, `.rvfx`
shader-folder loading, mid-song tempo-change mapping, gf/third-strumline
support, mine note kinds, `.raid` camera/character/stage event emission.
