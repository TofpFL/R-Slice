#include "title_atlas.hpp"

#include <cstdio>

#include "chart.hpp"  // readTextFile
#include "json.hpp"

namespace gh {

const AtlasFrame* TitleAtlas::find(std::string_view name) const {
    const auto it = frames.find(std::string(name));
    return it == frames.end() ? nullptr : &it->second;
}

std::string frameName(const std::string& prefix, int index, int pad) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%0*d", prefix.c_str(), pad, index);
    return buf;
}

namespace {

bool attrValue(std::string_view tag, const char* key, std::string& out) {
    const std::string pat = std::string(key) + "=\"";
    const size_t p = tag.find(pat);
    if (p == std::string_view::npos) return false;
    const size_t b = p + pat.size();
    const size_t e = tag.find('"', b);
    if (e == std::string_view::npos) return false;
    out.assign(tag.substr(b, e - b));
    return true;
}

float attrFloat(std::string_view tag, const char* key, float def) {
    std::string v;
    if (!attrValue(tag, key, v) || v.empty()) return def;
    return std::strtof(v.c_str(), nullptr);
}

}  // namespace

bool loadAtlasXml(const std::string& pngPath, const std::string& xmlPath, TitleAtlas& out,
                  std::string* err) {
    TitleAtlas at;
    if (!FileExists(pngPath.c_str())) {
        if (err) *err = "missing " + pngPath;
        return false;
    }
    std::string xml;
    if (!readTextFile(xmlPath, xml)) {
        if (err) *err = "cannot read " + xmlPath;
        return false;
    }
    at.tex = LoadTexture(pngPath.c_str());
    if (!at.tex.id) {
        if (err) *err = "cannot load " + pngPath;
        return false;
    }
    // The title sprites are upscaled well past 1:1 on the canvas: mipmaps +
    // trilinear keep GF from shimmering/shredding when she dances.
    GenTextureMipmaps(&at.tex);
    SetTextureFilter(at.tex, TEXTURE_FILTER_TRILINEAR);
    size_t pos = 0;
    while ((pos = xml.find("<SubTexture", pos)) != std::string::npos) {
        const size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        const std::string_view tag(xml.data() + pos, end - pos);
        std::string name;
        if (attrValue(tag, "name", name)) {
            AtlasFrame f;
            f.src = {attrFloat(tag, "x", 0), attrFloat(tag, "y", 0),
                     attrFloat(tag, "width", 0), attrFloat(tag, "height", 0)};
            f.offsetX = attrFloat(tag, "frameX", 0);
            f.offsetY = attrFloat(tag, "frameY", 0);
            f.w = attrFloat(tag, "frameWidth", f.src.width);
            f.h = attrFloat(tag, "frameHeight", f.src.height);
            // Some shipped atlases declare frames wider than the PNG (the
            // titleEnter strip is authored at 1477 but stored at 1019):
            // sampling past the edge smears/repeats the art. Clamp instead.
            if (f.src.x < 0) { f.src.width += f.src.x; f.src.x = 0; }
            if (f.src.y < 0) { f.src.height += f.src.y; f.src.y = 0; }
            if (f.src.x + f.src.width > at.tex.width) f.src.width = at.tex.width - f.src.x;
            if (f.src.y + f.src.height > at.tex.height) f.src.height = at.tex.height - f.src.y;
            if (f.src.width <= 0 || f.src.height <= 0) continue;
            at.frames[name] = f;
        }
        pos = end + 1;
    }
    if (at.frames.empty()) {
        UnloadTexture(at.tex);
        if (err) *err = "no frames in " + xmlPath;
        return false;
    }
    out = std::move(at);
    return true;
}

bool loadAtlasJson(const std::string& pngPath, const std::string& jsonPath, TitleAtlas& out,
                   std::string* err) {
    TitleAtlas at;
    if (!FileExists(pngPath.c_str())) {
        if (err) *err = "missing " + pngPath;
        return false;
    }
    std::string text;
    if (!readTextFile(jsonPath, text)) {
        if (err) *err = "cannot read " + jsonPath;
        return false;
    }
    json::Document doc;
    if (!doc.parse(text, err)) return false;
    const json::Value& root = doc.root();
    const json::Value* atlas = doc.get(root, "ATLAS");
    const json::Value* sprites = atlas ? doc.get(*atlas, "SPRITES") : nullptr;
    if (!sprites || sprites->type != json::Type::Array) {
        if (err) *err = "bad atlas json " + jsonPath;
        return false;
    }
    at.tex = LoadTexture(pngPath.c_str());
    if (!at.tex.id) {
        if (err) *err = "cannot load " + pngPath;
        return false;
    }
    // The title sprites are upscaled well past 1:1 on the canvas: mipmaps +
    // trilinear keep GF from shimmering/shredding when she dances.
    GenTextureMipmaps(&at.tex);
    SetTextureFilter(at.tex, TEXTURE_FILTER_TRILINEAR);
    const size_t n = doc.size(*sprites);
    for (size_t i = 0; i < n; ++i) {
        const json::Value& wrap = doc.at(*sprites, i);  // {"SPRITE": {...}}
        const json::Value* sp = doc.get(wrap, "SPRITE");
        if (!sp) continue;
        std::string name(doc.strOr(doc.get(*sp, "name"), ""));
        if (name.empty()) continue;
        AtlasFrame f;
        f.src = {float(json::Document::num(doc.get(*sp, "x"), 0)),
                 float(json::Document::num(doc.get(*sp, "y"), 0)),
                 float(json::Document::num(doc.get(*sp, "w"), 0)),
                 float(json::Document::num(doc.get(*sp, "h"), 0))};
        f.w = f.src.width;
        f.h = f.src.height;
        if (f.src.x + f.src.width > at.tex.width) f.src.width = at.tex.width - f.src.x;
        if (f.src.y + f.src.height > at.tex.height) f.src.height = at.tex.height - f.src.y;
        if (f.src.width <= 0 || f.src.height <= 0) continue;
        at.frames[name] = f;
    }
    if (at.frames.empty()) {
        UnloadTexture(at.tex);
        if (err) *err = "no frames in " + jsonPath;
        return false;
    }
    out = std::move(at);
    return true;
}

}  // namespace gh
