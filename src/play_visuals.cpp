// visuals.json playback: timed shader filters on the Psych scene cameras.
// Each clip resolves its `fx` to a shader source (built-in gh::fx effect or
// shaders/<name>.frag in the song's asset roots), wraps it in a
// ShaderInstance with the clip's float uniforms, and pushes it onto
// camGame/camHud.filters while songMs is inside [start, end).
#include "state_play.hpp"

#include "effects.hpp"
#include "scene.hpp"

namespace gh {

void PlayState::clearVisuals() {
    if (!scene_) return;
    for (VisualInst& v : visuals_) {
        auto& fg = scene_->camGame.filters;
        auto& fh = scene_->camHud.filters;
        if (v.game) fg.erase(std::remove(fg.begin(), fg.end(), v.game), fg.end());
        if (v.hud) fh.erase(std::remove(fh.begin(), fh.end(), v.hud), fh.end());
    }
    visuals_.clear();
}

scene::ShaderPtr PlayState::visualShader(const VisualClip& c) {
    scene::Scene& sc = *scene_;
    std::string name = c.fx;
    if (fx::source(name).empty() && name.rfind("__fx_", 0) != 0 && !fx::source("__fx_" + name).empty())
        name = "__fx_" + name;  // accept the short picker name too
    if (!fx::source(name).empty()) {
        fx::install(sc, name);
    } else if (!sc.shaderSources.count(name)) {
        // Custom shader: shaders/<name>.frag(.vert) in the asset roots, or a
        // file named directly in the song folder.
        scene::ShaderSource src;
        std::string frag = sc.files.find("shaders/" + name + ".frag");
        std::string vert = sc.files.find("shaders/" + name + ".vert");
        if (frag.empty() && (name.size() > 5 && (name.substr(name.size() - 5) == ".frag" ||
                                              name.substr(name.size() - 5) == ".glsl"))) {
            frag = sc.files.find(name);
            if (frag.empty() && !entry_->dataDir.empty()) {
                const std::string direct = entry_->dataDir + "/" + name;
                std::string text;
                if (scene::readFile(direct, text)) frag = direct;
            }
        }
        if (frag.empty() && !entry_->dataDir.empty()) {
            const std::string direct = entry_->dataDir + "/shaders/" + name + ".frag";
            if (scene::readFile(direct, src.frag)) frag = direct;
        }
        if (!frag.empty() && scene::readFile(frag, src.frag)) {
            if (!vert.empty()) scene::readFile(vert, src.vert);
            sc.shaderSources[name] = std::move(src);
        } else {
            debugNotice("visuals: no shader '" + name + "' (built-in or shaders/" + name + ".frag)", true, false);
            return nullptr;
        }
    }
    auto inst = std::make_shared<scene::ShaderInstance>();
    inst->name = name;
    inst->timeName = "uTime";
    inst->floats["uTime"] = {0.0f};
    inst->timeRate = 1.0f;  // Scene::update advances it with the song
    for (const auto& [k, v] : c.params) inst->floats[k] = {float(v)};
    return inst;
}

void PlayState::updateVisuals(double songMs) {
    if (!psych_ || !scene_ || chart_.visuals.empty()) return;
    scene::Scene& sc = *scene_;
    visuals_.resize(chart_.visuals.size());
    for (size_t i = 0; i < chart_.visuals.size(); ++i) {
        const VisualClip& c = chart_.visuals[i];
        VisualInst& v = visuals_[i];
        const bool open = c.endMs <= c.startMs || songMs < c.endMs;
        const bool active = songMs >= c.startMs && open;
        const bool onGame = active && (c.target == "game" || c.target == "both");
        const bool onHud = active && (c.target == "hud" || c.target == "both");
        if (!onGame && !onHud) {
            // Outside the clip: make sure nothing of it is left on a camera.
            if (v.game) {
                auto& f = sc.camGame.filters;
                f.erase(std::remove(f.begin(), f.end(), v.game), f.end());
                v.game.reset();
            }
            if (v.hud) {
                auto& f = sc.camHud.filters;
                f.erase(std::remove(f.begin(), f.end(), v.hud), f.end());
                v.hud.reset();
            }
            continue;
        }
        if (v.failed) continue;
        if (!v.game && !v.hud) {
            // First frame inside the clip: resolve the shader once and push
            // the same instance onto each requested camera.
            scene::ShaderPtr inst = visualShader(c);
            if (!inst) {
                v.failed = true;  // visualShader already reported why
                continue;
            }
            if (onGame) { v.game = inst; sc.camGame.filters.push_back(inst); }
            if (onHud) { v.hud = inst; sc.camHud.filters.push_back(inst); }
        }
    }
}

}  // namespace gh
