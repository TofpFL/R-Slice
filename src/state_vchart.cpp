// Video-backed chart visual mode — see state_vchart.hpp.
#include "state_vchart.hpp"

#include <algorithm>
#include <cstdio>

#include "audio.hpp"
#include "gfx.hpp"

namespace gh {

VChartState::VChartState(std::string songId, std::string diff, std::string videoPath)
    : songId_(std::move(songId)), diff_(std::move(diff)), videoPath_(std::move(videoPath)) {
    cfg_.botplay = true;  // notes resolve so Session GC keeps lanes bounded
}

VChartState::~VChartState() {
    video_.close();
    app().mixer.stop();
}

void VChartState::enter() {
    App& a = app();
    a.stopMenuMusic();
    a.mixer.clear();
    const SongEntry* e = a.library.find(songId_);
    if (!e) {
        error_ = "Song not found: " + songId_;
        return;
    }
    const std::string diff = diff_.empty() && !e->diffs.empty() ? e->diffs[0] : diff_;
    prep_ = a.library.prepare(*e, diff, false, false);
    if (!prep_.ok) {
        error_ = prep_.error;
        return;
    }
    chart_ = prep_.chart;
    stream_ = prep_.stream;  // opened ChartStream for .raid songs, null for JSON
    leadMs_ = 60000.0 / std::max(1.0, chart_.bpmAt(0)) * 5.0;

    a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
    a.mixer.setRate(1.0);
    a.mixer.play(-leadMs_ / 1000.0);

    // Video: .mpg plays directly; other containers convert once through
    // ffmpeg into the user cache (same pipeline as cutscene videos).
    std::string verr;
    const std::string playable = ensurePlayableVideo(videoPath_, a.userDir, verr);
    if (playable.empty()) {
        error_ = verr.empty() ? "cannot play video: " + videoPath_ : verr;
        return;
    }
    if (!video_.open(playable, true, false)) {
        error_ = "cannot decode video: " + playable;
        return;
    }

    session_.init(chart_, cfg_);
    session_.setTotals(chart_.playerNotes, chart_.opponentNotes);
    if (stream_) feed_.bind(*stream_);
    ready_ = true;
}

void VChartState::update(double dt) {
    App& a = app();
    if (IsKeyPressed(KEY_ESCAPE)) { a.change(makeMainMenu()); return; }
    if (!ready_) {
        if (!error_.empty()) a.change(makeMainMenu());
        return;
    }
    const double now = a.now;
    songMs_ = a.mixer.time(now) * 1000.0 - a.settings.offsetMs;
    video_.update(dt);
    std::string err;
    if (stream_) {
        if (!feed_.advance(songMs_, session_, chart_.keys, false, err)) {
            error_ = err;
            a.change(makeMainMenu());
            return;
        }
    } else {
        // JSON chart: notes were materialized by session_.init(); just GC.
        session_.gcBefore(songMs_);
    }
    session_.update(songMs_);
    session_.events.clear();
    if (songMs_ > chart_.lengthMs + 4000.0) { a.change(makeMainMenu()); return; }
}

void VChartState::lanes() {
    const int K = std::min(chart_.keys, 9);
    if (K <= 0) return;
    const Settings& s = app().settings;
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    const float ns = std::min(112.0f, 500.0f / float(K)) * 0.94f;
    const double base = s.scrollSpeed > 0.001f ? s.scrollSpeed : chart_.speed;
    const double pxPerMs = 0.45 * base;
    const double reach = (H * 1.5 + ns) / pxPerMs;
    // Receptor rows: opponent notes rise to the top row, player notes fall
    // to the bottom row (same convention as PlayState's dirSign).
    const float oppY = H * 0.14f, playY = H * 0.86f;
    const float margin = std::min(W * 0.06f, 60.0f);
    const float laneW = (W - margin * 2.0f) / float(K);
    for (int side = 0; side < 2; ++side) {
        if (side == 0 && !s.opponentNotes) continue;
        const float recY = side == 0 ? oppY : playY;
        const float dirSign = side == 0 ? 1.0f : -1.0f;  // opp rises, player falls
        for (int lane = 0; lane < K; ++lane) {
            const float cx = margin + laneW * (float(lane) + 0.5f);
            const Vector2 c{cx, recY};
            const int dir = gfx::laneDir(K, lane);
            const Color col = gfx::dirColor(dir);
            gfx::drawReceptor(dir, c, ns, gfx::fade(WHITE, 0.9f), 1.0f, 0.0f);
            const std::deque<LaneNote>& v = session_.lane(side, lane);
            if (v.empty()) continue;
            const double tStart = songMs_ - reach * 0.5 - session_.maxSustain(side, lane);
            const double tEnd = songMs_ + reach;
            auto lo = std::lower_bound(v.begin(), v.end(), tStart,
                                       [](const LaneNote& n, double t) { return n.t < t; });
            auto hi = std::upper_bound(lo, v.end(), tEnd,
                                       [](double t, const LaneNote& n) { return t < n.t; });
            const float offY = (H + ns) * 0.5f;
            // Sustains first (under the note heads).
            for (auto it = lo; it != hi; ++it) {
                const LaneNote& n = *it;
                if (n.len <= 0) continue;
                if (n.state == kHeldDone || n.state == kHit || n.state == kMineHit ||
                    n.state == kMinePassed)
                    continue;
                const double endT = n.t + n.len;
                const bool active = n.state == kHolding;
                const double startT = active ? std::max(n.t, songMs_) : n.t;
                const float y0 = active ? recY : recY + dirSign * float((startT - songMs_) * pxPerMs);
                const float y1 = recY + dirSign * float((endT - songMs_) * pxPerMs);
                if ((dirSign > 0 && y1 < recY - offY) || (dirSign < 0 && y1 > recY + offY)) continue;
                gfx::drawSustain(dir, cx, y0, y1, ns * 0.3f, 0.9f, true);
            }
            // Note heads, newest-first with pixel dedup like drawLanes.
            float lastY = 1e9f;
            for (auto it = hi; it != lo;) {
                --it;
                const LaneNote& n = *it;
                if (n.state != kPending && n.state != kMissed && n.state != kHolding)
                    continue;
                const float y = recY + dirSign * float((n.t - songMs_) * pxPerMs);
                if (y < -ns || y > H + ns) continue;
                if (n.len <= 0 && std::fabs(y - lastY) < 3.0f) continue;
                lastY = y;
                const float al = n.state == kMissed ? 0.35f : 1.0f;
                gfx::drawNote(dir, {cx, y}, ns, al, 1.0f, 0.0f);
            }
        }
    }
}

void VChartState::draw() {
    const int W = GetScreenWidth(), H = GetScreenHeight();
    ClearBackground(BLACK);
    if (video_.texture().id) {
        // Cover-fit the frame: scale to cover, crop the overflow.
        const float vw = float(video_.width()), vh = float(video_.height());
        if (vw > 0 && vh > 0) {
            const float sc = std::max(float(W) / vw, float(H) / vh);
            const float dw = vw * sc, dh = vh * sc;
            const Rectangle src{0, 0, vw, vh};
            const Rectangle dst{(float(W) - dw) * 0.5f, (float(H) - dh) * 0.5f, dw, dh};
            DrawTexturePro(video_.texture(), src, dst, {0, 0}, 0.0f, WHITE);
        }
    }
    if (!ready_) {
        const char* msg = error_.empty() ? "Loading..." : error_.c_str();
        DrawText(msg, W / 2 - MeasureText(msg, 24) / 2, H / 2, 24, RAYWHITE);
        return;
    }
    lanes();
}

std::unique_ptr<State> makeVChart(const std::string& songId, const std::string& diff,
                                  const std::string& videoPath) {
    return std::make_unique<VChartState>(songId, diff, videoPath);
}

}  // namespace gh
