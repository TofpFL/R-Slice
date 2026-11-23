// Chart <-> RAID bridge. See raid_chart.hpp.
#include <chrono>
#include <cmath>
#include <cstdio>

#include "json.hpp"
#include "raid_chart.hpp"
#include "session.hpp"

namespace gh::raid {

namespace {

std::string jsonEscape(std::string_view s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    r += buf;
                } else {
                    r += c;
                }
        }
    }
    return r;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// Builds the meta JSON stored in the file. `stats` may be null while
// encoding (stats are only known after writeRaid); the caller patches them
// via Info afterwards — the stored stats block is best-effort.
std::string metaJson(const Chart& c, const Info* stats) {
    std::string j = "{\"raid\":1,\"title\":\"" + jsonEscape(c.title) +
                    "\",\"difficulty\":\"" + jsonEscape(c.difficulty) +
                    "\",\"opponent\":\"" + jsonEscape(c.opponent) +
                    "\",\"player\":\"" + jsonEscape(c.player) +
                    "\",\"gf\":\"" + jsonEscape(c.gf) +
                    "\",\"stage\":\"" + jsonEscape(c.stage) + "\",";
    j += "\"keys\":" + std::to_string(c.keys) + ",\"speed\":" + num(c.speed) + ",";
    j += "\"noteTypes\":[";
    for (size_t i = 0; i < c.noteTypes.size(); ++i) {
        if (i) j += ",";
        j += "\"" + jsonEscape(c.noteTypes[i]) + "\"";
    }
    j += "],\"tempo\":[";
    for (size_t i = 0; i < c.tempo.size(); ++i) {
        if (i) j += ",";
        j += "{\"timeMs\":" + num(c.tempo[i].timeMs) + ",\"bpm\":" + num(c.tempo[i].bpm) +
             ",\"beat\":" + num(c.tempo[i].beat) + "}";
    }
    j += "],\"focus\":[";
    for (size_t i = 0; i < c.focus.size(); ++i) {
        if (i) j += ",";
        j += "{\"timeMs\":" + num(c.focus[i].timeMs) + ",\"side\":" +
             std::to_string(c.focus[i].side) + "}";
    }
    j += "],\"sections\":[";
    for (size_t i = 0; i < c.sections.size(); ++i) {
        if (i) j += ",";
        const SectionInfo& s = c.sections[i];
        j += "{\"timeMs\":" + num(s.timeMs) +
             ",\"mustHit\":" + (s.mustHit ? "true" : "false") +
             ",\"gf\":" + (s.gf ? "true" : "false") +
             ",\"alt\":" + (s.alt ? "true" : "false") +
             ",\"beats\":" + num(s.beats) + ",\"bpm\":" + num(s.bpm) + "}";
    }
    j += "],\"lengthMs\":" + std::to_string(c.lengthMs) +
         ",\"playerNotes\":" + std::to_string(c.playerNotes) +
         ",\"opponentNotes\":" + std::to_string(c.opponentNotes) +
         ",\"stats\":{";
    if (stats) {
        j += "\"logicalEvents\":" + std::to_string(stats->logicalEvents) +
             ",\"chunks\":" + std::to_string(stats->chunkCount) +
             ",\"patterns\":" + std::to_string(stats->patternCount) +
             ",\"literalEvents\":" + std::to_string(stats->literalEvents) +
             ",\"storedBytes\":" + std::to_string(stats->storedBytes) +
             ",\"rawBytes\":" + std::to_string(stats->rawBytes) +
             ",\"codec\":\"deflate\",\"encoder\":\"godshand-raid/1\"";
    } else {
        j += "\"codec\":\"deflate\",\"encoder\":\"godshand-raid/1\"";
    }
    j += "}}";
    return j;
}

Note eventToNote(const Event& e) {
    Note n;
    n.time = double(e.timeUs) / 1000.0;
    n.length = double(e.lengthUs) / 1000.0;
    n.lane = e.lane;
    n.side = e.side;
    n.kind = e.kind == 1 ? kMine : kNormal;
    n.flags = e.flags;
    n.type = e.type;
    n.stack = e.stack ? e.stack : 1;
    return n;
}

}  // namespace

std::vector<Event> chartEvents(const Chart& c) {
    std::vector<Event> out;
    out.reserve(c.notes.size());
    for (const Note& n : c.notes) {
        Event e;
        e.timeUs = int64_t(std::llround(n.time * 1000.0));
        e.lengthUs = uint32_t(std::llround(std::max(0.0, n.length) * 1000.0));
        e.pitch = -1;
        e.lane = n.lane;
        e.side = n.side;
        e.kind = n.kind == kMine ? 1 : 0;
        e.flags = n.flags;
        e.type = n.type;
        e.stack = n.stack ? n.stack : 1;
        out.push_back(e);
    }
    return out;
}

std::string chartMetaJson(const Chart& c) { return metaJson(c, nullptr); }

bool saveChart(const std::string& path, const Chart& chart,
               const EncodeOptions& opt, Info* statsOut, std::string& err) {
    // Two passes would be needed to embed final stats in meta; instead write
    // once with best-effort stats, then rewrite meta is overkill — the stats
    // block is informational. We do a cheap pre-pass: encode once to learn
    // sizes? writeRaid already reports them; meta stats stay approximate.
    std::vector<Event> ev = chartEvents(chart);
    return writeRaid(path, ev, metaJson(chart, nullptr), opt, statsOut, err);
}

bool chartFromMeta(const std::string& meta, Chart& out, Info& info) {
    json::Document d;
    std::string err;
    if (!d.parse(meta, &err)) return false;
    const json::Value& r = d.root();
    if (d.get(r, "raid") && json::Document::num(d.get(r, "raid"), 0) < 1) return false;
    out.title = std::string(d.strOr(d.get(r, "title"), ""));
    out.difficulty = std::string(d.strOr(d.get(r, "difficulty"), ""));
    out.opponent = std::string(d.strOr(d.get(r, "opponent"), ""));
    out.player = std::string(d.strOr(d.get(r, "player"), ""));
    out.gf = std::string(d.strOr(d.get(r, "gf"), ""));
    out.stage = std::string(d.strOr(d.get(r, "stage"), ""));
    out.keys = int(json::Document::num(d.get(r, "keys"), 4));
    out.speed = json::Document::num(d.get(r, "speed"), 2.0);
    out.lengthMs = json::Document::num(d.get(r, "lengthMs"), 0);
    out.playerNotes = uint64_t(json::Document::num(d.get(r, "playerNotes"), 0));
    out.opponentNotes = uint64_t(json::Document::num(d.get(r, "opponentNotes"), 0));
    if (const json::Value* nt = d.get(r, "noteTypes"))
        for (size_t i = 0; i < d.size(*nt); ++i) out.noteTypes.push_back(std::string(d.str(d.at(*nt, i))));
    if (out.noteTypes.empty()) out.noteTypes.push_back("");
    if (const json::Value* tp = d.get(r, "tempo"))
        for (size_t i = 0; i < d.size(*tp); ++i) {
            const json::Value& t = d.at(*tp, i);
            out.tempo.push_back({json::Document::num(d.get(t, "timeMs"), 0),
                                 json::Document::num(d.get(t, "bpm"), 120),
                                 json::Document::num(d.get(t, "beat"), 0)});
        }
    if (const json::Value* fo = d.get(r, "focus"))
        for (size_t i = 0; i < d.size(*fo); ++i) {
            const json::Value& t = d.at(*fo, i);
            out.focus.push_back({json::Document::num(d.get(t, "timeMs"), 0),
                                 uint8_t(json::Document::num(d.get(t, "side"), 0))});
        }
    if (const json::Value* se = d.get(r, "sections"))
        for (size_t i = 0; i < d.size(*se); ++i) {
            const json::Value& s = d.at(*se, i);
            out.sections.push_back({json::Document::num(d.get(s, "timeMs"), 0),
                                    json::Document::boolean(d.get(s, "mustHit"), true),
                                    json::Document::boolean(d.get(s, "gf"), false),
                                    json::Document::boolean(d.get(s, "alt"), false),
                                    json::Document::num(d.get(s, "beats"), 4),
                                    json::Document::num(d.get(s, "bpm"), 0)});
        }
    return true;
}

bool loadChart(const std::string& path, Chart& out, std::string& err) {
    Reader r;
    if (!r.open(path, err)) return false;
    const Info& info = r.info();
    // Refuse charts that cannot be materialized; streaming callers use
    // Reader::range directly.
    if (info.logicalEvents > (400ull << 20)) {
        err = "raid chart has " + std::to_string(info.logicalEvents) +
              " events; too large to materialize (use the streaming reader)";
        return false;
    }
    std::string meta = r.metaJson();
    Info minfo;
    minfo.version = info.version;
    minfo.logicalEvents = info.logicalEvents;
    minfo.chunkCount = info.chunkCount;
    minfo.patternCount = info.patternCount;
    if (!meta.empty()) {
        Chart mc;
        if (!chartFromMeta(meta, mc, minfo)) { err = "bad raid metadata"; return false; }
        out = std::move(mc);
    } else {
        out = Chart{};
    }
    out.rawNoteCount = info.logicalEvents;
    out.notes.reserve(size_t(std::min<uint64_t>(info.logicalEvents, 16ull << 20)));
    std::vector<Event> buf;
    for (size_t i = 0; i < info.chunkCount; ++i) {
        buf.clear();
        if (!r.chunkEvents(i, buf, err)) return false;
        for (const Event& e : buf) out.notes.push_back(eventToNote(e));
    }
    // finalize re-sorts, recomputes tempo beats/stats and merges any stacked
    // duplicates a foreign file may contain; it is idempotent on our own
    // output because events are already stored merged.
    double bpm = out.tempo.empty() ? 120 : out.tempo.front().bpm;
    out.finalize(bpm > 0 ? bpm : 120);
    return true;
}

// ------------------------------------------------------- streaming --------

bool ChartStream::open(const std::string& path, Chart& meta, std::string& err) {
    close();
    if (!reader_.open(path, err)) return false;
    const Info& info = reader_.info();
    meta = Chart{};
    std::string mj = reader_.metaJson();
    if (!mj.empty()) {
        Info minfo;
        minfo.version = info.version;
        minfo.logicalEvents = info.logicalEvents;
        minfo.chunkCount = info.chunkCount;
        minfo.patternCount = info.patternCount;
        Chart mc;
        if (!chartFromMeta(mj, mc, minfo)) { err = "bad raid metadata"; return false; }
        meta = std::move(mc);
    }
    meta.rawNoteCount = info.logicalEvents;
    if (meta.lengthMs <= 0) meta.lengthMs = double(reader_.durationUs()) / 1000.0;
    if (meta.keys <= 0) meta.keys = 4;
    if (meta.noteTypes.empty()) meta.noteTypes.push_back("");
    chart_ = meta;
    return true;
}

void ChartStream::close() {
    // In-flight decodes still touch the reader on worker threads. Wait for
    // every outstanding future before tearing down — closing with decodes
    // in flight races them against member destruction.
    for (auto& f : prefetchFuts_) f.wait();
    prefetchFuts_.clear();
    prefetchCoveredUs_ = INT64_MIN;
    reader_.close();
    chart_ = Chart{};
}

bool ChartStream::pump(int64_t startUs, int64_t endUs, std::vector<Event>& out,
                       std::vector<uint64_t>& indices, std::string& err) {
    harvestPrefetch();
    // Window-sliced decode: identical events/indices to range(), but a
    // cache miss expands only the requested window — with lazy pattern
    // evaluation a huge repeat instance costs O(repeats-in-window), not
    // O(repeats). rangeWindow self-selects whole-chunk decode+cache when
    // the window covers most of a chunk's span.
    return reader_.rangeWindow(startUs, endUs, out, indices, err);
}

void ChartStream::prefetch(int64_t startUs, int64_t endUs) {
    if (!reader_.isOpen() || endUs <= startUs) return;
    const Info& info = reader_.info();
    // Only enqueue chunks past the coverage high-water mark: prefetch
    // windows overlap, and re-requesting an already-warm or in-flight
    // chunk spends a whole decode for nothing. A chunk straddling the
    // mark may be requested twice at most.
    const int64_t freshLo = prefetchCoveredUs_ == INT64_MIN ? startUs
                                                           : std::max(startUs, prefetchCoveredUs_);
    prefetchCoveredUs_ = std::max(prefetchCoveredUs_, endUs);
    // Chunks overlapping [freshLo, endUs): first chunk with lastTimeUs >=
    // freshLo, through the last with firstTimeUs < endUs.
    size_t first = 0, last = size_t(info.chunkCount);
    // binary search on lastTimeUs via chunkAtOrBefore-style probing; the
    // reader does not expose per-chunk times for iteration, so walk the
    // index through chunkTimeRange (chunkCount is small enough that a
    // linear scan from the lower bound is fine — it is O(chunks in window)).
    // lower bound: first chunk whose lastTimeUs >= freshLo
    {
        size_t lo = 0, hi = size_t(info.chunkCount);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            int64_t f, l;
            if (!reader_.chunkTimeRange(mid, f, l)) return;
            if (l >= freshLo) hi = mid; else lo = mid + 1;
        }
        first = lo;
    }
    for (size_t i = first; i < size_t(info.chunkCount); ++i) {
        int64_t f, l;
        if (!reader_.chunkTimeRange(i, f, l)) return;
        if (f >= endUs) { last = i; break; }
        last = i + 1;
    }
    // Batch chunks per worker task: one std::async per chunk meant a
    // thread spawn (~0.1 ms) for every chunk in the window — hundreds of
    // spawns landed inside a single advance() and were the dominant
    // frame spike at high density. ~16 chunks/task keeps all workers
    // busy while cutting thread creation by an order of magnitude.
    constexpr size_t kPrefetchBatch = 16;
    for (size_t b = first; b < last; b += kPrefetchBatch)
        prefetchFuts_.push_back(
            reader_.warmChunksAsync(b, std::min(last, b + kPrefetchBatch)));
}

void ChartStream::harvestPrefetch() {
    // Reap finished futures individually so the outstanding set stays
    // bounded; decoded chunks live in the reader's LRU cache regardless
    // of the future's result.
    size_t w = 0;
    for (size_t i = 0; i < prefetchFuts_.size(); ++i) {
        if (prefetchFuts_[i].wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
            prefetchFuts_[i].get();
        } else {
            prefetchFuts_[w++] = std::move(prefetchFuts_[i]);
        }
    }
    prefetchFuts_.resize(w);
}

// ---------------------------------------------------------- PlaybackFeed --

bool PlaybackFeed::advance(double songMs, Session& session, int keys,
                           bool mirrorSides, std::string& err) {
    if (!isBound()) { err = "playback feed not bound"; return false; }
    const int64_t endUs = int64_t(std::max(0.0, songMs + kLookaheadMs) * 1000.0);
    lastPumpMs_ = lastConvertMs_ = lastGcMs_ = 0;
    // Top up the event buffer toward the lookahead horizon in bounded
    // time slices. Fetching the whole 4 s horizon in one pump() used to
    // decode every missing chunk synchronously on this thread — hundreds
    // of ms at high density. Slices preserve event order (contiguous,
    // appended in time order) while capping per-frame fetch work; the
    // horizon stays ahead of the playhead so judgement is unaffected.
    {
        const auto p0 = std::chrono::steady_clock::now();
        // Emergency floor: the buffer must always reach this far past the
        // playhead, however many slices that takes (bounded by
        // kMaxEmergencySlices). This keeps judgement supplied after seeks
        // and large song-clock steps; the 4 s horizon itself still fills
        // gradually at kMaxFetchSlices per call.
        const int64_t emergencyUs =
            int64_t(std::max(0.0, songMs) * 1000.0) + kEmergencyAheadUs;
        int slices = 0;
        while (fedUs_ < endUs &&
               (fedUs_ < emergencyUs ? slices < kMaxEmergencySlices
                                     : slices < kMaxFetchSlices)) {
            if (bufPos_ >= evsBuf_.size()) {
                evsBuf_.clear();
                idxBuf_.clear();
                bufPos_ = 0;
            }
            // fedUs_ starts at a negative sentinel ("from file start"), so
            // the first slice is anchored at 0 rather than fedUs_ + slice.
            const int64_t sliceEnd = fedUs_ < 0 ? std::min(endUs, kSliceUs)
                                                : std::min(endUs, fedUs_ + kSliceUs);
            const size_t before = evsBuf_.size();
            if (!stream_->pump(fedUs_, sliceEnd, evsBuf_, idxBuf_, err)) {
                lastPumpMs_ = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - p0).count();
                return false;
            }
            fedEvents_ += evsBuf_.size() - before;
            fedUs_ = sliceEnd;
            ++slices;
        }
        lastPumpMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - p0).count();
    }
    if (bufPos_ < evsBuf_.size()) {
        const auto c0 = std::chrono::steady_clock::now();
        const size_t stop = std::min(evsBuf_.size(), bufPos_ + kMaxConvert);
        for (; bufPos_ < stop; ++bufPos_) {
            const Event& e = evsBuf_[bufPos_];
            fedStack_ += std::max<uint16_t>(1, e.stack);
            LaneNote ln;
            ln.t = double(e.timeUs) / 1000.0;
            ln.len = double(e.lengthUs) / 1000.0;
            ln.kind = e.kind;
            ln.flags = e.flags;
            ln.type = e.type;
            ln.stack = std::max<uint16_t>(1, e.stack);
            ln.id = uint32_t(idxBuf_[bufPos_]);  // global expanded index (stable across rewind)
            const int side = int(e.side) ^ (mirrorSides ? 1 : 0);
            if (e.lane < uint32_t(keys)) {
                session.appendLaneNote(side, int(e.lane), ln);
                ++appendedEvents_;
            }
        }
        lastConvertMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - c0).count();
        if (bufPos_ >= evsBuf_.size()) {
            evsBuf_.clear();
            idxBuf_.clear();
            bufPos_ = 0;
        }
        const auto g0 = std::chrono::steady_clock::now();
        session.gcBefore(songMs);
        lastGcMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - g0).count();
    }
    if (endUs >= prefetchUs_) {
        // Prefetch far enough ahead for workers to keep up, but never so
        // far that the request window exceeds what the LRU can hold —
        // beyond that, tail chunks are evicted before use and the decode
        // work is wasted (and near chunks get evicted by far ones).
        // Half the event budget is reserved for the window itself.
        double prefetchMs = kPrefetchMs;
        const double fedSec = fedUs_ > 0 ? double(fedUs_) / 1e6 : 0.0;
        const uint64_t cap = stream_->cachedEventCapacity();
        if (fedSec > 1.0 && fedEvents_ > 0 && cap > 0) {
            const double evPerSec = double(fedEvents_) / fedSec;
            const double fitSec = double(cap / 2) / std::max(1.0, evPerSec);
            prefetchMs = std::min(kPrefetchMs, std::max(2000.0, fitSec * 1000.0));
        }
        // Anchor at 0: on the first call (and after resetPrefetch) this
        // warms the whole startup window on workers instead of leaving
        // the first ~4 s of chart for the pump to decode synchronously.
        // Later calls are unaffected — prefetchCoveredUs_ collapses the
        // request range to only the new tail.
        stream_->prefetch(int64_t(0), endUs + int64_t(prefetchMs * 1000.0));
        prefetchUs_ = endUs + int64_t(prefetchMs * 500.0);
    }
    return true;
}

}  // namespace gh::raid
