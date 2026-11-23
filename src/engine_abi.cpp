// engine_abi.cpp — C ABI implementation over the native ChartStream.
// See docs/ICHART_SOURCE.md §6 for the boundary contract.
//
// The pump/convert policy mirrors PlaybackFeed::advance (raid_chart.cpp)
// slice-for-slice — same lookahead, slice size, emergency floor, prefetch
// horizon — minus the Session: events are emitted to the caller instead of
// appended to lanes.

#include "engine_abi.h"
#include "raid.hpp"
#include "raid_chart.hpp"
#include "chart.hpp"
#include "chart_stream.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace gh::raid {

// Same bounds as PlaybackFeed — keep in sync with raid_chart.hpp.
namespace {

constexpr int64_t kSliceUs = 50000;
constexpr int kMaxFetchSlices = 3;
constexpr int64_t kEmergencyAheadUs = 500000;
constexpr int kMaxEmergencySlices = 16;
constexpr double kLookaheadMs = 4000.0;
constexpr double kPrefetchMs = 16000.0;
constexpr size_t kMaxConvert = 32768;

}  // namespace

// ABI handle — owned by gh_engine_open/close, opaque to callers.
// stream != null → RAID engine; stream == null → JSON engine with
// materialized notes in jsonChart (emitted in time order by poll).
struct AbiEngine {
    std::unique_ptr<ChartStream> stream;
    Chart meta;
    Chart jsonChart;
    size_t jsonCursor = 0;

    // Pump scratch — same role as PlaybackFeed::evsBuf_/idxBuf_/bufPos_.
    std::vector<Event> evsBuf;
    std::vector<uint64_t> idxBuf;
    size_t bufPos = 0;
    int64_t fedUs = std::numeric_limits<int64_t>::min() / 4;
    int64_t prefetchUs = 0;
    uint64_t fedEvents = 0;

    // Converted-but-undrained events awaiting the caller's buffer.
    std::vector<GhGameplayEvent> pending;
    size_t pendingPos = 0;

    double lastPumpMs = 0, lastConvertMs = 0;
    std::string lastErr;

    // Cached stack-weighted note totals per side (-1 = all); <0 = not
    // computed yet. Index by side+1 so -1..2 map to 0..3.
    double noteTotals[4] = {-1, -1, -1, -1};
};

namespace {

void setErr(char* err, size_t len, const std::string& msg) {
    if (err && len) std::snprintf(err, len, "%s", msg.c_str());
}

GhGameplayEvent convert(const Event& e, uint64_t globalIdx) {
    GhGameplayEvent g{};
    g.timeMs = double(e.timeUs) / 1000.0;
    g.kind = GH_EVENT_NOTE;
    g.lane = e.lane;
    g.side = e.side;
    g.lengthMs = float(double(e.lengthUs) / 1000.0);
    g.noteKind = e.kind;
    g.noteFlags = e.flags;
    g.noteType = e.type;
    g.stack = e.stack;
    g.aux = 0;
    g.id = globalIdx;
    return g;
}

// Top up evsBuf toward (nowMs + lookahead) in bounded slices, then convert
// up to kMaxConvert events into `pending`. Mirrors PlaybackFeed::advance's
// fetch + convert phases including the emergency catch-up floor.
GhGameplayEvent convertNote(const gh::Note& n, uint64_t idx) {
    GhGameplayEvent g{};
    g.timeMs = n.time;
    g.kind = GH_EVENT_NOTE;
    g.lane = n.lane;
    g.side = n.side;
    g.lengthMs = float(n.length);
    g.noteKind = n.kind;
    g.noteFlags = n.flags;
    g.noteType = n.type;
    g.stack = n.stack;
    g.id = idx;
    return g;
}

// V-Slice convention: "<song>-metadata.json" sits beside the chart files.
// Given a chart path, find the best-matching metadata file in its dir.
std::string findMetadataJson(const std::string& chartPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(chartPath);
    const fs::path dir = p.parent_path();
    if (dir.empty()) return "";
    std::string stem = p.stem().string();
    // "bopeebo-Normal" -> "bopeebo" (strip trailing -<word>)
    const size_t dash = stem.find_last_of('-');
    if (dash != std::string::npos) stem = stem.substr(0, dash);
    std::string best, first;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; ++it) {
        if (!it->is_regular_file(ec)) continue;
        std::string fn = it->path().filename().string();
        if (fn.size() < 14) continue;
        std::string low = fn;
        for (char& c : low) c = char(std::tolower((unsigned char)c));
        if (low.substr(low.size() - 14) != "-metadata.json") continue;
        const std::string full = it->path().string();
        if (first.empty()) first = full;
        const std::string prefix = fn.substr(0, fn.size() - 14);
        std::string pl = prefix, sl = stem;
        for (char& c : pl) c = char(std::tolower((unsigned char)c));
        for (char& c : sl) c = char(std::tolower((unsigned char)c));
        if (pl == sl && (best.empty() || prefix.size() > best.size()))
            best = full;
    }
    return best.empty() ? first : best;
}

bool pumpAndConvert(AbiEngine& eng, double nowMs, std::string& err) {
    const int64_t endUs =
        int64_t(std::max(0.0, nowMs + kLookaheadMs) * 1000.0);
    eng.lastPumpMs = eng.lastConvertMs = 0;
    {
        const auto p0 = std::chrono::steady_clock::now();
        const int64_t emergencyUs =
            int64_t(std::max(0.0, nowMs) * 1000.0) + kEmergencyAheadUs;
        int slices = 0;
        while (eng.fedUs < endUs &&
               (eng.fedUs < emergencyUs ? slices < kMaxEmergencySlices
                                        : slices < kMaxFetchSlices)) {
            if (eng.bufPos >= eng.evsBuf.size()) {
                eng.evsBuf.clear();
                eng.idxBuf.clear();
                eng.bufPos = 0;
            }
            const int64_t sliceEnd = eng.fedUs < 0
                ? std::min(endUs, kSliceUs)
                : std::min(endUs, eng.fedUs + kSliceUs);
            const size_t before = eng.evsBuf.size();
            if (!eng.stream->pump(eng.fedUs, sliceEnd, eng.evsBuf,
                                  eng.idxBuf, err)) {
                eng.lastPumpMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - p0).count();
                return false;
            }
            eng.fedEvents += eng.evsBuf.size() - before;
            eng.fedUs = sliceEnd;
            ++slices;
        }
        eng.lastPumpMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - p0).count();
    }
    if (eng.bufPos < eng.evsBuf.size()) {
        const auto c0 = std::chrono::steady_clock::now();
        const size_t stop =
            std::min(eng.evsBuf.size(), eng.bufPos + kMaxConvert);
        eng.pending.reserve(eng.pending.size() + (stop - eng.bufPos));
        for (; eng.bufPos < stop; ++eng.bufPos) {
            eng.pending.push_back(
                convert(eng.evsBuf[eng.bufPos], eng.idxBuf[eng.bufPos]));
        }
        eng.lastConvertMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - c0).count();
        if (eng.bufPos >= eng.evsBuf.size()) {
            eng.evsBuf.clear();
            eng.idxBuf.clear();
            eng.bufPos = 0;
        }
    }
    // Prefetch — same adaptive window as PlaybackFeed::advance.
    if (endUs >= eng.prefetchUs) {
        double pm = kPrefetchMs;
        const double fedSec = eng.fedUs > 0 ? double(eng.fedUs) / 1e6 : 0.0;
        const uint64_t cap = eng.stream->cachedEventCapacity();
        if (fedSec > 1.0 && eng.fedEvents > 0 && cap > 0) {
            const double evPerSec = double(eng.fedEvents) / fedSec;
            const double fitSec = double(cap / 2) / std::max(1.0, evPerSec);
            pm = std::min(kPrefetchMs, std::max(2000.0, fitSec * 1000.0));
        }
        eng.stream->prefetch(int64_t(0),
                             endUs + int64_t(pm * 1000.0));
        eng.prefetchUs = endUs + int64_t(pm * 500.0);
    }
    return true;
}

size_t drainPending(AbiEngine& eng, GhGameplayEvent* buf, size_t capacity) {
    const size_t avail = eng.pending.size() - eng.pendingPos;
    const size_t n = std::min(avail, capacity);
    if (n) {
        std::memcpy(buf, eng.pending.data() + eng.pendingPos,
                    n * sizeof(GhGameplayEvent));
        eng.pendingPos += n;
    }
    if (eng.pendingPos == eng.pending.size()) {
        eng.pending.clear();
        eng.pendingPos = 0;
    }
    return n;
}

}  // namespace

}  // namespace gh::raid

extern "C" {

void* gh_engine_open(const char* path, char* err, size_t errLen) {
    if (!path) { gh::raid::setErr(err, errLen, "null path"); return nullptr; }
    using namespace gh::raid;
    auto eng = std::make_unique<AbiEngine>();
    std::string e;
    if (isRaidFile(path)) {
        eng->stream = std::make_unique<ChartStream>();
        if (!eng->stream->open(path, eng->meta, e)) {
            gh::raid::setErr(err, errLen, e);
            return nullptr;
        }
    } else {
        // JSON-family chart (legacy/Psych/V-Slice/FL Studio project):
        // materialized engine — notes live in jsonChart, emitted in
        // time order. V-Slice charts pick up a sibling *-metadata.json.
        std::string metaText;
        const std::string metaPath = findMetadataJson(path);
        if (!metaPath.empty()) gh::readTextFile(metaPath, metaText);
        if (!gh::loadChartFile(path, "", eng->jsonChart, e, metaText, 0, true)) {
            gh::raid::setErr(err, errLen, e);
            return nullptr;
        }
    }
    eng->evsBuf.reserve(8192);
    eng->idxBuf.reserve(8192);
    eng->pending.reserve(8192);
    return eng.release();
}

void gh_engine_close(void* engine) {
    delete static_cast<gh::raid::AbiEngine*>(engine);
}

int32_t gh_engine_info(void* engine, GhEngineInfo* out) {
    if (!engine || !out) return -1;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    std::memset(out, 0, sizeof(*out));
    if (eng.stream) {
        out->engine = 1;  // raid
        std::snprintf(out->format, sizeof(out->format), "raid");
        out->logicalEvents = eng.stream->logicalEvents();
        out->rawBytes = eng.stream->reader().info().rawBytes;
    } else {
        out->engine = 0;  // json
        std::snprintf(out->format, sizeof(out->format), "json");
        out->logicalEvents = eng.jsonChart.notes.size();
        out->rawBytes = 0;
    }
    out->seekable = 1;
    out->bpm = eng.stream ? eng.meta.bpmAt(0.0) : eng.jsonChart.bpmAt(0.0);
    return 0;
}

double gh_engine_note_total(void* engine, int32_t side) {
    if (!engine) return 0;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    // side: 0 opponent, 1 player, 2 gf, -1 all. Cache slot = side + 1.
    int idx = (int)side + 1;
    if (idx < 0 || idx > 3) return 0;
    // First call decodes every chunk once and fills all side caches.
    if (eng.noteTotals[0] < 0) {
        uint64_t perSide[3] = {0, 0, 0};
        uint64_t all = 0;
        if (eng.stream) {
            // Expanded stack-weighted total: decode every chunk once and
            // sum event.stack. logicalEvents counts events, not the notes
            // they stand for — stacked notes are what score/combo see.
            auto& rd = eng.stream->reader();
            const auto& info = rd.info();
            std::vector<gh::raid::Event> buf;
            std::string err;
            for (size_t i = 0; i < info.chunkCount; ++i) {
                buf.clear();
                if (!rd.chunkEvents(i, buf, err)) {
                    eng.noteTotals[0] = eng.noteTotals[1] =
                        eng.noteTotals[2] = eng.noteTotals[3] = 0;
                    return 0;
                }
                for (const gh::raid::Event& e : buf) {
                    uint64_t s = e.stack ? e.stack : 1;
                    all += s;
                    int32_t es = (int32_t)e.side;
                    if (es >= 0 && es <= 2) perSide[es] += s;
                }
            }
        } else {
            for (const gh::Note& n : eng.jsonChart.notes) {
                uint64_t s = n.stack ? n.stack : 1;
                all += s;
                int32_t ns = (int32_t)n.side;
                if (ns >= 0 && ns <= 2) perSide[ns] += s;
            }
        }
        eng.noteTotals[0] = double(all);
        eng.noteTotals[1] = double(perSide[0]);
        eng.noteTotals[2] = double(perSide[1]);
        eng.noteTotals[3] = double(perSide[2]);
    }
    return eng.noteTotals[idx];
}

size_t gh_engine_poll(void* engine, GhGameplayEvent* buf, size_t capacity,
                      double nowMs) {
    if (!engine || !buf || capacity == 0) return 0;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);

    size_t written = gh::raid::drainPending(eng, buf, capacity);
    if (written == capacity) return written;

    if (!eng.stream) {
        // JSON engine: materialized notes, emitted in time order up to
        // the same lookahead horizon the RAID feed uses.
        const double horizon = std::max(0.0, nowMs + gh::raid::kLookaheadMs);
        const auto& notes = eng.jsonChart.notes;
        while (written < capacity && eng.jsonCursor < notes.size() &&
               notes[eng.jsonCursor].time <= horizon) {
            buf[written++] = gh::raid::convertNote(
                notes[eng.jsonCursor], eng.jsonCursor);
            ++eng.jsonCursor;
        }
        return written;
    }

    std::string err;
    eng.lastErr.clear();
    if (!gh::raid::pumpAndConvert(eng, nowMs, err)) {
        eng.lastErr = err;
        return written;
    }
    written += gh::raid::drainPending(eng, buf + written, capacity - written);
    return written;
}

int32_t gh_engine_seek(void* engine, double tMs) {
    if (!engine) return -1;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    if (!eng.stream) {
        const auto& notes = eng.jsonChart.notes;
        size_t lo = 0, hi = notes.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (notes[mid].time < tMs) lo = mid + 1; else hi = mid;
        }
        eng.jsonCursor = lo;
        return 0;
    }
    eng.fedUs = static_cast<int64_t>(tMs * 1000.0);
    eng.prefetchUs = 0;
    eng.evsBuf.clear();
    eng.idxBuf.clear();
    eng.bufPos = 0;
    eng.pending.clear();
    eng.pendingPos = 0;
    eng.stream->resetPrefetch();
    return 0;
}

void gh_engine_prefetch(void* engine, double tMs, double horizonMs) {
    if (!engine) return;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    if (!eng.stream) return;  // JSON engine has no decode cache
    eng.stream->prefetch(static_cast<int64_t>(tMs * 1000.0),
                         static_cast<int64_t>((tMs + horizonMs) * 1000.0));
}

int32_t gh_engine_cache_stats(void* engine, char* buf, size_t len) {
    if (!engine || !buf || len == 0) return -1;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    if (!eng.stream) {
        std::snprintf(buf, len, "json");
        return 0;
    }
    auto& rd = eng.stream->reader();
    uint64_t hits = 0, misses = 0, decodes = 0, bytes = 0;
    rd.cacheStats(hits, misses, decodes, bytes);
    const int n = std::snprintf(buf, len,
        "hits=%llu misses=%llu decodes=%llu bytes=%llu cached=%llu cap=%llu"
        " fed=%lld pend=%zu buf=%zu pos=%zu pump=%.2fms conv=%.2fms",
        (unsigned long long)hits, (unsigned long long)misses,
        (unsigned long long)decodes, (unsigned long long)bytes,
        (unsigned long long)rd.cachedEvents(),
        (unsigned long long)rd.cachedEventCapacity(),
        (long long)eng.fedUs, eng.pending.size(), eng.evsBuf.size(),
        eng.bufPos, eng.lastPumpMs, eng.lastConvertMs);
    return n < 0 || size_t(n) >= len ? -1 : 0;
}

int32_t gh_engine_last_error(void* engine, char* buf, size_t len) {
    if (!engine || !buf || len == 0) return -1;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    if (eng.lastErr.empty()) { buf[0] = 0; return 0; }
    const int n = std::snprintf(buf, len, "%s", eng.lastErr.c_str());
    return n < 0 || size_t(n) >= len ? -1 : 0;
}

size_t gh_engine_oplog(void* engine, double startMs, double endMs,
                       GhOpSegment* buf, size_t capacity) {
    if (!engine || !buf || capacity == 0 || endMs <= startMs) return 0;
    auto& eng = *static_cast<gh::raid::AbiEngine*>(engine);
    if (!eng.stream) return 0;  // JSON engine has no op stream
    std::vector<gh::raid::Reader::OpSegment> segs;
    std::string err;
    if (!eng.stream->reader().opLog(int64_t(startMs * 1000.0),
                                    int64_t(endMs * 1000.0), segs, err))
        return 0;
    const size_t n = std::min(segs.size(), capacity);
    for (size_t i = 0; i < n; ++i) {
        const auto& s = segs[i];
        buf[i].startMs = double(s.startUs) / 1000.0;
        buf[i].endMs = double(s.endUs) / 1000.0;
        buf[i].chunk = s.chunk;
        buf[i].kind = s.kind;
        buf[i].count = s.count;
        buf[i].aux = s.aux;
        buf[i].pitchDelta = s.pitchDelta;
        buf[i].laneDelta = s.laneDelta;
    }
    return n;
}

}  // extern "C"
