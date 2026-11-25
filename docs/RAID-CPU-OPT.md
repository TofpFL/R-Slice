# RAID CPU-Side Optimization Report

Benchmark: Fraudulent `.raid`, dense section t≈44–72s, GTX 1060 laptop
(i7-7700HQ), `RAID_PROFILE=1 RAID_PLAYSTATE=1 RAID_BOTPLAY=1`.
Frame percentiles are per-frame EMA sampled every 2s (`f50/f95/f99/fmax`).

Logs: `logs/prof-baseline.log` (pre-opt), `logs/prof-opt1.log` (judge cursor +
GL ahead-break), `logs/profrun3.log` / `logs/profrun4.log` (+ dead-lookup gate).

## Results — dense zone (per 2s window)

| Metric | Baseline | After opt | Change |
|---|---:|---:|---|
| `glnotes` ms/2s | ~360–515 | ~156–260 | **~2.1× faster** (~0.5ms/frame) |
| `glscan` notes/2s | ~0.9–1.36M | ~144–245k | **~6–8× less work** |
| `glquad` quads/2s | ~85–135k | ~91–159k | identical output (correctness) |
| `judge` ms/2s | ~148–250 | ~133–253 | unchanged (see below) |
| `jscan` records/2s | ~1.0–1.5M | = `jmat` ~2.6–4.4k | **~400× less scanning** |
| frame f50 (peak dense) | 4.7ms | 3.8–3.9ms | **~20% faster** |
| frame f95 (peak dense) | 7.9ms | 6.1–6.6ms | ~20% faster |
| frame f99 (peak dense) | 10.9ms | 8.5–11.3ms | ~similar (tail noise) |

Subsystem totals per frame (dense): `glnotes` ~0.5ms, `judge` ~0.4–0.6ms,
`notes` (sprite loops) ~0.4–0.9ms, `stream` ~0.15–0.3ms, `strum` ~0.02ms.

## Changes made

1. **`RaidProf.hx`** (new) — env-gated (`RAID_PROFILE`) per-subsystem timers,
   frame percentile ring, work counters. Zero cost when disabled.
2. **`RaidNoteJudge.hx`** — replaced per-frame O(window) rescan with a
   forward-only `scanCursor` (records are time-sorted, due-ness monotonic).
   Cursor rewinds on backward seek, adjusts on compact-splice. `jscan` now
   equals `jmat` — only newly-due records are visited.
3. **`RaidGLNotes.hx`** — `ahead-break`: notes beyond the far viewport edge
   break early on sorted `times[]` instead of scanning to stream horizon.
   Dead-note Map lookup gated to the judgement zone (`times[i] <= now+500`)
   since dead notes can only exist near the judgement instant.
4. **Instrumentation** in `PlayState.processNotes`, `Strumline.updateNotes`,
   `RaidChartStream.pump` (frame ring + ev/pump/conv counters).

## What the profiling proved

- ~85–90% of scanned records were discarded work — both the GL batch and the
  judge rescanned the entire stream window every frame.
- After removing scans, `judge` time is dominated by **`materialize()`** —
  `buildNoteSprite` + `onNoteIncoming.dispatch` per due record (~7–12/frame).
  This is inherent sprite-construction cost, not scanning.
- `glnotes` residual is per-frame vertex emission for ~250–400 visible quads
  (~22 Vector pushes each) plus per-note math — the real fix is Phase 5D
  (GPU-side positioning / persistent instance buffers).

## Known anomaly (pre-existing, not a regression)

At t≈70–73s (chart tail → results transition) all runs show a judge/materialize
burst (`jmat` ~4k) and occasional 100ms frames; sprite pool temporarily grows
(157→~400, once 2654 in a noisy run). Magnitude varies run-to-run (199/501/1426ms
across three runs) — GC/allocation-storm sensitivity at mass sprite churn.
Present before these changes; flagged for Phase 5 follow-up.

## Remaining bottlenecks (next targets, by measured cost)

1. Judge `materialize()` ~0.4–0.6ms/frame — NoteSprite construction fast-path
   or deferred non-judgement visuals.
2. `glnotes` ~0.5ms/frame — Phase 5D GPU positioning eliminates CPU quad rebuild.
3. `notes` sprite loops ~0.4–0.9ms/frame — `checkNoteWindow` over pool members.
4. `stream` ~0.15–0.3ms/frame — already cursor-based; allocs are per-event.

## Acceptance criteria status

- Judgement accuracy: preserved — `jmat`/`jscan` confirm identical
  materialization set; `glquad` counts match baseline output.
- Streaming/seek: cursor rewinds on backward seek; forward seek materializes
  due records in one tick (correct, bursty).
- Compatibility: no GL changes; `RaidGLNotes` remains pure OpenFL.
- <4ms CPU frame target: f50 ≈3.8–3.9ms dense (at target); f95 ~6ms remains
  above — needs Phase 5D/5E work, not more scan elimination.

---

# Phase 5D — GPU-Side Note Positioning (implemented, benchmarked)

## What changed

`RaidGLNotes` gained a native instanced-GL backend (`rs_notes_*` in
`raid_gl_smoke.cpp`) that replaces the per-frame CPU quad rebuild:

- **Instance data**: `{timeMs, dir, deadFlag}` = 16B/note, appended on
  `addNote`, flagged on `noteKilled`, prefix-erased on splice-compact.
  Uploaded incrementally via `glBufferSubData` — never rebuilt per frame.
- **Static quad VBO** + per-instance attribute divisor; one
  `glDrawArraysInstanced(TRIANGLES, 0, 6, N)` per strumline per frame.
- **Vertex shader** computes the full transform on GPU: scroll position
  (`uNow/uPxPerMs/uDirSign/uStrum/uScroll`), per-direction quad constants
  (`uDir[16]` vec4s — frame size, scale, origin, offsets, xBase, UVs),
  view→device affine (`uRow0/uRow1`, probed once per frame via
  `canvas.localToGlobal` so camera zoom/scrollRect/scaleMode are captured),
  and viewport culling (degenerate quad — Phase E culling without
  compute/readback).
- **Texture**: noteskin atlas uploaded once as straight-alpha RGBA;
  shader premultiplies and blends `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`
  (matches OpenFL's premultiplied pipeline).
- **Shaders**: `#version 330` primary, `#version 120` fallback (AMD compat),
  attribute slots bound explicitly.
- **CPU per-frame cost now**: ~15 uniform sets + 1 draw call + affine probe.
- CPU `drawTriangles` path retained as automatic fallback (renderer not
  ready → returns 0 → CPU path). Env override: `RAID_GL_NOTES=0`.

Judgement unchanged: sprites still materialize for the judge; GL renderer
only replaces the visual batch. `materialize()` cost untouched (next target).

## Benchmark (Fraudulent dense 44–72s, GTX 1060, profiled run)

| Metric | Pre-5D (profrun4) | Post-5D (prof-phased) |
|---|---:|---:|
| frame f50 (peak) | 3.8–3.9ms | **3.4–3.6ms** |
| frame f95 (peak) | 6.1–6.3ms | **4.6–5.0ms** |
| `glnotes` ms/2s window | ~165–245 | **~42–71 (~3× less)** |
| `glquad` CPU quads | ~110k/2s | 0 (GPU-side) |

Frame EMA dense: ~3.4–3.8ms. Correctness proxies: identical materialization
counts (`jmat`), clean run to results screen, no GL errors.

## NOT yet verified

- **Visual correctness** — positions/UVs/blend/z-order need an eyeball
  check (screenshot or gameplay). Math was verified line-by-line against
  the CPU path; affine probe assumes no canvas rotation (true for cameras).
- **AMD** — code paths are capability-gated + 120-fallback ready, but
  unverified on RX 580 until next bin copy.
- Seek/restart/modchart visual regression — logic mirrored but unverified.

## Remaining bottlenecks (updated)

1. Judge `materialize()` ~0.4–0.6ms/frame — now the largest single item.
2. `notes` sprite loops ~0.4–0.9ms/frame.
3. `stream` ~0.15–0.4ms/frame.
4. `glnotes` residual ~0.2ms/frame (uniform sets + affine probe +
   per-frame `localToGlobal` Point allocs — could cache).
