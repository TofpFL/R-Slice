// visuals.json: per-song visual effects timeline (the Visuals Editor).
//
// A .RAID (or chart JSON) only carries notes; the FX live beside it in the
// song folder, next to events.json:
//
//   {
//     "version": 1,
//     "clips": [
//       { "start": 12345.0, "end": 20345.0, "target": "game",
//         "fx": "__fx_glitch", "params": { "uStrength": 0.7 } }
//     ]
//   }
//
// Each clip turns one shader on for a stretch of the song: "game" is the
// playfield camera, "hud" the receptors/HUD camera, "both" chains it on
// each. `fx` names a built-in effect (gh::fx::source) or a .frag/.glsl file
// shipped with the song/mod. `params` sets float uniforms by name.
// `end` omitted or <= start means "until the song ends".
#pragma once

#include <map>
#include <string>
#include <vector>

namespace gh {

struct VisualClip {
    double startMs = 0;
    double endMs = 0;  // <= startMs: until the song ends
    std::string target = "game";  // game | hud | both
    std::string fx;                // built-in name or shader file
    std::map<std::string, double> params;  // float uniforms by name
};

bool parseVisuals(const std::string& jsonText, std::vector<VisualClip>& out, std::string& err);
std::string visualsJson(const std::vector<VisualClip>& clips);

}  // namespace gh
