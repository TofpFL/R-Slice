// C ABI for the playback-engine boundary — see docs/ICHART_SOURCE.md §6.
//
// Flat, POD-only interface so Haxe (hxcpp) and other hosts can drive a
// streaming chart without C++ classes, virtuals, or pointers into
// GC-managed objects. Pull model: the caller passes song time (`nowMs`)
// and drains due events each frame.
//
// This header is plain C — safe to include from C and C++.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Event kinds emitted through GhGameplayEvent. Note is the only kind the
// RAID backend produces today; the field exists so JSON/side-channel
// events can share the shape later.
enum GhEventKind {
    GH_EVENT_NOTE = 0,
    GH_EVENT_CAMERA = 1,
    GH_EVENT_CHARACTER = 2,
    GH_EVENT_STAGE = 3,
    GH_EVENT_HEALTH = 4,
    GH_EVENT_MARKER = 5,
    GH_EVENT_TEMPO = 6
};

typedef struct GhGameplayEvent {
    double timeMs;          // event time, song milliseconds
    int32_t kind;           // GhEventKind
    int32_t lane;           // 0..keys-1, -1 for non-lane events
    int32_t side;           // 0 opponent, 1 player, 2 gf
    float lengthMs;         // sustain length, 0 = tap
    uint8_t noteKind;       // 0 normal, 1 mine
    uint8_t noteFlags;
    uint16_t noteType;      // index into chart meta noteTypes
    uint16_t stack;         // stacked-note multiplicity (>= 1)
    int64_t aux;            // kind-specific payload (0 for notes)
    uint64_t id;            // stable per-chart id (raid: global expanded index)
} GhGameplayEvent;

typedef struct GhEngineInfo {
    int32_t engine;         // 0 json, 1 raid
    char format[32];        // "raid", "fnf-json", ...
    uint64_t logicalEvents; // total events if known, else 0
    uint64_t rawBytes;      // file size on disk, 0 if unknown
    int32_t seekable;       // 0 = live/unseekable stream
    double bpm;             // starting tempo (bpmAt(0)); 0 when unknown
} GhEngineInfo;

// Backend selection by file: magic bytes / extension decide the engine.
// Returns NULL on failure; `err`/`errLen` receive a reason when provided.
void* gh_engine_open(const char* path, char* err, size_t errLen);
void gh_engine_close(void* engine);

// Fills `out` with engine/format info. Returns 0 on success.
int32_t gh_engine_info(void* engine, GhEngineInfo* out);

// Stack-weighted total notes the chart can produce — the FNF "note count"
// where one press hits a whole stack. `side` selects the strumline side:
// 0 opponent, 1 player, 2 gf, -1 all sides. Opponent/gf notes never reach
// the results tally, so side=1 is the "totalNotes" denominator. RAID: sum
// of event.stack over every expanded event (computed once, cached per
// side). JSON: sum over materialized notes. Returns 0 when unknown.
double gh_engine_note_total(void* engine, int32_t side);

// Drains up to `capacity` events due in (lastPoll, nowMs + lookahead].
// `nowMs` is song time from the audio clock — passed in, never owned.
// Events are consumed; polling twice with the same nowMs returns nothing
// new. Returns the number of events written to `buf`.
size_t gh_engine_poll(void* engine, GhGameplayEvent* buf, size_t capacity,
                      double nowMs);

// Rewind for restart / practice / scrub. Returns 0 on success.
int32_t gh_engine_seek(void* engine, double tMs);

// Optional decode warm-up for [tMs, tMs+horizonMs). No-op for engines
// without a decode cache.
void gh_engine_prefetch(void* engine, double tMs, double horizonMs);

// Chunk decode-cache instrumentation for raid engines, written into
// `buf` as "hits=.. misses=.. decodes=.. bytes=.. cached=.. cap=..".
// Writes "json" for materialized charts. Returns 0 on success, -1 on a
// bad handle or insufficient buffer.
int32_t gh_engine_cache_stats(void* engine, char* buf, size_t len);

// One stretch of a chunk's op stream (raid only) — see
// gh::raid::Reader::OpSegment. kind 0 = literal run, 2 = pattern
// instance; for patterns `count` is the patternId and `aux` the repeat
// count. Expanded events lose pattern identity; this is the "which
// pattern is playing when" view used for call-driven scheduling/logging.
typedef struct GhOpSegment {
    double startMs;
    double endMs;
    uint32_t chunk;
    uint8_t  kind;
    uint32_t count;
    uint32_t aux;
    int32_t  pitchDelta;
    int32_t  laneDelta;
} GhOpSegment;

// Fills `buf` with op segments overlapping [startMs, endMs), in playback
// order. Returns the number written (0 for json engines / empty window).
// Decodes chunk op streams synchronously — callers should throttle and
// keep windows modest; it does not materialize events.
size_t gh_engine_oplog(void* engine, double startMs, double endMs,
                       GhOpSegment* buf, size_t capacity);

// Last pump/decode error for the engine (empty string when healthy).
// Lets streaming hosts distinguish "no events due" from a stalled feed.
int32_t gh_engine_last_error(void* engine, char* buf, size_t len);

#ifdef __cplusplus
}
#endif
