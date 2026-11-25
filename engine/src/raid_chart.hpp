// Bridge between the engine's Chart model and the RAID format: encode a
// Chart to .raid, decode a .raid back to a Chart, and the metadata JSON the
// format stores. See raid.hpp for the format itself.
#pragma once

#include <cstdint>
#include <algorithm>
#include <future>
#include <string>
#include <vector>

#include "chart.hpp"
#include "raid.hpp"

namespace gh { class Session; }

namespace gh::raid {

// Chart -> raid::Event list (time ms -> us, pitch unused by Chart -> -1).
std::vector<Event> chartEvents(const Chart& c);

// The meta JSON stored inside a .raid file for `chart` (no encoder stats —
// those are only known after encoding).
std::string chartMetaJson(const Chart& c);

// Writes a .raid file for `chart`. statsOut receives encoder statistics
// (logical vs literal events, stored vs raw bytes — the honest compression
// report). metaJson is built from the chart automatically.
bool saveChart(const std::string& path, const Chart& chart,
               const EncodeOptions& opt, Info* statsOut, std::string& err);

// Reads a .raid file into a Chart. Decodes every chunk (full
// materialization); refuses charts whose logical event count cannot fit in
// memory — playback code should use Reader::range instead.
bool loadChart(const std::string& path, Chart& out, std::string& err);

// Parses the raid meta JSON into chart fields (no events). Used by
// loadChart and by callers that only need headers.
bool chartFromMeta(const std::string& metaJson, Chart& out, Info& info);

// ------------------------------------------------------- streaming --------
// ChartStream is the playback-side adapter for .raid charts: it owns a
// Reader plus the metadata-only Chart (notes stay empty; lengthMs,
// playerNotes/opponentNotes/rawNoteCount are filled from the file). Notes
// are pulled in time windows via pump(), which wraps Reader::range with
// global event indices, and prefetch() warms upcoming chunks on worker
// threads. Nothing is materialized beyond the requested window.
class ChartStream {
public:
    // Opens the file and fills `meta` with chart metadata (title, tempo,
    // sections, note types, totals). meta.notes stays empty.
    bool open(const std::string& path, Chart& meta, std::string& err);
    void close();
    bool isOpen() const { return reader_.isOpen(); }

    const Chart& chart() const { return chart_; }
    // The underlying streaming reader (opLog, pattern logging, etc.).
    Reader& reader() { return reader_; }
    uint64_t logicalEvents() const { return reader_.logicalEvents(); }
    // Chart length in ms (last event time), 0 when the file is empty.
    double lengthMs() const { return double(reader_.durationUs()) / 1000.0; }

    // All events with timeUs in [startUs, endUs) plus their global indices.
    // Windows may be requested in any order; the reader's chunk cache makes
    // sequential playback cheap. `out`/`indices` are appended to.
    bool pump(int64_t startUs, int64_t endUs, std::vector<Event>& out,
              std::vector<uint64_t>& indices, std::string& err);

    // Warms the chunks overlapping [startUs, endUs) on worker threads so a
    // later pump() hits the LRU cache. Futures are reaped incrementally on
    // the next pump()/close(); decoded events live only in the cache.
    void prefetch(int64_t startUs, int64_t endUs);
    void harvestPrefetch();
    // Song restart / seek-back: forget coverage so already-passed chunks
    // get re-warmed (the LRU may have evicted them since).
    void resetPrefetch() { prefetchCoveredUs_ = INT64_MIN; }

    // Decode bounds forwarded to the underlying Reader.
    void setMaxConcurrentDecodes(int n) { reader_.setMaxConcurrentDecodes(n); }
    void setMemoryBudgetBytes(uint64_t bytes) { reader_.setMemoryBudgetBytes(bytes); }
    // Resident decoded-event count / capacity of the reader's LRU cache.
    uint64_t cachedEvents() const { return reader_.cachedEvents(); }
    uint64_t cachedEventCapacity() const { return reader_.cachedEventCapacity(); }

private:
    Reader reader_;
    Chart chart_;
    // Outstanding prefetch decodes (cache-warm futures; results land in
    // the reader's LRU — no per-chunk buffers are kept).
    std::vector<std::future<bool>> prefetchFuts_;
    // High-water mark of prefetch coverage: windows overlap by design, so
    // only chunks past this time are enqueued — otherwise every chunk in
    // the overlap is decoded twice (once per window), halving worker
    // throughput exactly when density is highest.
    int64_t prefetchCoveredUs_ = INT64_MIN;
};

// PlaybackFeed is the playback coordinator: it owns the feed cursors,
// lookahead/prefetch policy, Event->LaneNote conversion and session GC for
// a .raid chart, so playstate code only advances the song clock and calls
// advance(). Non-owning: bind() a ChartStream that outlives the feed.
//
// Responsibilities map onto the engine subsystems: Reader resolves pattern
// instances and windows (scheduler/resolver), ChartStream manages event
// windows + prefetch, Session owns note lifecycle — the feed only sequences
// them. Nothing here materializes more than one lookahead window.
class PlaybackFeed {
public:
    PlaybackFeed() = default;
    PlaybackFeed(const PlaybackFeed&) = delete;
    PlaybackFeed& operator=(const PlaybackFeed&) = delete;

    void bind(ChartStream& stream) { stream_ = &stream; reset(); }
    void unbind() { stream_ = nullptr; }
    bool isBound() const { return stream_ != nullptr && stream_->isOpen(); }

    // Song restart / seek-back: re-feeds from the start on next advance(),
    // matching a cleared Session. Call after session_.init()/setTotals().
    void reset() {
        if (stream_) stream_->resetPrefetch();
        fedUs_ = prefetchUs_ = std::numeric_limits<int64_t>::min() / 4;
        fedEvents_ = appendedEvents_ = fedStack_ = 0;
        bufPos_ = 0;
        evsBuf_.clear();
        idxBuf_.clear();
    }

    // Feeds every event in (fedUs, songMs + kLookaheadMs) into `session`
    // lanes as LaneNotes (mirror flips side; lanes >= keys are dropped;
    // stack clamps to >= 1; id = global expanded index), then GCs resolved
    // notes and prefetches the next kPrefetchMs on worker threads.
    // Idempotent per songMs — safe to call every frame.
    bool advance(double songMs, Session& session, int keys, bool mirrorSides,
                 std::string& err);

    // The pump horizon advance() will request for this song position —
    // for auxiliary consumers like PatternLogger that need the same bound.
    static int64_t horizonUs(double songMs) {
        return int64_t(std::max(0.0, songMs + kLookaheadMs) * 1000.0);
    }

    // Last advance() call's phase times (ms): stream pump, Event->LaneNote
    // conversion+append, Session GC. For frame-time diagnostics.
    double lastPumpMs() const { return lastPumpMs_; }
    double lastConvertMs() const { return lastConvertMs_; }
    double lastGcMs() const { return lastGcMs_; }

    uint64_t fedEvents() const { return fedEvents_; }
    uint64_t appendedEvents() const { return appendedEvents_; }
    // Stack-weighted note count fed so far — the "logical" multiplicity
    // credited for score/combo, vs fedEvents()' physical event count.
    uint64_t fedStackedNotes() const { return fedStack_; }

    // Decode bounds forwarded to the bound stream's Reader.
    void setMaxConcurrentDecodes(int n) { if (stream_) stream_->setMaxConcurrentDecodes(n); }
    void setMemoryBudgetBytes(uint64_t bytes) { if (stream_) stream_->setMemoryBudgetBytes(bytes); }
    uint64_t cachedEvents() const { return stream_ ? stream_->cachedEvents() : 0; }
    uint64_t cachedEventCapacity() const { return stream_ ? stream_->cachedEventCapacity() : 0; }

private:
    ChartStream* stream_ = nullptr;
    int64_t fedUs_ = 0, prefetchUs_ = 0;
    uint64_t fedEvents_ = 0, appendedEvents_ = 0, fedStack_ = 0;
    double lastPumpMs_ = 0, lastConvertMs_ = 0, lastGcMs_ = 0;
    // Reused pump scratch: avoids a fresh Event/index allocation every
    // frame; capacity persists across advance() calls. bufPos_ is the
    // drain cursor — a frame converts at most kMaxConvert events so a
    // catch-up pump (song clock jumped) spreads over frames instead of
    // spiking. Undrained events all sit >= ~4 s ahead of the playhead
    // (the pump horizon), so deferring them cannot delay judgement.
    std::vector<Event> evsBuf_;
    std::vector<uint64_t> idxBuf_;
    size_t bufPos_ = 0;
    static constexpr size_t kMaxConvert = 32768;
    // Pump fetches the lookahead horizon in bounded song-time slices so a
    // cold-cache decode burst spreads across frames instead of landing in
    // one pump() call. Order is preserved: slices are contiguous.
    static constexpr int64_t kSliceUs = 50000;
    static constexpr int kMaxFetchSlices = 3;
    // Per-call fetch may exceed kMaxFetchSlices only while the buffer
    // doesn't yet reach the playhead + this margin (seek/restart catch-up).
    static constexpr int64_t kEmergencyAheadUs = 500000;
    static constexpr int kMaxEmergencySlices = 16;
    // Judgement needs ~166 ms; 4 s covers judgement + on-screen scroll,
    // 16 s prefetch keeps worker decodes ahead of the pump horizon.
    static constexpr double kLookaheadMs = 4000.0;
    static constexpr double kPrefetchMs = 16000.0;
};

}  // namespace gh::raid
