// Video-backed chart visual mode: a .raid chart's notes scrolling over a
// user-supplied video background. Presentation only — no characters,
// stages, HUD, judgement UI or input. Notes still resolve through
// Session (botplay) so GC and lane storage behave exactly like gameplay.
#pragma once

#include <memory>
#include <string>

#include "app.hpp"
#include "gfx.hpp"
#include "raid_chart.hpp"
#include "session.hpp"
#include "video.hpp"

namespace gh {

class VChartState : public State {
public:
    VChartState(std::string songId, std::string diff, std::string videoPath);
    ~VChartState() override;
    bool wantsMenuMusic() const override { return false; }
    void enter() override;
    void update(double dt) override;
    void draw() override;

private:
    void lanes();  // receptors + scrolling notes + sustains

    std::string songId_, diff_, videoPath_;
    bool ready_ = false;
    std::string error_;

    std::shared_ptr<raid::ChartStream> stream_;  // .raid songs only (from prepare)
    raid::PlaybackFeed feed_;
    Session session_;
    Chart chart_;
    SessionConfig cfg_;
    VideoPlayer video_;
    Prepared prep_;

    double songMs_ = -1e9, leadMs_ = 0;
    bool done_ = false;
};

std::unique_ptr<State> makeVChart(const std::string& songId, const std::string& diff,
                                  const std::string& videoPath);

}  // namespace gh
