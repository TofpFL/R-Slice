// modchart.json: per-song modifier timeline (the Modchart Editor).
//
// Codename/StepMania-style modcharting: timed tracks that drive object
// properties - camera, HUD, notes, receptors, characters - plus one-shot
// events and references to visuals.json shader clips. Lives beside
// visuals.json in the song folder; the .RAID stays notes-only.
//
//   {
//     "version": 1,
//     "tracks": [
//       { "target": "camera",
//         "props": {
//           "zoom":  [{"t": 4000, "v": 1.0}, {"t": 8000, "v": 1.6, "ease": "easeOut"}],
//           "angle": [{"t": 0, "v": 0}, {"t": 16000, "v": 360, "ease": "linear"}]
//         } },
//       { "target": "notes", "props": { "offsetY": [...] } },
//       { "target": "shader", "clip": { "start": 4000, "end": 9000,
//           "target": "game", "fx": "glitch", "params": {"uStrength": 0.8} } }
//     ],
//     "events": [ { "t": 12345, "name": "flash", "params": {"alpha": 0.8} } ]
//   }
//
// A "shader" track embeds a visuals.json clip so one timeline owns all FX.
// Property values interpolate between keys with the named easing; outside a
// property's key range the neutral value applies (zoom 1, alpha 1, rest 0).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "visuals.hpp"

namespace gh {

struct ModKey {
    double tMs = 0;
    double value = 0;
    std::string ease = "linear";  // linear | easeIn | easeOut | easeInOut | smooth
};

// One animatable property of a track target ("zoom", "offsetX", ...).
struct ModProp {
    std::string name;
    double neutral = 0;               // value outside the key range (catalog default)
    std::vector<ModKey> keys;         // sorted by tMs
};

struct ModTrack {
    std::string target;                 // camera | hud | notes | strums | bf | dad | gf | shader
    std::vector<ModProp> props;         // property tracks (empty for shader)
    VisualClip clip;                    // target == "shader": the embedded clip
    bool expanded = true;               // editor UI state, not saved
};

struct ModEvent {
    double tMs = 0;
    std::string name;
    std::map<std::string, double> params;
};

struct Modchart {
    std::vector<ModTrack> tracks;
    std::vector<ModEvent> events;  // sorted by tMs
};

// The editable property catalog per target (name -> neutral value).
const std::vector<std::pair<std::string, std::string>>& modTargets();
const std::vector<std::pair<std::string, double>>& modProps(const std::string& target);

bool parseModchart(const std::string& jsonText, Modchart& out, std::string& err);
std::string modchartJson(const Modchart& mc);

// Value of a property track at time tMs (neutral outside its key range).
double evalProp(const ModProp& p, double tMs);
// All properties of a target evaluated at tMs: name -> value.
std::map<std::string, double> evalTarget(const ModTrack& tr, double tMs);
// Every target's evaluated properties at tMs.
std::map<std::string, std::map<std::string, double>> evalAll(const Modchart& mc, double tMs);

}  // namespace gh
