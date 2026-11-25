// modchart.json parsing/writing/evaluation - see modchart.hpp for the schema.
#include "modchart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "json.hpp"

namespace gh {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// Bound helpers: json::Document's get/strOr are members, so parseModchart
// installs these as lambdas over its own Document `d`.
struct J {
    const json::Document& d;
    const json::Value* get(const json::Value& v, const char* key) const { return d.get(v, key); }
    std::string str(const json::Value* v, const std::string& def = "") const {
        return v ? std::string(d.strOr(v, def)) : def;
    }
};

// ---- the editable catalog -------------------------------------------------

const std::vector<std::pair<std::string, std::string>> kTargets = {
    {"camera", "Camera (playfield view)"}, {"hud", "HUD / receptors"},
    {"notes", "Notes"},                {"strums", "Receptors"},
    {"bf", "Boyfriend"},               {"dad", "Opponent"},
    {"gf", "Girlfriend"},              {"shader", "Shader clip (visuals.json)"},
};

const std::vector<std::pair<std::string, double>> kCameraProps = {
    {"zoom", 1}, {"x", 0}, {"y", 0}, {"angle", 0}, {"alpha", 1},
};
const std::vector<std::pair<std::string, double>> kHudProps = {
    {"zoom", 1}, {"x", 0}, {"y", 0}, {"angle", 0}, {"alpha", 1},
};
const std::vector<std::pair<std::string, double>> kNotesProps = {
    {"offsetX", 0}, {"offsetY", 0}, {"rotation", 0}, {"alpha", 1},
    {"scaleX", 1}, {"scaleY", 1}, {"multSpeed", 1},
};
const std::vector<std::pair<std::string, double>> kStrumProps = {
    {"offsetX", 0}, {"offsetY", 0}, {"scale", 1}, {"alpha", 1},
};
const std::vector<std::pair<std::string, double>> kCharProps = {
    {"x", 0}, {"y", 0}, {"angle", 0}, {"alpha", 1}, {"scaleX", 1}, {"scaleY", 1},
};

double numOf(const json::Value* v, double def) { return json::Document::num(v, def); }

double neutralOf(const std::string& target, const std::string& prop) {
    const auto& props = modProps(target);
    for (const auto& [n, v] : props)
        if (n == prop) return v;
    return 0;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& modTargets() { return kTargets; }

const std::vector<std::pair<std::string, double>>& modProps(const std::string& target) {
    static const std::vector<std::pair<std::string, double>> kEmpty;
    if (target == "camera" || target == "hud") return target == "camera" ? kCameraProps : kHudProps;
    if (target == "notes") return kNotesProps;
    if (target == "strums") return kStrumProps;
    if (target == "bf" || target == "dad" || target == "gf") return kCharProps;
    return kEmpty;
}

// ---- easing ----------------------------------------------------------------

static double ease01(const std::string& e, double u) {
    u = std::clamp(u, 0.0, 1.0);
    if (e == "easeIn") return u * u;
    if (e == "easeOut") return 1 - (1 - u) * (1 - u);
    if (e == "easeInOut") return u < 0.5 ? 2 * u * u : 1 - std::pow(-2 * u + 2, 2) / 2;
    if (e == "smooth") return u * u * (3 - 2 * u);
    return u;  // linear and anything unknown
}

double evalProp(const ModProp& p, double tMs) {
    const std::vector<ModKey>& k = p.keys;
    if (k.empty()) return p.neutral;
    if (tMs <= k.front().tMs) return tMs < k.front().tMs ? p.neutral : k.front().value;
    if (tMs >= k.back().tMs) return k.back().value;
    // Binary search for the surrounding keys.
    size_t lo = 0, hi = k.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (k[mid].tMs <= tMs) lo = mid; else hi = mid;
    }
    const ModKey& a = k[lo];
    const ModKey& b = k[hi];
    if (b.tMs <= a.tMs) return b.value;
    const double u = (tMs - a.tMs) / (b.tMs - a.tMs);
    return a.value + (b.value - a.value) * ease01(b.ease.empty() ? a.ease : b.ease, u);
}

std::map<std::string, double> evalTarget(const ModTrack& tr, double tMs) {
    std::map<std::string, double> out;
    for (const ModProp& p : tr.props) out[p.name] = evalProp(p, tMs);
    return out;
}

std::map<std::string, std::map<std::string, double>> evalAll(const Modchart& mc, double tMs) {
    std::map<std::string, std::map<std::string, double>> out;
    for (const ModTrack& tr : mc.tracks)
        if (tr.target != "shader") out[tr.target] = evalTarget(tr, tMs);
    return out;
}

// ---- parsing ----------------------------------------------------------------

bool parseModchart(const std::string& jsonText, Modchart& out, std::string& err) {
    out = Modchart{};
    json::Document d;
    if (!d.parse(jsonText, &err)) return false;
    const J j{d};
    if (const json::Value* ts = j.get(d.root(), "tracks")) {
        if (ts->type != json::Type::Array) { err = "modchart: \"tracks\" is not an array"; return false; }
        for (size_t i = 0; i < d.size(*ts); ++i) {
            const json::Value& t = d.at(*ts, i);
            if (t.type != json::Type::Object) continue;
            ModTrack tr;
            tr.target = j.str(j.get(t, "target"));
            if (tr.target.empty()) { err = "modchart track " + std::to_string(i) + ": no target"; return false; }
            if (tr.target == "shader") {
                if (const json::Value* c = j.get(t, "clip")) {
                    if (c->type == json::Type::Object) {
                        tr.clip.startMs = numOf(j.get(*c, "start"), numOf(j.get(*c, "t"), 0));
                        tr.clip.endMs = numOf(j.get(*c, "end"), numOf(j.get(*c, "dur"), 0));
                        tr.clip.target = j.str(j.get(*c, "target"), "game");
                        tr.clip.fx = j.str(j.get(*c, "fx"), j.str(j.get(*c, "shader")));
                        if (const json::Value* p = j.get(*c, "params"))
                            if (p->type == json::Type::Object)
                                for (size_t k = 0; k < d.size(*p); ++k) {
                                    const json::Value& pv = d.valAt(*p, k);
                                    if (pv.type == json::Type::Number || pv.type == json::Type::Bool)
                                        tr.clip.params[std::string(d.keyAt(*p, k))] = json::Document::num(&pv, 0);
                                }
                    }
                }
                if (tr.clip.fx.empty()) { err = "modchart shader track " + std::to_string(i) + ": no fx"; return false; }
            } else {
                const auto& catalog = modProps(tr.target);
                if (catalog.empty()) { err = "modchart track " + std::to_string(i) + ": unknown target '" + tr.target + "'"; return false; }
                if (const json::Value* ps = j.get(t, "props")) {
                    if (ps->type != json::Type::Object) { err = "modchart track " + std::to_string(i) + ": props not an object"; return false; }
                    for (size_t k = 0; k < d.size(*ps); ++k) {
                        const std::string pname(d.keyAt(*ps, k));
                        bool known = false;
                        for (const auto& [cn, nv] : catalog) known = known || cn == pname;
                        if (!known) continue;  // forward-compatible: skip unknown props
                        const json::Value& ks = d.valAt(*ps, k);
                        if (ks.type != json::Type::Array) continue;
                        ModProp mp;
                        mp.name = pname;
                        mp.neutral = neutralOf(tr.target, pname);
                        for (size_t ki = 0; ki < d.size(ks); ++ki) {
                            const json::Value& kv = d.at(ks, ki);
                            if (kv.type != json::Type::Object) continue;
                            ModKey key;
                            key.tMs = std::max(0.0, numOf(j.get(kv, "t"), numOf(j.get(kv, "time"), 0)));
                            key.value = numOf(j.get(kv, "v"), numOf(j.get(kv, "value"), 0));
                            key.ease = j.str(j.get(kv, "ease"), "linear");
                            mp.keys.push_back(key);
                        }
                        std::stable_sort(mp.keys.begin(), mp.keys.end(),
                                         [](const ModKey& a, const ModKey& b) { return a.tMs < b.tMs; });
                        tr.props.push_back(std::move(mp));
                    }
                }
            }
            out.tracks.push_back(std::move(tr));
        }
    }
    if (const json::Value* es = j.get(d.root(), "events")) {
        if (es->type == json::Type::Array)
            for (size_t i = 0; i < d.size(*es); ++i) {
                const json::Value& e = d.at(*es, i);
                if (e.type != json::Type::Object) continue;
                ModEvent ev;
                ev.tMs = std::max(0.0, numOf(j.get(e, "t"), numOf(j.get(e, "time"), 0)));
                ev.name = j.str(j.get(e, "name"), j.str(j.get(e, "event")));
                if (const json::Value* p = j.get(e, "params"))
                    if (p->type == json::Type::Object)
                        for (size_t k = 0; k < d.size(*p); ++k) {
                            const json::Value& pv = d.valAt(*p, k);
                            if (pv.type == json::Type::Number || pv.type == json::Type::Bool)
                                ev.params[std::string(d.keyAt(*p, k))] = json::Document::num(&pv, 0);
                        }
                if (!ev.name.empty()) out.events.push_back(std::move(ev));
            }
    }
    std::stable_sort(out.events.begin(), out.events.end(),
                     [](const ModEvent& a, const ModEvent& b) { return a.tMs < b.tMs; });
    return true;
}

// ---- writing -----------------------------------------------------------------

std::string modchartJson(const Modchart& mc) {
    std::string o = "{\n  \"version\": 1,\n  \"tracks\": [\n";
    for (size_t i = 0; i < mc.tracks.size(); ++i) {
        const ModTrack& tr = mc.tracks[i];
        o += "    { \"target\": \"" + tr.target + "\"";
        if (tr.target == "shader") {
            const VisualClip& c = tr.clip;
            o += ", \"clip\": { \"start\": " + std::to_string(c.startMs) +
                 ", \"end\": " + std::to_string(c.endMs) +
                 ", \"target\": \"" + c.target + "\", \"fx\": \"" + jsonEscape(c.fx) + "\"";
            if (!c.params.empty()) {
                o += ", \"params\": {";
                bool first = true;
                for (const auto& [k, v] : c.params) {
                    if (!first) o += ", ";
                    first = false;
                    o += "\"" + jsonEscape(k) + "\": " + std::to_string(v);
                }
                o += "}";
            }
            o += " }";
        } else {
            o += ", \"props\": {";
            bool fp = true;
            for (const ModProp& mp : tr.props) {
                if (mp.keys.empty()) continue;
                if (!fp) o += ", ";
                fp = false;
                o += "\"" + jsonEscape(mp.name) + "\": [";
                for (size_t j = 0; j < mp.keys.size(); ++j) {
                    const ModKey& k = mp.keys[j];
                    if (j) o += ", ";
                    o += "{\"t\": " + std::to_string(k.tMs) + ", \"v\": " + std::to_string(k.value);
                    if (k.ease != "linear") o += ", \"ease\": \"" + k.ease + "\"";
                    o += "}";
                }
                o += "]";
            }
            o += "}";
        }
        o += " }" + std::string(i + 1 < mc.tracks.size() ? "," : "") + "\n";
    }
    o += "  ],\n  \"events\": [\n";
    for (size_t i = 0; i < mc.events.size(); ++i) {
        const ModEvent& e = mc.events[i];
        o += "    { \"t\": " + std::to_string(e.tMs) + ", \"name\": \"" + jsonEscape(e.name) + "\"";
        if (!e.params.empty()) {
            o += ", \"params\": {";
            bool first = true;
            for (const auto& [k, v] : e.params) {
                if (!first) o += ", ";
                first = false;
                o += "\"" + jsonEscape(k) + "\": " + std::to_string(v);
            }
            o += "}";
        }
        o += " }" + std::string(i + 1 < mc.events.size() ? "," : "") + "\n";
    }
    o += "  ]\n}\n";
    return o;
}

}  // namespace gh
