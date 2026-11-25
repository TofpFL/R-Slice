#pragma once

#include <cstdint>

// C++ bridge between the Haxe externs (RaidEngine.hx) and the R-Slice
// C ABI (engine_abi.h in R-Slice-Main/src). Compiled into the lime build
// via project/Build.xml; links gh_core/gh_lua/gh_miniz static libs.
//
// Event access is via rs_event_field() so Haxe never handles raw C
// structs or pointers into native memory.

void* rs_engine_open(const char* path);
void  rs_engine_close(void* eng);

// Polls up to the internal buffer capacity; returns events written.
// Afterwards rs_event_field(i, f) reads event i (0-based) field f:
//   0 timeMs   1 lane   2 side   3 lengthMs   4 kind
//   5 noteKind 6 noteFlags 7 noteType 8 stack 9 id
int    rs_engine_poll(void* eng, double nowMs);
double rs_event_field(int index, int field);
// Flat per-event buffer filled by the last rs_engine_poll: 6 doubles per
// event in the order [timeMs, lane, side, lengthMs, kind, stack]. Valid
// until the next poll; index as ptr[eventIndex * 6 + field]. Bulk-read
// path — preferred over rs_event_field for hot loops.
const double* rs_events_flat();

// Rewind the stream cursor; buffered events are dropped.
int rs_engine_seek(void* eng, double tMs);

// Engine identity: 0 = json, 1 = raid, -1 = null handle.
int    rs_engine_kind(void* eng);
// Total logical events reported by the engine (0 when unknown).
double rs_engine_logical_events(void* eng);
// Stack-weighted note total for one strumline side (0 opponent, 1 player,
// 2 gf, -1 all sides); 0 when unknown. First call decodes once and caches
// every side.
double rs_engine_note_total(void* eng, int32_t side);
// 1 when the engine supports seek(), else 0.
int    rs_engine_seekable(void* eng);
// Optional decode warm-up for [tMs, tMs+horizonMs); no-op on JSON.
void   rs_engine_prefetch(void* eng, double tMs, double horizonMs);
// Starting tempo in BPM from the chart metadata (0 when unknown).
double rs_engine_bpm(void* eng);

// Chunk decode-cache instrumentation as a static string:
// "hits=.. misses=.. decodes=.. bytes=.. cached=.. cap=.." for raid
// engines, "json" for materialized charts, NULL on a bad handle.
// Valid until the next call.
const char* rs_engine_cache_stats(void* eng);

// Reads the .raid meta JSON section directly (not via an engine handle).
// Returns a static buffer valid until the next call, or NULL on failure.
// The caller must copy the string before calling again.
const char* rs_raid_meta_json(const char* path);

// ---- op-log (pattern calls) -------------------------------------------
// Fills up to kMaxOpSegments segments overlapping [startMs, endMs);
// returns the count written. Afterwards rs_op_field(i, f) reads segment
// i: 0 startMs  1 endMs  2 chunk  3 kind (0 literal, 2 pattern)
// 4 count (literal events / patternId)  5 aux (repeatCount)
// 6 pitchDelta  7 laneDelta. Decodes op streams synchronously — throttle.
int    rs_engine_oplog(void* eng, double startMs, double endMs);
int    rs_engine_oplog_count(void* eng);
double rs_op_field(int index, int field);

// Last pump/decode error as a static string (empty when healthy),
// NULL on a bad handle. Valid until the next call.
const char* rs_engine_last_error(void* eng);

// ---- GL smoke test (raid_gl_smoke.cpp) --------------------------------
// Capabilities report / rolling stats as static strings.
const char* rs_gl_probe();
double     rs_gl_smoke_frame(int count, int mode);
const char* rs_gl_smoke_stats();
// Vendor-neutral capability bitmask (see raid_gl_smoke.hpp for bits).
int   rs_gl_caps();

// ---- Phase 5D instanced note renderer (raid_gl_smoke.cpp) -------------
// Full signatures in raid_gl_smoke.hpp; declared here so generated Haxe
// cpp (which only includes this header) can call them.
int rs_notes_create();
void rs_notes_destroy(int h);
void rs_notes_reset(int h);
int rs_notes_setTexture(int h, double pixPtr, int w, int hh);
void rs_notes_setDir(int h, int dir,
                     double v0, double v1, double v2, double v3,
                     double v4, double v5, double v6, double v7,
                     double v8, double v9, double v10, double v11,
                     double v12, double v13, double v14, double v15);
void rs_notes_add(int h, float time, int dir);
void rs_notes_kill(int h, int index);
void rs_notes_removePrefix(int h, int n);
int rs_notes_draw(int h,
                  double now, double pxPerMs, double dirSign,
                  double strumX, double strumY, double scrollX, double scrollY,
                  double r00, double r01, double r02,
                  double r10, double r11, double r12,
                  double cullX0, double cullY0, double cullX1, double cullY1);
