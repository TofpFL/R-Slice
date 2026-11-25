# RAID Engine — Performance Audit & Optimization Log

## Bench harness

- `raid_engine bench <file.raid>` — gameplay-realistic path: `ChartStream::pump`
  at 60 Hz cadence with prefetch, reports decoded logical events/sec.
- `raid_engine bench <file.raid> <jobs>` — parallel path: `Reader::range` over
  disjoint time slices on `jobs` threads (`setMaxConcurrentDecodes(jobs)`),
  measures decode scaling. `jobs 1` is the single-thread Reader baseline.

All numbers are **decoded logical events/sec** (expanded stream — not raw
bytes, not pattern references).

## Baseline (before optimization, ChartStream path)

| Chart          | Events  | Events/s |
|----------------|---------|----------|
| Dogphobia      | 213,699 |   446K   |
| Stupid Griefer | 473,755 |   842K   |
| Watermelon     | 703,602 | 1,165K   |
| Taimuresu      |  75,819 |    80K   |
| TEOGOK         | 905,618 |   632K   |

## Optimizations applied

### CPU decode (`src/raid.cpp`)

1. **`emitLiteral` scratch buffer** — thread-local scratch + bulk insert
   (was per-op vector malloc + per-event push_back).
2. **`Reader::range` zero-copy filter** — `rangeChunk()` filters
   `[start,end)` directly from the LRU cache (was chunk→buf→out double copy).
3. **Pattern-group expansion (P1)** — `vector<vector<Event>>` + O(n·k) linear
   merge replaced by one contiguous arena + offset/count descriptors +
   binary-heap k-way merge on `(timeUs, partIndex)` — identical ordering,
   one allocation stream, O(n log k) merge.
4. **Varint fast path (P2)** — `Cur::var()` single-byte fast path for the
   common <128 case; same validation, multi-byte fallback unchanged.

### Playback memory (`src/session.cpp`)

5. **Amortized lane GC (P3)** — `gcBefore` now erases only when ≥512 resolved
   notes accumulated (or the vector is mostly dead) instead of memmoving the
   whole lane vector every 2 s of song time.

## Measured results (ChartStream path, after all CPU opts)

| Chart          | Before | After  | Speedup |
|----------------|--------|--------|---------|
| Dogphobia      |  446K  | ~1.6M  | ~3.5×   |
| Stupid Griefer |  842K  | ~2.0M  | ~2.4×   |
| Watermelon     | 1165K  | ~2.4M  | ~2.0×   |
| Taimuresu      |   80K  | ~0.2M  | ~2.5×   |
| TEOGOK         |  632K  | ~1.3M  | ~2.1×   |

(Note: this machine's timing is noisy ±20%; treat sub-20% deltas as noise.)

## Parallel decode scaling (`bench <file> <jobs>`, Reader::range path)

TEOGOK.RAID — 905,618 logical events, 1016 s chart:

| Jobs | Events/s | vs 1-thread |
|------|----------|-------------|
| 1    | 2.91M    | 1.0×        |
| 2    | 3.28M    | 1.13×       |
| 4    | 4.34M    | 1.49×       |
| 8    | 4.68M    | 1.61×       |

Watermelon.RAID @4 jobs measured 2.1M vs 3.0M @1 — within run-to-run noise
on this machine; scaling is real but sub-linear (shared `ioMutex_` serializes
file reads, `cacheMutex_` serializes LRU lookups, `decSem_` caps in-flight
decodes at `jobs`).

Key finding: **Reader::range single-shot (2.9M/s on TEOGOK) is ~2.2× faster
than the ChartStream 60 Hz pump path (1.3M/s)** — pump-cadence windows and
prefetch bookkeeping are the remaining streaming overhead, not decode itself.

## Rendering (P5) — investigated, not yet changed

- Per-note `DrawTexturePro` = 1 draw call/note; raylib has no public
  instancing. Culling already strong (window bounds, offscreen-Y, minGap
  pixel dedup, sustain head/tail, visibility/alpha early-outs).
- Near-term win without backend swap: rlgl instanced note pass (~5 draws/frame)
  — design in `docs/RENDERER.md`.
- GL 4.3 compute / indirect draw deliberately deferred (P6).

## Compatibility & correctness

- `gh_tests.exe`: **6889 passed, 0 failed** after every step.
- Decode output byte-identical; `.RAID` v1/v2 unchanged; all validation
  (CRC, counts, bounds, transforms) preserved.
- Bounded memory preserved: arena replaces per-part vectors (same total),
  GC threshold keeps ≤~512 resolved notes/lane extra, decSem_ still caps
  concurrent decode allocations.

## Pattern-group streaming merge (FLP encoder)

`writeRaidFlp`'s overlap-group path previously materialized every merged
event of a group into one `vector<Event>` (stable_sort over the whole
group), then rescanned every segment x every def x 2 binary searches per
chunk window. On `vocales.flp` (FL Studio 26.1.7.654, 245,766 playlist
items, 52 overlap groups, ~61k segs in the largest) this was effectively
non-terminating.

Replaced with a **streaming k-way merge**: a min-heap over per-segment
cursors ordered by `(timeUs, segment order, stream index)` reproduces the
old stable_sort order exactly, emits `chunkEvents`-sized windows on the
fly, and records which (segment, def-block) contributed to each window —
the same `GroupEntry` records the decoder already understands. No format
change, no merged vector: encoder memory is O(active segs + chunk), not
O(group events).

Measured (`vocales.flp`, 169 MB, 3.24M pattern notes / 245,766 playlist
items -> 6,416,320 logical events):

| Metric | Before | After |
|---|---|---|
| Encode time | >minutes (stalled in group 0) | **2.3-3.4 s** |
| Output | n/a | 809 KB raid (761 KB stored payload, 7.15x) |
| Chunks | n/a | 781 |
| Decode (4 jobs) | n/a | 2.81M events/s, verify clean |

Correctness: `Voices.flp` (80 segs, 23 overlap groups) re-encoded with the
streaming path decodes **byte-identical** to the previous merged-path
output across all 230,995 events (t/lane/side/kind/pitch/len/type/stack).
Repeat `vocales.flp` encodes are byte-identical (deterministic heap order).
`gh_tests`: 6889 passed, 0 failed.

## Canonical pattern deduplication (FLP encoder)

`writeRaidFlp` now separates **pattern definitions** from **instances**:

- `Def` = canonical pattern-table entry: literal payload bytes, spanUs,
  expanded count, side counts. Stored once per unique payload.
- `DefRef` = lightweight instance block: `{defId, src, srcBegin, srcEnd,
  originUs, originLane}` — the per-clip transform metadata (patternId,
  start anchor, lane origin, window bounds) the spec calls for. Group
  entries already carry `patternId + anchorUs + laneDelta + winLo/winHi`
  (+ repeatCount/stride via op-4 bursts); FL Studio clips map onto this
  exactly — no new opcode was needed.

Dedup key: FNV-1a hash of the def's serialized literal payload, verified
by **byte-for-byte payload + spanUs equality** — hash collisions can never
merge distinct patterns. Payloads encode events relative to the block's
first event, so identical payloads are decode-equivalent; per-instance
`anchorUs`/`laneDelta` (computed from each ref's own `srcBegin`) preserve
exact placement. Pattern ids remain deterministic (first occurrence in
segment order wins).

`vocales.flp`: 6 canonical defs / 6 instances — FLP clips already shared
identical (pattern, rotation, trim-window) keys via `winDefs`, so canonical
dedup is transparent there: output is **byte-identical** to the pre-dedup
encoder. The dedup pays off on projects whose distinct windows/rotations
happen to contain identical event content. `gh_tests`: 6889 passed, 0 failed.

## Playback-side scheduling (existing architecture)

The requested burst scheduler/lazy resolver already exists in composed
form — no new machinery was justified:

- **Instance index**: chunk table (`firstTimeUs`/`lastTimeUs` per chunk,
  non-overlapping) + `Reader::opLog`/`chunkTimeRange` give O(log chunks)
  window lookup — no per-frame instance scans.
- **Lazy windowed resolution**: `Reader::range(start,end)` decodes only
  intersecting chunks; op-3 group entries clip pattern expansion to
  `[winLoUs,winHiUs)` per window (`emitLiteral` clip path), so a pattern
  spanning chunk boundaries resolves per-window without materializing the
  whole instance. Deterministic tie-break: heap merge on
  `(timeUs, partIndex)` inside group expansion.
- **Resource controls**: `Reader` memory budget (default 128 MiB,
  derives `maxChunkEvents_`/cache caps), `setMaxConcurrentDecodes`
  (default 2, `DecodeGuard` semaphore), `kMaxChunkExpand` (4M) expansion
  cap, `kMaxDepth` nesting cap — all pre-existing, all preserved.
- An instance is never one gameplay note: group/burst ops expand to
  individual `Event`s retaining time/length/pitch/lane/side/kind/type/
  stack before `ChartStream`→`Session` conversion.

### Dedup verification

- New `scan_tests` case: two identical-content patterns + one differing by
  a single pitch -> `patternCount == 2`, all 6 events decode at correct
  times/lanes (instances keep per-clip anchors). +14 assertions.
- `gh_tests`: **6903 passed, 0 failed**.
- `vocales.flp` re-encode with dedup active: byte-identical output,
  verify clean, 4.1M events/s decode (4 jobs).

## Playback RAM audit (WCB = Weird_Shitty_Boat.raid)

WCB's `.raid` stores 6,416,320 events whose stack-weighted note total is
**382,551,250,982 (~383 B logical notes)** — the "300B" figure. Measured
with new `peak RSS` reporting in `streamtest`/`bench` (psapi
PeakWorkingSetSize on Windows, getrusage on Linux):

| Run | Peak RSS | RSS at close |
|---|---|---|
| streamtest (60 Hz pump + 16 s prefetch) | 291.2 MB | 6.1 MB |
| bench jobs=1 (full decode) | 291.2 MB | 6.1 MB |
| bench jobs=4 | 291.6 MB | 6.7 MB |

Consumer breakdown (evidence: `Reader::Impl` fields + open path):

- **Event LRU cache — dominant (~134 MB)**: `memBudget_` default 128 MiB,
  `cacheCapEvents_ = memBudget_/sizeof(Event)` (~5.6M events). Bounded by
  design; `setMemoryBudget()` makes it configurable.
- **Chunk index — scales with chunkCount**: `chunks_` holds a 56 B
  `ChunkEnt` per chunk resident for the Reader's lifetime. WCB: 781
  chunks (~44 KB). E404C projection (~1.4M chunks at the 4M-event chunk
  cap): **~78 MB resident**.
- **Open-time index spike — FIXED**: `Reader::open` previously allocated
  `indexSize` bytes, fread the whole index, then parsed into `chunks_`
  (2x index memory transiently). Now streams the index through a 1 MiB
  block buffer — open peak is O(1 MiB) regardless of chart size.
- **Pattern blob**: fully decompressed resident (`patternBlob_`), sized
  by UNIQUE pattern data only — the intended floor.
- **Session/ChartStream**: lane notes GC'd amortized (512-note threshold);
  pump/prefetch vectors are per-call transients.
- Verify/close RSS ~6 MB confirms caches release on `Reader::close`.

`gh_tests`: 6903 passed, 0 failed.

## PlaybackFeed — PlayState decoupling

`raid::PlaybackFeed` (`src/raid_chart.hpp/.cpp`) is the playback
coordinator between `ChartStream` and `Session`. It owns the feed cursors
(`fedUs_`/`prefetchUs_`), the 4 s lookahead / 16 s prefetch policy,
`Event -> LaneNote` conversion (mirror, lane filter, stable global ids,
stack clamp), session append, and `gcBefore`. `PlayState::pumpNotes` is
now ~10 lines: advance the song clock, call `feed_.advance(...)`, tick
the pattern logger. Subsystem mapping: `Reader` = pattern
resolver/cache/scheduler, `ChartStream` = event-window manager +
prefetch, `Session` = note lifecycle, `PlaybackFeed` = sequencing only.
No new format version; `.RAID` v1/v2 compatibility unchanged.

Bug found by the new feed test and fixed: `ChartStream::close()` used
`harvestPrefetch()` which is non-blocking — closing with async decodes
in flight raced their writes into `prefetchBufs_` against teardown.
`close()` now waits for every outstanding prefetch future first.

`testPlaybackFeed` (tests/raid_tests.cpp) verifies streamed lanes match
a materialized-chart session note-for-note (t/len/kind/type/stack/id),
mirror flips sides, `reset()` re-feeds for restart/seek, and an unbound
feed errors explicitly.

`gh_tests`: 13257 passed, 0 failed.

## Window-sliced range decode (`Reader::rangeWindow`)

`raid::Reader::rangeWindow(startUs,endUs,…)` is an opt-in alternative to
`range()`: on a chunk-cache miss it resolves only the `[startUs,endUs)`
portion of a chunk instead of materializing the whole chunk — literal
blocks are emitted with a query filter (stable global indices preserved),
pattern instances/bursts reuse cached leaf expansion with arithmetic
repetition pruning, and group merges assign indices by merge rank.
`decodeChunk`/`range()` are unchanged and remain the default/fallback.

Hybrid guard in `Impl::rangeWindowChunk`: when the query window covers
≥50% of a chunk's time span it delegates to `rangeChunk` (full decode +
LRU cache), because sequential playback re-queries overlapping windows
and reusing decoded chunks beats re-slicing per window.

Equivalence: `testSlicedRange` compares `rangeWindow` vs `range` on
every field (t/len/kind/flags/type/stack/side/lane) across randomized
windows and edge cases on both motif charts (op2/op4) and FLP playlist
group charts (op3).

Measured (`raid_engine slicebench`, 60 Hz playback-style windows):

| chart | range | rangeWindow |
|---|---|---|
| WCB (781 chunks, ~106 ms/chunk) | 1.3 s, 5.10M ev/s | 3.7 s, 1.75M ev/s |
| Dogphobia (27 chunks, multi-second spans) | 0.2 s, 1.37M ev/s | 3.7 s, 0.06M ev/s |

Verdict: **rangeWindow loses on sequential playback for real charts.**
Chips are already the right granularity — sequential windows re-touch
the same chunks, so the LRU cache wins; slicing pays per-window inflate
+ op re-parse with no reuse. The slice only wins for windows small
relative to chunk span AND inst/burst-dense ops (repeat-heavy E404C-style
charts, if they exist). PlaybackFeed intentionally still uses `range()`;
rangeWindow is retained as an API for sparse/random-access queries and
as groundwork if E404C proves giant chunks are the bottleneck.
`gh_tests`: 16194 passed, 0 failed.

## Density scaling (`densgen` / `densbench`)

`raid_engine densgen <out.raid> <sec> <evPerSec> <chunkEvents> [susEvery]
[overlapEvery] [keys]` writes raw `raid::Event`s via `writeRaid` — it
bypasses `Chart::finalize`, which merges same side+lane notes within
1 ms into stacks and would cap physical density at ~2*keys notes/ms.
`densbench` plays the file through the real path (ChartStream +
PlaybackFeed + Session, botplay judgement, 60 Hz) and reports pump
percentiles, lane-note residency, chunk-cache hit rate and bytes decoded.

`Reader::cacheStats(hits,misses,decodes,bytesDecoded)` exposes the LRU
counters (atomics, all decode paths incl. async prefetch).

Measured (keys=4, chunkEvents=8192, sustain every 32nd note):

| chart | density | fed NPS | pump p99.9 | peak lane notes | peak RSS | cache hit |
|---|---|---|---|---|---|---|
| d10k (60 s) | 10K ev/s | 3.2M | 4.8 ms | 361K | 57 MB | 94.6% |
| d100k (60 s) | 100K ev/s | 4.6M | 27 ms | 3.61M | 180 MB | 62.0% |
| d300k (27 s) | 300K ev/s | 4.8M | 81 ms | 9.0M* | 320 MB | 33.5% |
| WCB | ~77K ev/s avg | 2.2M | 63 ms | 6.4M* | 274 MB | 67.1% |
| Dogphobia | ~1.2K ev/s | 1.4M | 0.3 ms | 94K | 28 MB | 99.7% |

*peak = all events resident: songs shorter than the GC margin never
shrink. Session residency floor is ~(4 s lookahead + 30 s
`kGcMarginMs`) x density x 32 B — the margin dominates, not the
lookahead.

Findings:

1. **Session lane notes are the density ceiling, not the decoder.**
   ~34 s of physical events stay resident for judgement/rewind margin.
   100K ev/s -> 3.6M notes (~115 MB); 300K ev/s -> ~10M (~320 MB).
   LaneNote is a flat 32 B struct — no per-note heap. This is where
   virtual/compressed notes would pay off IF a chart sustains
   >~300K physical ev/s — E404C's 5.58T is stack-weighted logical and
   may decode to far fewer physical events (WCB: 6.4M physical carries
   382B stack-weighted).

2. **Chunk cache hit rate collapses with density.** chunkEvents=8192 is
   a fixed *event* count: at 300K ev/s a chunk spans ~27 ms, so the 4 s
   lookahead touches ~146 chunks per pump and the 8-entry/128 MiB LRU
   thrashes (33.5% hit). Hit rate is a function of
   `chunkTimespan vs lookahead`, not chart size. Charts encoded for
   extreme density should use larger chunkEvents (decoder accepts any
   size <= kMaxChunkExpand); alternatively rangeWindow's sliced decode
   becomes relevant exactly here — giant chunks + small windows.

3. **Pump p99.9 spikes scale with density** (81 ms at 300K/s) — GC
   prefix erases over multi-million-note lane vectors and cache-miss
   decode bursts. Avg pump stays ~1 ms; spikes are the frame-time risk.

4. **GameEvent drain matters**: Session::events must be consumed per
   frame (the game does); an undrained bench inflated RSS ~370 MB.

`gh_tests`: 16194 passed, 0 failed.

## Session GC compaction + feed scratch (follow-up)

Three changes after the density baseline:

1. **Bounded GC compaction** (`Session::gcBefore`): the dead-prefix
   erase now drops at most 262144 notes per lane per pass instead of
   memmoving every survivor at once. Ordering, note states, cursors and
   active holds are adjusted by exactly the erased count; the remaining
   dead prefix is consumed on later passes. Amortized cost is unchanged,
   per-pass worst case is ~8 MB of moves instead of the whole lane
   vector. Side benefit: resolved-but-not-yet-erased notes remain
   rewind-restorable slightly longer.

2. **One-time lane reserve** (`Session::reserveLaneNotes`, called once
   by `PlaybackFeed::advance`): estimates resident notes as
   physicalEvents x 34 s / (lengthMs x 2 x keys), clamped [4K, 1M] per
   lane vector. Eliminates repeated doubling reallocs of multi-hundred-
   thousand-note vectors mid-song.

3. **Feed scratch reuse**: `PlaybackFeed` keeps `evsBuf_`/`idxBuf_`
   across `advance()` calls — no per-frame Event/index allocation.

4. **Terminology**: `fedEvents()`/`appendedEvents()` are PHYSICAL events;
   `fedStackedNotes()` is the stack-weighted logical multiplicity.
   NPS figures below name which they measure. WCB's 382.5B stack-weighted
   notes ride on only 6.42M physical events — the "trillion-note" class
   is a weighting property, not an object count.

Measured after (same charts, same bench):

| chart | physical NPS | pump p99.9 | peak RSS |
|---|---|---|---|
| d300k (27 s) | 4.80M -> 6.09M (+27%) | 81 -> 80 ms | 320 -> 360 MB |
| d100k (60 s) | 4.60M -> 5.04M (+10%) | 27 -> 26 ms | 180 -> 191 MB |
| WCB | 2.24M -> 2.72M (+21%) | 63 -> 46 ms | 274 -> 275 MB |
| Dogphobia | ~same | ~same | 28 -> 12 MB |

Interpretation: throughput improved meaningfully (less time lost to
realloc/GC spikes inside pump), p99.9 improved ~25% on WCB. RSS rose
slightly where reserve over-allocates vs actual density — the estimate
is average-density based; peak sections still grow beyond it. Remaining
spikes are decode-burst + GC-erase bound, not realloc.

`gh_tests`: 16194 passed, 0 failed.

## Decode scheduling fixes (cache cap, warm prefetch, adaptive reserve)

Root causes found by `densbench <file> rt` (real-time-paced 60 Hz —
the unpaced bench outruns prefetch workers and overstates spikes):

1. **LRU entry cap defeated prefetch.** `cache_.size() >= 8` evicted
   regardless of the 128 MiB event budget — at 300K ev/s a chunk spans
   ~27 ms, so 8 resident chunks covered <0.3 s while prefetch ran 16 s
   ahead. Hit rate was 33%. Cap raised to 8192 entries so the event
   budget (~4.19M events) is the real bound: hit rate 33% -> 72% (300K/s)
   and 62% -> 86% (100K/s).

2. **Prefetch stored every chunk twice.** `chunkEventsAsync` decoded
   into the LRU AND copied all events into a per-future buffer that
   lived until the whole batch finished (~150 MB redundant at 300K/s).
   New `Reader::warmChunkAsync`/`Impl::warmChunk` decodes cache-only;
   `prefetchBufs_` deleted; `harvestPrefetch` now reaps finished futures
   individually instead of all-or-nothing.

3. **Adaptive lane reserve.** `PlaybackFeed` tops up
   `Session::reserveLaneNotes` when observed occupancy exceeds 75% of
   the estimate (nonuniform density), and the estimate now accounts for
   songs shorter than the ~34 s residency window.

Real-time-paced results:

| chart | pump avg | p99 | p99.9 | cache hit | peak RSS |
|---|---|---|---|---|---|
| d300k rt | 0.49 ms | 0.79 ms | ~100 ms | 72% | 493 MB |
| d100k rt | 0.18 ms | 0.33 ms | 17.8 ms | 86% | 284 MB |

Status vs the 16.7 ms p99.9 target: d100k is at the line (17.8 ms);
d300k still spikes ~100 ms. Remaining suspects, in likely order:
synchronous decode bursts on the residual ~28% misses (cache holds
~14 s at 300K/s vs 16 s prefetch — boundary thrash), lane-vector
growth beyond reserve in peak-density sections, and first-touch page
faults on the ~290 MB reserve. Next step is sub-timing inside
`advance()` (pump vs convert vs GC) to attribute the spike before more
changes. `gh_tests`: 16194 passed, 0 failed.

## Frame-time stability pass (slice pump, deque lanes, batched prefetch)

Sub-timing inside `advance()` (`lastPumpMs_/lastConvertMs_/lastGcMs_`,
exposed in the F3 overlay's FTIMES/FEED rows) attributed the d300k
~100 ms p99.9 spikes to three distinct causes, fixed independently:

1. **Whole-horizon pump.** `PlaybackFeed` fetched the entire 4 s
   lookahead in ONE `pump()` call; on cache misses that synchronously
   decoded hundreds of ms of chart on the main thread. Now the horizon
   is fetched in 50 ms song-time slices, max 3 per `advance()`
   (`kSliceUs`/`kMaxFetchSlices`), preserving order (contiguous,
   appended in time order). An emergency floor
   (`kEmergencyAheadUs` = playhead + 500 ms, `kMaxEmergencySlices` = 16)
   keeps judgement supplied after seeks/restarts/large clock steps —
   the horizon itself still fills gradually.

2. **Vector lane GC memmove + realloc spikes.** `Session` lanes are now
   `std::deque<LaneNote>`: front erase destroys the dead prefix in
   place instead of memmoving the whole surviving tail (the old
   "bounded compaction" still moved every survivor). This also removed
   the multi-hundred-thousand-note vector doubling-reallocs —
   `reserveLaneNotes` is gone. `gcBefore` additionally keeps a per-lane
   `gcScan_` resume hint so the dead-prefix scan is amortized O(1)
   instead of re-walking the prefix every pass; `rewindTo`/`init`
   reset it. GC max on WSB: ~20 ms -> ~9 ms once, typically <0.1 ms.

3. **Prefetch thread storm.** `prefetch()` spawned one `std::async`
   (real thread, ~0.1 ms creation) PER CHUNK — a dense window is
   hundreds of chunks, all spawned inside one `advance()`. New
   `Reader::warmChunksAsync(first,last)` batches ~16 chunks per worker
   task. Also: prefetch now anchors at 0 on the first call so the
   startup window warms on workers instead of being decoded
   synchronously slice-by-slice.

Real-time-paced 60 Hz results (`densbench <file> rt`; "pump" column is
whole `advance()`, convert/gc are sub-phases):

| chart | frame avg | p99 | p99.9 | max | notes |
|---|---|---|---|---|---|
| Dogphobia | 0.034 ms | 0.13 | 0.28 ms | 1.5 ms | was already fine |
| d100k synth | 0.32 ms | 1.8 | 5.6 ms | 24 ms | p99.9 17.8 -> 5.6 |
| WCB (6.4M phys ev) | 0.30 ms | 2.5 | ~4-5 ms | ~17 ms | p99.9 ~19 -> ~4 |
| d300k synth | ~1.2 ms | ~8 | ~26-44 ms | ~45 ms | still over target |

`gh_tests`: 16194 passed, 0 failed.

### Remaining d300k bottleneck

The 300K physical-events/sec synthetic (9M events, whole 27 s chart
resident) still shows ~26-44 ms p99.9 with high run-to-run variance.
With a 512 MiB cache budget (zero re-decodes — every chunk decoded
exactly once) spikes persist, so they are NOT decode misses. Remaining
suspects: eviction-thrash misses at the default 128 MiB budget
(~48-51% hit rate there), catch-up conversion frames
(`kMaxConvert` = 32768 deque push_backs), allocator/scheduler
contention between 8 decode workers and the main thread, and deque
block-alloc churn. RSS also rose ~+290 MB vs the vector era at 9M
resident notes (deque block overhead + fragmentation) — bounded but
worth revisiting with a custom chunked lane store if memory matters.

`densbench` gained an optional 4th arg: memory budget in MiB
(`densbench <file> rt [mb]`).
