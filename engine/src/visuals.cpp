// visuals.json parsing/writing - see visuals.hpp for the schema.
#include "visuals.hpp"

#include <algorithm>
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

double numOf(const json::Value* v, double def) { return json::Document::num(v, def); }

}  // namespace

bool parseVisuals(const std::string& jsonText, std::vector<VisualClip>& out, std::string& err) {
    out.clear();
    json::Document d;
    if (!d.parse(jsonText, &err)) return false;
    const json::Value* clips = d.get(d.root(), "clips");
    if (!clips || clips->type != json::Type::Array) {
        err = "visuals.json: missing \"clips\" array";
        return false;
    }
    for (size_t i = 0; i < d.size(*clips); ++i) {
        const json::Value& c = d.at(*clips, i);
        if (c.type != json::Type::Object) continue;
        VisualClip vc;
        vc.startMs = numOf(d.get(c, "start"), numOf(d.get(c, "t"), 0));
        vc.endMs = numOf(d.get(c, "end"), numOf(d.get(c, "dur"), 0));
        if (const json::Value* v = d.get(c, "duration")) vc.endMs = vc.startMs + numOf(v, 0);
        vc.target = std::string(d.strOr(d.get(c, "target"), "game"));
        vc.fx = std::string(d.strOr(d.get(c, "fx"), d.strOr(d.get(c, "shader"), "")));
        if (vc.fx.empty()) { err = "visuals.json clip " + std::to_string(i) + ": no fx"; return false; }
        if (const json::Value* p = d.get(c, "params")) {
            if (p->type == json::Type::Object)
                for (size_t k = 0; k < d.size(*p); ++k) {
                    const json::Value& pv = d.valAt(*p, k);
                    if (pv.type == json::Type::Number || pv.type == json::Type::Bool)
                        vc.params[std::string(d.keyAt(*p, k))] = json::Document::num(&pv, 0);
                }
        }
        if (vc.startMs < 0) vc.startMs = 0;
        out.push_back(std::move(vc));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const VisualClip& a, const VisualClip& b) { return a.startMs < b.startMs; });
    return true;
}

std::string visualsJson(const std::vector<VisualClip>& clips) {
    std::string o = "{\n  \"version\": 1,\n  \"clips\": [\n";
    for (size_t i = 0; i < clips.size(); ++i) {
        const VisualClip& c = clips[i];
        o += "    { \"start\": " + std::to_string(c.startMs) +
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
        o += " }" + std::string(i + 1 < clips.size() ? "," : "") + "\n";
    }
    o += "  ]\n}\n";
    return o;
}

}  // namespace gh
