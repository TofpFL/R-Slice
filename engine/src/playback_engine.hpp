// Playback engine abstraction — see docs/ICHART_SOURCE.md.
//
// Two engines behind one gameplay layer:
//   kJson — materialized JSON-family charts; the existing pipeline, untouched.
//           Session::init(chart_) does all note delivery; advance() is a no-op.
//   kRaid — .raid charts; ChartStream windowed decode + PlaybackFeed feeding
//           Session inside advance(), exactly as PlayState does today.
//
// The gameplay layer owns the Session object and its lifecycle; the engine
// owns feeding (decode -> convert -> append -> resolved-note GC).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "chart.hpp"
#include "raid_chart.hpp"
#include "session.hpp"

namespace gh {

enum class PlaybackEngineKind : uint8_t { kJson = 0, kRaid = 1 };

struct PlaybackEngineInfo {
    PlaybackEngineKind kind = PlaybackEngineKind::kJson;
    std::string format;          // "fnf-json", "psych-v1", "vslice", "raid", ...
    uint64_t logicalEvents = 0;  // total events if known, else 0
    uint64_t rawBytes = 0;       // file size on disk
    bool seekable = true;
};

class IPlaybackEngine {
public:
    virtual ~IPlaybackEngine() = default;

    virtual PlaybackEngineInfo info() const = 0;

    // Per-frame delivery: pump due notes into `session` lanes up to the
    // lookahead horizon, then GC resolved notes. Idempotent per nowMs.
    // `nowMs` is song time from the audio clock — passed in, never owned.
    virtual bool advance(double nowMs, Session& session, int keys,
                         bool mirrorSides, std::string& err) = 0;

    // Called right after session_.init(chart_, cfg_) on every (re)start.
    // RAID sets lane totals from the file header and resets feed cursors;
    // JSON is a no-op (init already refilled the lanes).
    virtual void onSessionInit(Session& session, const Chart& meta) {
        (void)session;
        (void)meta;
    }

    // Seek-back (JS Engine loopTheSong, practice rewind): restore pending
    // state for notes at/after t. Default = Session::rewindTo; the RAID feed
    // keeps its cursors — resident notes replay, matching JSON behavior.
    virtual void seek(double tMs, Session& session) { session.rewindTo(tMs); }

    // Feed diagnostics (RAID only; zero for JSON).
    virtual uint64_t fedEvents() const { return 0; }
    virtual uint64_t appendedEvents() const { return 0; }
    virtual double lastPumpMs() const { return 0; }
    virtual double lastConvertMs() const { return 0; }
    virtual double lastGcMs() const { return 0; }
};

// JSON-family charts: the existing materialized pipeline, untouched.
// Session::init(chart_) delivers every note up front; advance() is a no-op.
class JsonPlaybackEngine final : public IPlaybackEngine {
public:
    explicit JsonPlaybackEngine(std::string format = "fnf-json")
        : format_(std::move(format)) {}

    PlaybackEngineInfo info() const override {
        PlaybackEngineInfo i;
        i.kind = PlaybackEngineKind::kJson;
        i.format = format_;
        i.seekable = true;
        return i;
    }

    bool advance(double, Session&, int, bool, std::string&) override { return true; }

private:
    std::string format_;
};

// .raid charts: wraps ChartStream + PlaybackFeed unchanged. The engine owns
// the PlaybackFeed (bound to the stream at construction); PlayState keeps the
// shared_ptr<ChartStream> for diagnostics (pattern log, cache stats).
class RaidPlaybackEngine final : public IPlaybackEngine {
public:
    explicit RaidPlaybackEngine(std::shared_ptr<raid::ChartStream> stream);

    PlaybackEngineInfo info() const override;

    bool advance(double nowMs, Session& session, int keys,
                 bool mirrorSides, std::string& err) override;

    void onSessionInit(Session& session, const Chart& meta) override;

    raid::PlaybackFeed& feed() { return feed_; }

    uint64_t fedEvents() const override { return feed_.fedEvents(); }
    uint64_t appendedEvents() const override { return feed_.appendedEvents(); }
    double lastPumpMs() const override { return feed_.lastPumpMs(); }
    double lastConvertMs() const override { return feed_.lastConvertMs(); }
    double lastGcMs() const override { return feed_.lastGcMs(); }

private:
    std::shared_ptr<raid::ChartStream> stream_;
    raid::PlaybackFeed feed_;
};

}  // namespace gh
