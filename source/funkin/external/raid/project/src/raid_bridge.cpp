#include "raid_bridge.hpp"
#include "engine_abi.h"
#include "raid.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr int kMaxEvents = 8192;
// Fields per event in g_flat, matching the order RaidChartStream.pump reads:
// [timeMs, lane, side, lengthMs, kind, stack]. Haxe Float == C double.
constexpr int kFlatFields = 6;

GhGameplayEvent g_events[kMaxEvents];
double g_flat[kMaxEvents * kFlatFields];
int g_count = 0;

}  // namespace

void* rs_engine_open(const char* path) {
    char err[256] = {};
    void* eng = gh_engine_open(path, err, sizeof(err));
    if (!eng) std::fprintf(stderr, "[raid] open failed: %s\n", err);
    return eng;
}

void rs_engine_close(void* eng) { gh_engine_close(eng); }

int rs_engine_poll(void* eng, double nowMs) {
    if (!eng) return 0;
    g_count = int(gh_engine_poll(eng, g_events, kMaxEvents, nowMs));
    for (int i = 0; i < g_count; ++i) {
        const GhGameplayEvent& g = g_events[i];
        double* f = g_flat + i * kFlatFields;
        f[0] = g.timeMs;
        f[1] = double(g.lane);
        f[2] = double(g.side);
        f[3] = double(g.lengthMs);
        f[4] = double(g.kind);
        f[5] = double(g.stack);
    }
    return g_count;
}

const double* rs_events_flat() { return g_flat; }

double rs_event_field(int index, int field) {
    if (index < 0 || index >= g_count) return 0.0;
    const GhGameplayEvent& g = g_events[index];
    switch (field) {
        case 0: return g.timeMs;
        case 1: return double(g.lane);
        case 2: return double(g.side);
        case 3: return double(g.lengthMs);
        case 4: return double(g.kind);
        case 5: return double(g.noteKind);
        case 6: return double(g.noteFlags);
        case 7: return double(g.noteType);
        case 8: return double(g.stack);
        case 9: return double(g.id);
        default: return 0.0;
    }
}

int rs_engine_seek(void* eng, double tMs) {
    return gh_engine_seek(eng, tMs);
}

int rs_engine_kind(void* eng) {
    GhEngineInfo info{};
    return gh_engine_info(eng, &info) == 0 ? info.engine : -1;
}

double rs_engine_logical_events(void* eng) {
    GhEngineInfo info{};
    return gh_engine_info(eng, &info) == 0 ? double(info.logicalEvents) : 0.0;
}

double rs_engine_note_total(void* eng, int32_t side) {
    return gh_engine_note_total(eng, side);
}

int rs_engine_seekable(void* eng) {
    GhEngineInfo info{};
    return gh_engine_info(eng, &info) == 0 ? info.seekable : 0;
}

void rs_engine_prefetch(void* eng, double tMs, double horizonMs) {
    gh_engine_prefetch(eng, tMs, horizonMs);
}

double rs_engine_bpm(void* eng) {
    GhEngineInfo info{};
    return gh_engine_info(eng, &info) == 0 ? info.bpm : 0.0;
}

const char* rs_engine_cache_stats(void* eng) {
    static char buf[512];
    if (!eng || gh_engine_cache_stats(eng, buf, sizeof(buf)) != 0) return nullptr;
    return buf;
}

const char* rs_raid_meta_json(const char* path) {
    static std::string buf;
    buf.clear();
    gh::raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) return nullptr;
    buf = rd.metaJson();
    return buf.empty() ? nullptr : buf.c_str();
}

namespace {

constexpr int kMaxOpSegments = 512;
GhOpSegment g_ops[kMaxOpSegments];
int g_opCount = 0;

}  // namespace

int rs_engine_oplog(void* eng, double startMs, double endMs) {
    g_opCount = 0;
    if (!eng) return 0;
    // chunkOps decodes every chunk payload overlapping the window — at
    // extreme density that is ~1000 read+inflate+walk passes on the game
    // thread every call. Diagnostic only; off unless explicitly enabled.
    static const bool enabled = std::getenv("RAID_CALLS") != nullptr;
    if (!enabled) return 0;
    g_opCount = int(gh_engine_oplog(eng, startMs, endMs, g_ops, kMaxOpSegments));
    return g_opCount;
}

int rs_engine_oplog_count(void* eng) { return eng ? g_opCount : 0; }

double rs_op_field(int index, int field) {
    if (index < 0 || index >= g_opCount) return 0.0;
    const GhOpSegment& s = g_ops[index];
    switch (field) {
        case 0: return s.startMs;
        case 1: return s.endMs;
        case 2: return double(s.chunk);
        case 3: return double(s.kind);
        case 4: return double(s.count);
        case 5: return double(s.aux);
        case 6: return double(s.pitchDelta);
        case 7: return double(s.laneDelta);
        default: return 0.0;
    }
}

const char* rs_engine_last_error(void* eng) {
    static char buf[512];
    if (!eng || gh_engine_last_error(eng, buf, sizeof(buf)) != 0) return nullptr;
    return buf[0] ? buf : nullptr;
}
