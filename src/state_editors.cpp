// The editors' menu (main menu: 7), as Psych Engine's Master Editor Menu: the
// chart editor and the character, stage and event editors. They work on the
// mods the game found; the menu picks the mod they save into.
//
// Character editor: a Psych character JSON (characters/<name>.json): its
// animations and their offsets (the arrow keys, with the idle ghost behind),
// image, scale, flip, health icon and bar colour, position and camera point.
// Stage editor: a Psych stage JSON (stages/<name>.json): zoom, where the three
// characters stand and where the camera looks at each, shown over the stage
// itself (its JSON objects and its Lua script) with the characters dragged by
// the mouse. Event editor: custom events (custom_events/<name>.txt and .lua)
// from a template. Files keep the fields the editors do not know.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>

#include "app.hpp"
#include "basegame.hpp"
#include "common.hpp"
#include "editor_ui.hpp"
#include "effects.hpp"
#include "gfx.hpp"
#include "json.hpp"
#include "lua_host.hpp"
#include "process.hpp"
#include "psych.hpp"
#include "raylib.h"
#include "scene.hpp"
#include "scene_draw.hpp"
#include "visuals.hpp"

namespace gh {

namespace {
namespace fs = std::filesystem;
using namespace edui;
using gfx::fade;

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};
constexpr float kPanelX = 872, kPanelW = gfx::W - kPanelX - 8;

// The keys line at the bottom, as the menus show it.
void hints(const char* s) {
    if (app().touchMode) return;
    DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
    gfx::text(FontId::Ui, s, gfx::W / 2, gfx::H - 33, 20, Color{220, 214, 235, 255}, Align::Center);
}

// ------------------------------------------------------------------- mods --

struct ModChoice {
    std::string name;                // its folder in mods/ ("" = the mods folder itself)
    std::vector<std::string> roots;  // its asset folders; the editors save into roots[0]
};

std::vector<ModChoice> modChoices() {
    App& a = app();
    std::vector<ModChoice> out;
    for (const auto& [pack, roots] : a.library.packRoots())
        if (!pack.empty() && !roots.empty()) out.push_back({pack, roots});
    out.push_back({"", {a.base + "mods/"}});
    return out;
}

int& modIndex() {
    static int i = 0;
    return i;
}

ModChoice currentMod() {
    const std::vector<ModChoice> list = modChoices();
    int& i = modIndex();
    i = clampv(i, 0, int(list.size()) - 1);
    return list[size_t(i)];
}

std::string modLabel(const ModChoice& m) { return m.name.empty() ? std::string("mods/ (no mod)") : m.name; }

void useMod(scene::Scene& sc, const ModChoice& m) {
    std::vector<std::string> roots = m.roots;
    roots.push_back(app().base + "mods/");
    sc.files.setRoots(roots);
    sc.files.setSharedRoots(app().library.allAssetRoots());
}

// The files in `dir` ending in `ext`, by name: the chosen mod's first, then
// the ones other mods have.
std::vector<std::string> listNames(const scene::Scene& sc, const std::string& dir, const std::string& ext) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto scan = [&](const std::vector<std::string>& roots) {
        std::vector<std::string> found;
        for (const std::string& r : roots) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(toPath(r + dir), ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string n = pathStr(e.path().filename());
                if (!endsWithI(n, ext)) continue;
                n.resize(n.size() - ext.size());
                if (seen.insert(n).second) found.push_back(n);
            }
        }
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    };
    scan(sc.files.roots());
    scan(sc.files.sharedRoots());
    return out;
}

// ------------------------------------------------------------ JSON files --

std::string jnum(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string((long long)v);
    char b[48];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// A JSON value as Psych's editors write it: tabs, short arrays on one line.
void emit(const json::Document& d, const json::Value& v, std::string& o, int ind) {
    const std::string pad(size_t(ind), '\t');
    switch (v.type) {
        case json::Type::Null: o += "null"; break;
        case json::Type::Bool: o += v.b ? "true" : "false"; break;
        case json::Type::Number: o += jnum(v.num); break;
        case json::Type::String: o += jsonStr(std::string(d.str(v))); break;
        case json::Type::Array: {
            const size_t n = d.size(v);
            bool flat = true;
            for (size_t i = 0; i < n; ++i) {
                const json::Type t = d.at(v, i).type;
                flat = flat && t != json::Type::Array && t != json::Type::Object;
            }
            if (n == 0 || flat) {
                o += "[";
                for (size_t i = 0; i < n; ++i) {
                    if (i) o += ", ";
                    emit(d, d.at(v, i), o, ind);
                }
                o += "]";
                break;
            }
            o += "[\n";
            for (size_t i = 0; i < n; ++i) {
                o += pad + "\t";
                emit(d, d.at(v, i), o, ind + 1);
                o += i + 1 < n ? ",\n" : "\n";
            }
            o += pad + "]";
            break;
        }
        case json::Type::Object: {
            const size_t n = d.size(v);
            if (n == 0) {
                o += "{}";
                break;
            }
            o += "{\n";
            for (size_t i = 0; i < n; ++i) {
                o += pad + "\t" + jsonStr(std::string(d.keyAt(v, i))) + ": ";
                emit(d, d.valAt(v, i), o, ind + 1);
                o += i + 1 < n ? ",\n" : "\n";
            }
            o += pad + "}";
            break;
        }
    }
}

// An object: `fields` (key, JSON text) in their order, then the fields of the
// file as it was (`oldText`) that `fields` does not set.
std::string writeObject(const std::vector<std::pair<std::string, std::string>>& fields, const std::string& oldText) {
    std::vector<std::string> parts;
    std::set<std::string> have;
    for (const auto& [k, v] : fields) {
        parts.push_back("\t" + jsonStr(k) + ": " + v);
        have.insert(k);
    }
    json::Document d;
    if (!oldText.empty() && d.parse(oldText) && d.root().type == json::Type::Object) {
        const json::Value& r = d.root();
        for (size_t i = 0; i < d.size(r); ++i) {
            const std::string k(d.keyAt(r, i));
            if (have.count(k)) continue;
            std::string v;
            emit(d, d.valAt(r, i), v, 1);
            parts.push_back("\t" + jsonStr(k) + ": " + v);
        }
    }
    std::string o = "{\n";
    for (size_t i = 0; i < parts.size(); ++i) o += parts[i] + (i + 1 < parts.size() ? ",\n" : "\n");
    return o + "}\n";
}

std::string pair(double a, double b) { return "[" + jnum(a) + ", " + jnum(b) + "]"; }

bool saveText(const std::string& path, const std::string& text, std::string& err) {
    std::error_code ec;
    fs::create_directories(toPath(path).parent_path(), ec);
    std::ofstream f(toPath(path), std::ios::binary);
    if (!f) {
        err = "could not write " + path;
        return false;
    }
    f << text;
    if (!f) err = "could not write " + path;
    return bool(f);
}

// A safe file name for what the player typed.
std::string fileName(std::string s) {
    s = trim(s);
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '-';
    return s;
}

// ------------------------------------------------------------- characters --

bool loadCharacterDef(const scene::Scene& sc, const std::string& name, psych::CharacterDef& def, std::string& path, std::string& text,
                      std::string& err) {
    def = psych::CharacterDef{};
    text.clear();
    path = sc.files.find("characters/" + name + ".json");
    if (path.empty()) path = sc.files.find("data/characters/" + name + ".json");
    if (!path.empty()) {
        if (!scene::readFile(path, text)) {
            err = "could not read " + path;
            return false;
        }
        return psych::parseCharacter(text, def, err);
    }
    if (const char* base = basegame::characterJson(name)) {  // older engines' hard-coded characters
        path.clear();
        return psych::parseCharacter(base, def, err);
    }
    err = "characters/" + name + ".json not found";
    return false;
}

std::string imageKeys(const psych::CharacterDef& def) {
    std::string keys;
    for (const std::string& im : def.images) keys += (keys.empty() ? "" : ",") + im;
    return keys;
}

// The character as the play state builds it (Psych's Character), or nullptr
// when its image cannot be found.
scene::CharacterPtr buildCharacter(scene::Scene& sc, const psych::CharacterDef& def, bool isPlayer, std::string& err) {
    const std::string keys = imageKeys(def);
    scene::GraphicPtr g = keys.empty() ? nullptr : sc.gfx.atlas(keys);
    if (!g) {
        err = "image not found: " + (keys.empty() ? std::string("(none)") : keys);
        return nullptr;
    }
    auto c = std::make_shared<scene::Character>();
    c->kind = scene::Kind::Character;
    c->def = def;
    c->isPlayer = isPlayer;
    c->setGraphic(g);
    for (const psych::AnimDef& ad : def.anims) {
        scene::Anim a;
        a.name = ad.anim;
        a.frames = ad.indices.empty() ? psych::framesByPrefix(g->frames, ad.prefix) : psych::framesByIndices(g->frames, ad.prefix, ad.indices);
        if (a.frames.empty()) continue;
        a.fps = ad.fps;
        a.loop = ad.loop;
        a.hasOffset = true;
        a.offX = ad.offX;
        a.offY = ad.offY;
        c->addAnim(std::move(a));
    }
    if (def.scale != 1.0) {
        c->setGraphicSize(std::floor(c->width * float(def.scale)), 0);
        c->updateHitbox();
    }
    c->flipX = def.flipX != isPlayer;
    c->antialiasing = def.antialiasing;
    c->camX = def.camX;
    c->camY = def.camY;
    c->posX = def.posX;
    c->posY = def.posY;
    c->icon = def.icon;
    c->barColor = def.barColor;
    c->singDuration = def.singDuration;
    if (!def.anims.empty()) c->playAnim(def.anims[0].anim, true);
    return c;
}

// The animation names an atlas holds (frame names without their numbers).
std::vector<std::string> atlasPrefixes(const scene::Graphic& g) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const psych::AtlasFrame& f : g.frames) {
        std::string n = f.name;
        while (!n.empty() && std::isdigit((unsigned char)n.back())) n.pop_back();
        if (!n.empty() && seen.insert(n).second) out.push_back(n);
    }
    return out;
}

std::string iconPath(const scene::Scene& sc, const std::string& icon) {
    for (const std::string& rel : {"images/icons/icon-" + icon + ".png", "images/icons/" + icon + ".png", "images/icon-" + icon + ".png"}) {
        const std::string p = sc.files.find(rel);
        if (!p.empty()) return p;
    }
    return {};
}

// Psych's "Get Icon Color": the commonest colour of the icon's first frame,
// black and see-through pixels left out.
bool iconColor(const std::string& path, uint32_t& rgb) {
    Image img = LoadImage(path.c_str());
    if (!img.data) return false;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const int w = img.width >= img.height * 2 ? img.width / 2 : img.width;
    std::map<uint32_t, int> count;
    const unsigned char* px = static_cast<const unsigned char*>(img.data);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px + (size_t(y) * size_t(img.width) + size_t(x)) * 4;
            if (p[3] == 0) continue;
            const uint32_t c = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            if (c == 0) continue;
            ++count[c];
        }
    UnloadImage(img);
    int best = 0;
    for (const auto& [c, n] : count)
        if (n >= best) {
            best = n;
            rgb = c;
        }
    return best > 0;
}

// ------------------------------------------------------------- the view --

// What part of the world the editor shows (centre and zoom), through the
// game camera of its scene; dragged with the right mouse button, zoomed with
// the wheel.
struct View {
    float x = 640, y = 360, zoom = 1;
    bool dragging = false;
    Vector2 last{};

    void apply(scene::Camera& cam) const {
        cam.zoom = zoom;
        cam.scrollX = x - gfx::W * 0.5f;
        cam.scrollY = y - gfx::H * 0.5f;
    }
    Vector2 toWorld(Vector2 canvas) const { return {x + (canvas.x - gfx::W * 0.5f) / zoom, y + (canvas.y - gfx::H * 0.5f) / zoom}; }
    Vector2 toCanvas(float wx, float wy) const { return {gfx::W * 0.5f + (wx - x) * zoom, gfx::H * 0.5f + (wy - y) * zoom}; }
    void handle(const Input& in, const Ui& ui, bool overPanel) {
        const Vector2 m = ui.mouse;
        if (!overPanel && (in.mousePressed(MOUSE_BUTTON_RIGHT) || in.mousePressed(MOUSE_BUTTON_MIDDLE))) {
            dragging = true;
            last = m;
        }
        if (dragging) {
            if (!IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) dragging = false;
            x -= (m.x - last.x) / zoom;
            y -= (m.y - last.y) / zoom;
            last = m;
        }
        if (!overPanel && ui.wheel != 0 && !ui.used) {
            const Vector2 before = toWorld(m);
            zoom = clampv(zoom * (ui.wheel > 0 ? 1.1f : 1.0f / 1.1f), 0.05f, 8.0f);
            const Vector2 after = toWorld(m);
            x += before.x - after.x;
            y += before.y - after.y;
        }
    }
};

void drawGrid(const View& v) {
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{24, 20, 34, 255});
    const float step = 100 * v.zoom;
    if (step < 8) return;
    const Vector2 o = v.toCanvas(0, 0);
    for (float x = std::fmod(o.x, step); x < gfx::W; x += step) DrawLineEx({x, 0}, {x, gfx::H}, 1, Color{40, 34, 56, 255});
    for (float y = std::fmod(o.y, step); y < gfx::H; y += step) DrawLineEx({0, y}, {gfx::W, y}, 1, Color{40, 34, 56, 255});
    DrawLineEx({o.x, 0}, {o.x, gfx::H}, 1.5f, Color{70, 60, 100, 255});
    DrawLineEx({0, o.y}, {gfx::W, o.y}, 1.5f, Color{70, 60, 100, 255});
}

void cross(Vector2 c, float r, Color col) {
    DrawLineEx({c.x - r, c.y}, {c.x + r, c.y}, 3, BLACK);
    DrawLineEx({c.x, c.y - r}, {c.x, c.y + r}, 3, BLACK);
    DrawLineEx({c.x - r, c.y}, {c.x + r, c.y}, 1.5f, col);
    DrawLineEx({c.x, c.y - r}, {c.x, c.y + r}, 1.5f, col);
}

void panelBox(float y0, float y1) {
    DrawRectangleRounded({kPanelX - 6, y0, kPanelW + 12, y1 - y0}, 0.03f, 6, kPanel);
    DrawRectangleRoundedLinesEx({kPanelX - 6, y0, kPanelW + 12, y1 - y0}, 0.03f, 6, 1.5f, Color{110, 92, 150, 160});
}

void feedUi(Ui& ui) {
    App& a = app();
    ui.in = &a.input;
    ui.mouse = gfx::toCanvas(GetMousePosition());
    ui.click = a.input.mousePressed(MOUSE_BUTTON_LEFT) && !(a.touchMode && a.touch.covers(GetMousePosition()));
    ui.rclick = a.input.mousePressed(MOUSE_BUTTON_RIGHT);
    ui.wheel = a.input.wheel();
    ui.used = false;
}

bool overPanel(const Ui& ui) { return ui.mouse.x >= kPanelX - 8 || !ui.dropId.empty(); }

// Scripts in the editors: a stage's Lua builds its sprites; nothing else happens.
struct EditorGame : ScriptGame {
    double songPosition() const override { return 0; }
    bool getGameProp(std::string_view, scene::Value&) override { return false; }
    bool setGameProp(std::string_view, const scene::Value&) override { return false; }
    void triggerEvent(const std::string&, const std::string&, const std::string&) override {}
    void debugPrint(const std::string&, uint32_t) override {}
    void debugNotice(const std::string&, bool, bool) override {}
    bool shadersEnabled() override { return false; }
};

// A line of news at the bottom of an editor.
struct Status {
    std::string text;
    double at = -10;
    bool bad = false;
    void set(const std::string& t, bool error = false) {
        text = t;
        bad = error;
        at = app().now;
    }
    void draw() const {
        if (app().now - at > 6 || text.empty()) return;
        const float al = float(std::min(1.0, 6 - (app().now - at)));
        const float w = gfx::measure(FontId::Ui, text, 18).x;
        DrawRectangleRec({12, gfx::H - 70, w + 20, 28}, fade(BLACK, 0.7f * al));
        gfx::text(FontId::Ui, text, 22, gfx::H - 66, 18, fade(bad ? Color{255, 120, 120, 255} : kGold, al));
    }
};

// ============================================================ editor menu ==

class EditorMenuState : public State {
public:
    void update(double) override {
        App& a = app();
        Input& in = a.input;
        const int mods = int(modChoices().size());
        if (in.up()) { sel_ = (sel_ + 5) % 6; sfxPlay(Sfx::Scroll); }
        if (in.down()) { sel_ = (sel_ + 1) % 6; sfxPlay(Sfx::Scroll); }
        if ((in.left() || in.right()) && mods > 1) {
            int& m = modIndex();
            m = ((m + (in.left() ? -1 : 1)) % mods + mods) % mods;
            sfxPlay(Sfx::Scroll);
        }
        if (in.back()) {
            sfxPlay(Sfx::Cancel);
            a.change(makeMainMenu());
            return;
        }
        if (!in.accept()) return;
        sfxPlay(Sfx::Confirm);
        switch (sel_) {
            case 0: openChartEditor(); break;
            case 1: a.change(makeCharacterEditor()); break;
            case 2: a.change(makeStageEditor()); break;
            case 3: a.change(makeEventEditor()); break;
            case 4: a.change(makeVisualsEditor()); break;
            default: a.change(makeModchartEditor()); break;
        }
    }
    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x6C8CFF, menuPulse());
        gfx::text(FontId::Title, "EDITORS", gfx::W / 2, 60, 64, kGold, Align::Center, 2.0f);
        static const char* items[6] = {"Chart Editor", "Character Editor", "Stage Editor", "Event Editor", "Visuals Editor", "Modchart Editor"};
        static const char* about[6] = {"A song of this mod (the one chosen in Freeplay if it is here)",
                                       "Animations, offsets, image, icon and colours of a character",
                                       "Zoom, where the characters stand and what the camera sees",
                                       "New events for charts: a description and a Lua script",
                                       "Timed shader/FX clips for a song: post effects on playfield and receptors",
                                       "Codename-style modifier tracks: cameras, characters, notes, receptors and shader clips"};
        for (int i = 0; i < 6; ++i) {
            const bool on = i == sel_;
            const float y = 175.0f + i * 80.0f;
            gfx::text(FontId::Title, items[i], gfx::W / 2, y, on ? 54.0f : 44.0f, on ? kGold : fade(kIvory, 0.55f), Align::Center, 2.0f);
            if (on) gfx::text(FontId::Ui, about[i], gfx::W / 2, y + 58, 20, fade(kIvory, 0.85f), Align::Center, 1.5f);
        }
        const ModChoice m = currentMod();
        gfx::text(FontId::Ui, "<  Mod: " + modLabel(m) + "  >", gfx::W / 2, gfx::H - 112, 26, kIvory, Align::Center, 1.5f);
        gfx::text(FontId::Ui, "The editors save into " + m.roots[0], gfx::W / 2, gfx::H - 80, 16, fade(kIvory, 0.6f), Align::Center, 1.0f);
        hints("UP/DOWN choose   LEFT/RIGHT mod   ENTER open   ESC back");
        gfx::endCanvas();
    }
    TouchLayout touchLayout() const override { return {}; }

private:
    int sel_ = 0;

    void openChartEditor() {
        App& a = app();
        const ModChoice m = currentMod();
        const auto& songs = a.library.songs();
        const SongEntry* pick = nullptr;
        if (a.freeplaySel >= 0 && a.freeplaySel < int(songs.size())) {
            const SongEntry& e = songs[size_t(a.freeplaySel)];
            if (!e.builtin && (m.name.empty() || e.pack == m.name)) pick = &e;
        }
        for (size_t i = 0; !pick && i < songs.size(); ++i)
            if (!songs[i].builtin && !songs[i].diffs.empty() && (m.name.empty() || songs[i].pack == m.name)) pick = &songs[i];
        if (!pick || pick->diffs.empty()) {
            a.notify("No song in this mod to chart: choose one in Freeplay and press 7 there");
            a.change(makeFreeplay());
            return;
        }
        const int d = clampv(a.freeplayDiff, 0, int(pick->diffs.size()) - 1);
        a.change(makeChartEditor(pick->id, pick->diffs[size_t(d)]));
    }
};

// ======================================================= character editor ==

class CharacterEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        useMod(sc_, mod_);
        names_ = listNames(sc_, "characters/", ".json");
        auto it = std::find(names_.begin(), names_.end(), "bf");
        load(it != names_.end() ? *it : names_.empty() ? std::string("bf") : names_[0]);
    }
    bool keyReserved(int) const override { return !ui_.focus.empty(); }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (ch_) ch_->updateAnim(dt);
        view_.handle(in, ui_, overPanel(ui_));
        if (!ui_.focus.empty()) return;
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        if (in.pressed(KEY_ESCAPE)) {
            if (dirty_ && !leaveArmed_) {
                status_.set("Unsaved changes: ESC again to leave without saving (CTRL+S saves)");
                leaveArmed_ = true;
            } else {
                a.change(makeEditorMenu());
            }
            return;
        }
        if (ctrl && in.pressed(KEY_S)) save();
        const int n = int(def_.anims.size());
        if (n > 0 && (in.pressed(KEY_W) || in.pressed(KEY_S)) && !ctrl) selectAnim((anim_ + (in.pressed(KEY_W) ? n - 1 : 1)) % n);
        if (in.pressed(KEY_SPACE)) playSelected();
        if (in.pressed(KEY_G)) ghostOn_ = !ghostOn_;
        if (in.pressed(KEY_R) && !ctrl) resetView();
        const int step = shift ? 10 : 1;
        int dx = 0, dy = 0;
        if (in.pressed(KEY_LEFT)) dx += step;  // Psych: the arrows move the picture, its offset goes the other way
        if (in.pressed(KEY_RIGHT)) dx -= step;
        if (in.pressed(KEY_UP)) dy += step;
        if (in.pressed(KEY_DOWN)) dy -= step;
        if ((dx || dy) && anim_ < n) {
            psych::AnimDef& ad = def_.anims[size_t(anim_)];
            ad.offX += float(dx);
            ad.offY += float(dy);
            offsetsChanged();
        }
    }

    void draw() override {
        gfx::beginCanvas();
        view_.apply(sc_.camGame);
        drawGrid(view_);
        rend_.drawCamera(sc_, scene::Cam::Game, nullptr, nullptr);
        if (ch_) {
            // Where the character stands (its position) and where the camera looks.
            cross(view_.toCanvas(ch_->x, ch_->y), 10, Color{120, 200, 255, 255});
            const float mx = ch_->x + ch_->width * 0.5f, my = ch_->y + ch_->height * 0.5f;
            const Vector2 camPt = player_ ? Vector2{mx - 100 - def_.camX, my - 100 + def_.camY} : Vector2{mx + 150 + def_.camX, my - 100 + def_.camY};
            cross(view_.toCanvas(camPt.x, camPt.y), 16, kGold);
        } else {
            gfx::text(FontId::Ui, error_.empty() ? "No character" : error_, 430, 340, 22, Color{255, 140, 140, 255}, Align::Center, 1.5f);
        }
        drawInfo();
        drawPanel();
        status_.draw();
        gfx::endCanvas();
    }

private:
    scene::Scene sc_;
    SceneRenderer rend_;
    Ui ui_;
    View view_;
    Status status_;
    ModChoice mod_;
    std::vector<std::string> names_;
    int nameIdx_ = 0;
    psych::CharacterDef def_;
    std::string loadedPath_, loadedText_, saveName_, image_, icon_, error_;
    scene::CharacterPtr ch_, ghost_;
    std::vector<std::string> prefixes_;
    int anim_ = 0, tab_ = 0, prefixIdx_ = 0;
    bool player_ = true, ghostOn_ = true, dirty_ = false, leaveArmed_ = false;
    std::string fName_, fPrefix_, fIndices_;
    double fFps_ = 24;
    bool fLoop_ = false;
    float listScroll_ = 0;

    void load(const std::string& name) {
        std::string err;
        if (!loadCharacterDef(sc_, name, def_, loadedPath_, loadedText_, err)) {
            status_.set(err, true);
            if (def_.images.empty()) def_.images = {"characters/" + name};
        }
        saveName_ = name;
        auto it = std::find(names_.begin(), names_.end(), name);
        nameIdx_ = it == names_.end() ? 0 : int(it - names_.begin());
        player_ = name.rfind("bf", 0) == 0 || name.find("player") != std::string::npos || name.find("boyfriend") != std::string::npos;
        anim_ = 0;
        dirty_ = leaveArmed_ = false;
        syncFields();
        rebuild();
        resetView();
    }

    void syncFields() {
        image_ = imageKeys(def_);
        icon_ = def_.icon;
        fillAnimFields();
    }

    void fillAnimFields() {
        if (anim_ >= int(def_.anims.size())) return;
        const psych::AnimDef& ad = def_.anims[size_t(anim_)];
        fName_ = ad.anim;
        fPrefix_ = ad.prefix;
        fFps_ = ad.fps;
        fLoop_ = ad.loop;
        fIndices_.clear();
        for (size_t i = 0; i < ad.indices.size(); ++i) fIndices_ += (i ? "," : "") + std::to_string(ad.indices[i]);
    }

    // Base positions as in a song: the player at the stage's (770, 100), the opponent at (100, 100).
    Vector2 basePos() const { return player_ ? Vector2{770, 100} : Vector2{100, 100}; }

    void rebuild() {
        error_.clear();
        ch_ = buildCharacter(sc_, def_, player_, error_);
        ghost_.reset();
        sc_.order.clear();
        prefixes_.clear();
        if (!ch_) return;
        const Vector2 b = basePos();
        ch_->x = b.x + def_.posX;
        ch_->y = b.y + def_.posY;
        if (ch_->graphic) prefixes_ = atlasPrefixes(*ch_->graphic);
        // The ghost: the first animation's first frame, faint, to line the others up against.
        ghost_ = buildCharacter(sc_, def_, player_, error_);
        if (ghost_) {
            ghost_->x = ch_->x;
            ghost_->y = ch_->y;
            ghost_->alpha = 0.33f;
            ghost_->paused = true;
            ghost_->visible = ghostOn_;
            sc_.order.push_back(ghost_);
        }
        sc_.order.push_back(ch_);
        playSelected();
    }

    void resetView() {
        if (ch_) {
            view_.x = ch_->x + ch_->width * 0.5f;
            view_.y = ch_->y + ch_->height * 0.5f;
        } else {
            view_.x = 640;
            view_.y = 360;
        }
        view_.zoom = 1;
        view_.x -= (kPanelX - gfx::W) * 0.5f / view_.zoom;  // centred in the part left of the panel
    }

    void selectAnim(int i) {
        anim_ = clampv(i, 0, std::max(0, int(def_.anims.size()) - 1));
        fillAnimFields();
        playSelected();
    }

    void playSelected() {
        if (!ch_ || anim_ >= int(def_.anims.size())) return;
        ch_->playAnim(def_.anims[size_t(anim_)].anim, true);
        if (ghost_) ghost_->visible = ghostOn_;
    }

    void offsetsChanged() {
        dirty_ = true;
        leaveArmed_ = false;
        if (!ch_ || anim_ >= int(def_.anims.size())) return;
        const psych::AnimDef& ad = def_.anims[size_t(anim_)];
        for (scene::CharacterPtr c : {ch_, ghost_}) {
            if (!c) continue;
            if (scene::Anim* a = c->anim(ad.anim)) {
                a->offX = ad.offX;
                a->offY = ad.offY;
            }
            if (c->curAnimName() == ad.anim) {
                c->offsetX = ad.offX;
                c->offsetY = ad.offY;
            }
        }
    }

    void changed() {
        dirty_ = true;
        leaveArmed_ = false;
        rebuild();
    }

    void drawInfo() {
        std::string l1 = "Animation: none";
        if (anim_ < int(def_.anims.size())) {
            const psych::AnimDef& ad = def_.anims[size_t(anim_)];
            l1 = "Animation: " + ad.anim + "   offset [" + jnum(ad.offX) + ", " + jnum(ad.offY) + "]";
            if (ch_ && !ch_->hasAnim(ad.anim)) l1 += "   (no frames named \"" + ad.prefix + "\" in the image)";
        }
        DrawRectangleRec({8, 8, 856, 56}, fade(BLACK, 0.55f));
        gfx::text(FontId::Ui, l1, 18, 13, 20, kInk);
        gfx::text(FontId::Ui, saveName_ + (dirty_ ? "  (not saved)" : "") + "   mod: " + modLabel(mod_), 18, 38, 16, kDim);
        hints("W/S animation  SPACE play  ARROWS offset (SHIFT x10)  G ghost  RIGHT-DRAG move  WHEEL zoom  R reset  CTRL+S save");
    }

    void drawPanel() {
        panelBox(8, gfx::H - 40);
        float y = 16;
        const float x = kPanelX, w = kPanelW, rh = 30;
        if (ui_.button({x, y, w * 0.5f - 4, rh}, "Character", tab_ == 0)) tab_ = 0;
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh}, "Animations", tab_ == 1)) tab_ = 1;
        y += rh + 12;
        if (tab_ == 0) drawCharacterTab(x, y, w, rh);
        else drawAnimTab(x, y, w, rh);
        ui_.overlays();
    }

    void drawCharacterTab(float x, float y, float w, float rh) {
        if (!names_.empty()) {
            if (ui_.dropdown("char", {x, y, w, rh}, "Character", names_, nameIdx_)) load(names_[size_t(nameIdx_)]);
            y += rh + 6;
        }
        ui_.text("save", {x, y, w, rh}, "Save as", saveName_);
        y += rh + 6;
        ui_.text("image", {x, y, w - 92, rh}, "Image", image_);
        if (ui_.button({x + w - 86, y, 86, rh}, "Reload")) {
            def_.images.clear();
            for (std::string part; const char ch : image_ + ",") {
                if (ch == ',') {
                    if (!trim(part).empty()) def_.images.push_back(trim(part));
                    part.clear();
                } else {
                    part += ch;
                }
            }
            changed();
        }
        y += rh + 6;
        if (ui_.text("icon", {x, y, w - 92, rh}, "Health icon", icon_)) {
            def_.icon = trim(icon_);
            dirty_ = true;
        }
        const std::string ip = iconPath(sc_, def_.icon);
        if (Texture2D* t = ip.empty() ? nullptr : rend_.texture(ip)) {
            const float fw = t->width >= t->height * 2 ? t->width / 2.0f : float(t->width);
            DrawTexturePro(*t, {0, 0, fw, float(t->height)}, {x + w - 80, y - 14, 58, 58 * t->height / fw}, {0, 0}, 0, WHITE);
        }
        y += rh + 22;
        double v = def_.scale;
        if (ui_.stepper({x, y, w, rh}, "Scale", v, 0.05, 0.05, 20, "%.2f")) {
            def_.scale = v;
            changed();
        }
        y += rh + 6;
        bool flip = def_.flipX, noAa = !def_.antialiasing;
        if (ui_.check({x, y, w * 0.5f, rh}, "Flip X", flip)) {
            def_.flipX = flip;
            changed();
        }
        if (ui_.check({x + w * 0.5f, y, w * 0.5f, rh}, "No antialiasing", noAa)) {
            def_.antialiasing = !noAa;
            changed();
        }
        y += rh + 6;
        auto posStep = [&](const char* label, float& f) {
            double d = f;
            if (ui_.stepper({x, y, w, rh}, label, d, 1, -10000, 10000, "%.0f")) {
                f = float(d);
                changed();
            }
            y += rh + 6;
        };
        posStep("Position X", def_.posX);
        posStep("Position Y", def_.posY);
        posStep("Camera X", def_.camX);
        posStep("Camera Y", def_.camY);
        double sd = def_.singDuration;
        if (ui_.stepper({x, y, w, rh}, "Sing duration", sd, 0.1, 0, 100, "%.1f")) {
            def_.singDuration = sd;
            dirty_ = true;
        }
        y += rh + 6;
        // Health bar colour: red, green, blue, and the icon's own colour.
        double rgb[3] = {double(def_.barColor >> 16 & 255), double(def_.barColor >> 8 & 255), double(def_.barColor & 255)};
        static const char* names[3] = {"Bar red", "Bar green", "Bar blue"};
        for (int i = 0; i < 3; ++i) {
            if (ui_.stepper({x, y, w - 46, rh}, names[i], rgb[i], 1, 0, 255, "%.0f")) {
                def_.barColor = uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | uint32_t(rgb[2]);
                dirty_ = true;
            }
            y += rh + 4;
        }
        DrawRectangleRounded({x + w - 40, y - 3 * (rh + 4), 40, 3 * rh + 8}, 0.2f, 4, gfx::rgb(def_.barColor));
        if (ui_.button({x, y, w * 0.5f - 4, rh}, "Colour from icon")) {
            uint32_t c = 0;
            if (!ip.empty() && iconColor(ip, c)) {
                def_.barColor = c;
                dirty_ = true;
            } else {
                status_.set("No icon to take a colour from", true);
            }
        }
        bool pl = player_;
        if (ui_.check({x + w * 0.5f + 4, y, w * 0.5f, rh}, "As the player", pl)) {
            player_ = pl;
            rebuild();
        }
        y += rh + 10;
        if (ui_.button({x, y, w * 0.5f - 4, rh + 4}, "Save")) save();
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh + 4}, "New character")) newCharacter();
    }

    void drawAnimTab(float x, float y, float w, float rh) {
        // The animations: a click picks and plays one.
        const float listH = 210;
        DrawRectangleRec({x, y, w, listH}, Color{20, 14, 32, 255});
        const int n = int(def_.anims.size());
        const float row = 24;
        const int rows = int(listH / row);
        if (CheckCollisionPointRec(ui_.mouse, {x, y, w, listH}) && ui_.wheel != 0) {
            listScroll_ = clampv(listScroll_ - ui_.wheel * 2, 0.0f, float(std::max(0, n - rows)));
            ui_.used = true;
        }
        const int first = int(listScroll_);
        for (int k = 0; k < rows && first + k < n; ++k) {
            const int i = first + k;
            const Rectangle r{x + 2, y + 2 + k * row, w - 4, row - 2};
            const bool hov = ui_.hover(r);
            if (i == anim_ || hov) DrawRectangleRec(r, i == anim_ ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
            const psych::AnimDef& ad = def_.anims[size_t(i)];
            const bool ok = ch_ && ch_->hasAnim(ad.anim);
            gfx::text(FontId::Ui, ad.anim, r.x + 6, r.y + 2, 18, ok ? kInk : Color{255, 130, 130, 255});
            gfx::text(FontId::Ui, "[" + jnum(ad.offX) + ", " + jnum(ad.offY) + "]", r.x + r.width - 8, r.y + 2, 16, kDim, Align::Right);
            if (hov && ui_.click) {
                ui_.used = true;
                selectAnim(i);
            }
        }
        y += listH + 10;
        ui_.text("aname", {x, y, w, rh}, "Name", fName_);
        y += rh + 6;
        ui_.text("aprefix", {x, y, w, rh}, "Prefix", fPrefix_);
        y += rh + 6;
        if (!prefixes_.empty()) {
            if (ui_.dropdown("prefixes", {x, y, w, rh}, "In image", prefixes_, prefixIdx_)) fPrefix_ = prefixes_[size_t(prefixIdx_)];
            y += rh + 6;
        }
        ui_.stepper({x, y, w, rh}, "FPS", fFps_, 1, 1, 240, "%.0f");
        y += rh + 6;
        ui_.check({x, y, w * 0.4f, rh}, "Loop", fLoop_);
        ui_.text("aind", {x + w * 0.4f, y, w * 0.6f, rh}, "Indices", fIndices_);
        y += rh + 10;
        if (ui_.button({x, y, w * 0.5f - 4, rh}, "Add / update")) addOrUpdate();
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh}, "Remove")) removeAnim();
        y += rh + 8;
        if (anim_ < n) {
            psych::AnimDef& ad = def_.anims[size_t(anim_)];
            double ox = ad.offX, oy = ad.offY;
            if (ui_.stepper({x, y, w, rh}, "Offset X", ox, 1, -5000, 5000, "%.0f")) {
                ad.offX = float(ox);
                offsetsChanged();
            }
            y += rh + 6;
            if (ui_.stepper({x, y, w, rh}, "Offset Y", oy, 1, -5000, 5000, "%.0f")) {
                ad.offY = float(oy);
                offsetsChanged();
            }
            y += rh + 6;
        }
        bool g = ghostOn_;
        if (ui_.check({x, y, w, rh}, "Ghost (first animation)", g)) {
            ghostOn_ = g;
            if (ghost_) ghost_->visible = g;
        }
    }

    void addOrUpdate() {
        const std::string name = trim(fName_);
        if (name.empty() || trim(fPrefix_).empty()) {
            status_.set("An animation needs a name and a prefix", true);
            return;
        }
        psych::AnimDef ad;
        ad.anim = name;
        ad.prefix = trim(fPrefix_);
        ad.fps = fFps_;
        ad.loop = fLoop_;
        std::string part;
        for (char c : fIndices_ + ",") {
            if (c == ',') {
                if (!trim(part).empty()) ad.indices.push_back(std::atoi(trim(part).c_str()));
                part.clear();
            } else {
                part += c;
            }
        }
        auto it = std::find_if(def_.anims.begin(), def_.anims.end(), [&](const psych::AnimDef& a) { return a.anim == name; });
        if (it != def_.anims.end()) {
            ad.offX = it->offX;
            ad.offY = it->offY;
            *it = ad;
            anim_ = int(it - def_.anims.begin());
        } else {
            def_.anims.push_back(ad);
            anim_ = int(def_.anims.size()) - 1;
        }
        changed();
        if (ch_ && !ch_->hasAnim(name)) status_.set("No frames called \"" + ad.prefix + "\" in the image", true);
    }

    void removeAnim() {
        if (anim_ >= int(def_.anims.size())) return;
        def_.anims.erase(def_.anims.begin() + anim_);
        anim_ = std::max(0, anim_ - 1);
        fillAnimFields();
        changed();
    }

    void newCharacter() {
        def_ = psych::CharacterDef{};
        def_.images = {"characters/BOYFRIEND"};
        def_.icon = "face";
        psych::AnimDef idle;
        idle.anim = "idle";
        idle.prefix = "BF idle dance";
        def_.anims.push_back(idle);
        loadedPath_.clear();
        loadedText_.clear();
        saveName_ = "new-character";
        anim_ = 0;
        syncFields();
        changed();
        resetView();
    }

    void save() {
        const std::string name = fileName(saveName_);
        if (name.empty()) {
            status_.set("Type a name to save as", true);
            return;
        }
        std::string anims = "[\n";
        for (size_t i = 0; i < def_.anims.size(); ++i) {
            const psych::AnimDef& ad = def_.anims[i];
            std::string ind = "[";
            for (size_t k = 0; k < ad.indices.size(); ++k) ind += (k ? ", " : "") + std::to_string(ad.indices[k]);
            ind += "]";
            anims += "\t\t{\n\t\t\t\"anim\": " + jsonStr(ad.anim) + ",\n\t\t\t\"name\": " + jsonStr(ad.prefix) + ",\n\t\t\t\"fps\": " +
                     jnum(ad.fps) + ",\n\t\t\t\"loop\": " + (ad.loop ? "true" : "false") + ",\n\t\t\t\"indices\": " + ind +
                     ",\n\t\t\t\"offsets\": " + pair(ad.offX, ad.offY) + "\n\t\t}" + (i + 1 < def_.anims.size() ? ",\n" : "\n");
        }
        anims += "\t]";
        const uint32_t c = def_.barColor;
        const std::vector<std::pair<std::string, std::string>> fields = {
            {"animations", anims},
            {"image", jsonStr(imageKeys(def_))},
            {"scale", jnum(def_.scale)},
            {"sing_duration", jnum(def_.singDuration)},
            {"healthicon", jsonStr(def_.icon)},
            {"position", pair(def_.posX, def_.posY)},
            {"camera_position", pair(def_.camX, def_.camY)},
            {"flip_x", def_.flipX ? "true" : "false"},
            {"no_antialiasing", def_.antialiasing ? "false" : "true"},
            {"healthbar_colors", "[" + std::to_string(c >> 16 & 255) + ", " + std::to_string(c >> 8 & 255) + ", " + std::to_string(c & 255) + "]"},
            {"_editor_isPlayer", player_ ? "true" : "false"},
        };
        // The same file when it is in this mod; a new one in the mod otherwise.
        const std::string path = mod_.roots[0] + "characters/" + name + ".json";
        std::string err;
        if (!saveText(path, writeObject(fields, loadedText_), err)) {
            status_.set(err, true);
            return;
        }
        scene::readFile(path, loadedText_);
        loadedPath_ = path;
        dirty_ = leaveArmed_ = false;
        if (std::find(names_.begin(), names_.end(), name) == names_.end()) names_.push_back(name);
        status_.set("Saved " + path);
    }
};

// =========================================================== stage editor ==

class StageEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        scene::Scene probe;
        useMod(probe, mod_);
        names_ = listNames(probe, "stages/", ".json");
        auto it = std::find(names_.begin(), names_.end(), "stage");
        load(it != names_.end() ? *it : names_.empty() ? std::string("stage") : names_[0]);
    }
    bool keyReserved(int) const override { return !ui_.focus.empty(); }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (sc_) {
            for (auto& kv : sc_->objects) kv.second->updateAnim(dt);
            for (const scene::CharacterPtr& c : chars_)
                if (c) c->updateAnim(dt);
        }
        view_.handle(in, ui_, overPanel(ui_));
        dragCharacters(in);
        if (!ui_.focus.empty()) return;
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        if (in.pressed(KEY_ESCAPE)) {
            if (dirty_ && !leaveArmed_) {
                status_.set("Unsaved changes: ESC again to leave without saving (CTRL+S saves)");
                leaveArmed_ = true;
            } else {
                a.change(makeEditorMenu());
            }
            return;
        }
        if (ctrl && in.pressed(KEY_S)) save();
        if (in.pressed(KEY_R) && !ctrl) resetView();
        if (in.pressed(KEY_C)) showCam_ = !showCam_;
    }

    void draw() override {
        gfx::beginCanvas();
        drawGrid(view_);
        if (sc_) {
            view_.apply(sc_->camGame);
            rend_.drawCamera(*sc_, scene::Cam::Game, nullptr, nullptr);
            static const Color cols[3] = {Color{120, 200, 255, 255}, Color{255, 140, 200, 255}, Color{200, 140, 255, 255}};
            for (int i = 0; i < 3; ++i) {
                if (i == 1 && def_.hideGf) continue;
                const float* p = pos(i);
                cross(view_.toCanvas(p[0], p[1]), 10, cols[i]);
                gfx::text(FontId::Ui, kWho[i], view_.toCanvas(p[0], p[1]).x + 12, view_.toCanvas(p[0], p[1]).y + 4, 16, cols[i]);
                if (const scene::CharacterPtr& c = chars_[i]; c && i == tab_ - 1) {
                    const Vector2 a = view_.toCanvas(c->x, c->y), b = view_.toCanvas(c->x + c->width, c->y + c->height);
                    DrawRectangleLinesEx({a.x, a.y, b.x - a.x, b.y - a.y}, 1.5f, cols[i]);
                }
            }
            if (showCam_) drawCameraFrame();
        }
        DrawRectangleRec({8, 8, 856, 32}, fade(BLACK, 0.55f));
        gfx::text(FontId::Ui, saveName_ + (dirty_ ? "  (not saved)" : "") + "   mod: " + modLabel(mod_) + "   zoom " + jnum(def_.zoom), 18, 13,
                  18, kInk);
        hints("LEFT-DRAG a character to move it   RIGHT-DRAG move the view   WHEEL zoom   C camera   R reset   CTRL+S save");
        drawPanel();
        status_.draw();
        gfx::endCanvas();
    }

private:
    static constexpr const char* kWho[3] = {"Boyfriend", "Girlfriend", "Opponent"};
    std::unique_ptr<scene::Scene> sc_;
    std::unique_ptr<ScriptHost> host_;
    EditorGame game_;
    SceneRenderer rend_;
    Ui ui_;
    View view_;
    Status status_;
    ModChoice mod_;
    std::vector<std::string> names_;
    int nameIdx_ = 0, tab_ = 0, camWho_ = 0;
    psych::StageDef def_;
    std::string loadedText_, saveName_;
    std::string who_[3] = {"bf", "gf", "dad"};
    scene::CharacterPtr chars_[3];
    bool dirty_ = false, leaveArmed_ = false, showCam_ = true;
    int drag_ = -1;
    Vector2 dragLast_{};

    float* pos(int i) { return i == 0 ? def_.bf : i == 1 ? def_.gf : def_.dad; }
    float* camOff(int i) { return i == 0 ? def_.camBf : i == 1 ? def_.camGf : def_.camDad; }

    void load(const std::string& name) {
        saveName_ = name;
        auto it = std::find(names_.begin(), names_.end(), name);
        nameIdx_ = it == names_.end() ? 0 : int(it - names_.begin());
        def_ = psych::StageDef{};
        loadedText_.clear();
        scene::Scene probe;
        useMod(probe, mod_);
        const std::string path = probe.files.find("stages/" + name + ".json");
        std::string err;
        if (!path.empty() && (!scene::readFile(path, loadedText_) || !psych::parseStage(loadedText_, def_, err)))
            status_.set("stages/" + name + ".json: " + err, true);
        dirty_ = leaveArmed_ = false;
        buildScene(name);
        resetView();
    }

    // The stage as a song shows it: its JSON objects, the three characters and
    // what its Lua script makes.
    void buildScene(const std::string& name) {
        host_.reset();
        rend_.clear();
        sc_ = std::make_unique<scene::Scene>();
        scene::Scene& sc = *sc_;
        useMod(sc, mod_);
        sc.stage = def_;
        sc.gfMarker = std::make_shared<scene::Sprite>();
        sc.gfMarker->kind = scene::Kind::Marker;
        sc.gfMarker->tag = "gfGroup";
        for (int i = 0; i < 3; ++i) makeChar(i);
        bool placed = false;
        for (const psych::StageObject& o : def_.objects) {
            const std::string t = toLower(o.type);
            if (t == "gf" || t == "gfgroup") {
                sc.order.push_back(sc.gfMarker);
                if (chars_[1]) sc.order.push_back(chars_[1]);
                placed = true;
                continue;
            }
            if (t == "dad" || t == "dadgroup") {
                if (chars_[2]) sc.order.push_back(chars_[2]);
                continue;
            }
            if (t == "boyfriend" || t == "boyfriendgroup") {
                if (chars_[0]) sc.order.push_back(chars_[0]);
                continue;
            }
            auto s = std::make_shared<scene::Sprite>();
            s->tag = o.name;
            if (t == "square") s->setGraphic(sc.gfx.solid(1, 1, 0xFF000000u | o.color));
            else if (t == "animatedsprite") s->setGraphic(sc.gfx.atlas(o.image));
            else s->setGraphic(sc.gfx.image(o.image));
            for (const psych::AnimDef& ad : o.anims) {
                if (!s->graphic) break;
                scene::Anim an;
                an.name = ad.anim;
                an.frames = ad.indices.empty() ? psych::framesByPrefix(s->graphic->frames, ad.prefix)
                                               : psych::framesByIndices(s->graphic->frames, ad.prefix, ad.indices);
                an.fps = ad.fps;
                an.loop = ad.loop;
                if (!an.frames.empty()) s->addAnim(std::move(an));
            }
            if (!o.firstAnim.empty()) s->play(o.firstAnim, true);
            s->x = o.x;
            s->y = o.y;
            s->scaleX = o.scaleX;
            s->scaleY = o.scaleY;
            if (t == "square") s->setGraphicSize(o.scaleX, o.scaleY), s->scaleX = o.scaleX, s->scaleY = o.scaleY;
            s->updateHitbox();
            s->scrollX = o.scrollX;
            s->scrollY = o.scrollY;
            s->alpha = o.alpha;
            s->angle = o.angle;
            s->color = o.color;
            s->flipX = o.flipX;
            s->flipY = o.flipY;
            s->antialiasing = o.antialiasing;
            s->inScene = true;
            if (!o.name.empty()) sc.objects[o.name] = s;
            sc.order.push_back(s);
        }
        if (!placed) {
            sc.order.push_back(sc.gfMarker);
            if (chars_[1]) sc.order.push_back(chars_[1]);
            if (chars_[2]) sc.order.push_back(chars_[2]);
            if (chars_[0]) sc.order.push_back(chars_[0]);
        }
        // The stage's script builds its sprites (Psych 0.6 / 0.7 stages are Lua).
        const std::string lua = sc.files.find("stages/" + name + ".lua");
        if (!lua.empty()) {
            host_ = std::make_unique<ScriptHost>(sc, game_);
            host_->load(lua);
            host_->call("onCreatePost");
        }
        rend_.preload(sc);
    }

    void makeChar(int i) {
        chars_[i].reset();
        if (i == 1 && def_.hideGf) return;
        scene::Scene& sc = *sc_;
        psych::CharacterDef cd;
        std::string path, text, err;
        if (loadCharacterDef(sc, who_[i], cd, path, text, err)) chars_[i] = buildCharacter(sc, cd, i == 0, err);
        if (!chars_[i]) return;
        chars_[i]->scrollX = chars_[i]->scrollY = i == 1 ? 0.95f : 1.0f;
        place(i);
    }

    void place(int i) {
        if (!chars_[i]) return;
        const float* p = pos(i);
        chars_[i]->x = p[0] + chars_[i]->posX;
        chars_[i]->y = p[1] + chars_[i]->posY;
    }

    // Where the camera looks at character i, as Psych's moveCamera works it out.
    Vector2 cameraFocus(int i) {
        const scene::CharacterPtr& c = chars_[i];
        const float* off = camOff(i);
        if (!c) return {pos(i)[0] + off[0], pos(i)[1] + off[1]};
        const float mx = c->x + c->width * 0.5f, my = c->y + c->height * 0.5f;
        if (i == 0) return {mx - 100 - c->camX + off[0], my - 100 + c->camY + off[1]};
        if (i == 2) return {mx + 150 + c->camX + off[0], my - 100 + c->camY + off[1]};
        return {mx + c->camX + off[0], my + c->camY + off[1]};
    }

    void drawCameraFrame() {
        const Vector2 f = cameraFocus(camWho_);
        const float hw = gfx::W * 0.5f / float(std::max(0.05, def_.zoom)), hh = gfx::H * 0.5f / float(std::max(0.05, def_.zoom));
        const Vector2 a = view_.toCanvas(f.x - hw, f.y - hh), b = view_.toCanvas(f.x + hw, f.y + hh);
        DrawRectangleLinesEx({a.x, a.y, b.x - a.x, b.y - a.y}, 2, kGold);
        cross(view_.toCanvas(f.x, f.y), 14, kGold);
        gfx::text(FontId::Ui, std::string("Camera on ") + kWho[camWho_], a.x + 6, a.y + 4, 16, kGold);
    }

    void dragCharacters(const Input& in) {
        if (!sc_) return;
        const Vector2 w = view_.toWorld(ui_.mouse);
        if (in.mousePressed(MOUSE_BUTTON_LEFT) && !overPanel(ui_) && ui_.focus.empty()) {
            for (int i : {0, 2, 1}) {  // the front one first
                const scene::CharacterPtr& c = chars_[i];
                if (c && w.x >= c->x && w.x <= c->x + c->width && w.y >= c->y && w.y <= c->y + c->height) {
                    drag_ = i;
                    dragLast_ = w;
                    tab_ = i + 1;
                    camWho_ = i;
                    break;
                }
            }
        }
        if (drag_ >= 0) {
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                drag_ = -1;
                return;
            }
            float* p = pos(drag_);
            const float dx = std::round(w.x - dragLast_.x), dy = std::round(w.y - dragLast_.y);
            if (dx != 0 || dy != 0) {
                p[0] += dx;
                p[1] += dy;
                dragLast_.x += dx;
                dragLast_.y += dy;
                place(drag_);
                dirty_ = true;
                leaveArmed_ = false;
            }
        }
    }

    void resetView() {
        const Vector2 a = cameraFocus(2), b = cameraFocus(0);
        view_.x = (a.x + b.x) * 0.5f;
        view_.y = (a.y + b.y) * 0.5f;
        view_.zoom = float(std::max(0.05, def_.zoom)) * 0.55f;
        view_.x -= (kPanelX - gfx::W) * 0.5f / view_.zoom;
    }

    void drawPanel() {
        panelBox(8, gfx::H - 40);
        float y = 16;
        const float x = kPanelX, w = kPanelW, rh = 30;
        static const char* tabs[4] = {"Stage", "BF", "GF", "Dad"};
        for (int i = 0; i < 4; ++i)
            if (ui_.button({x + i * (w / 4), y, w / 4 - 4, rh}, tabs[i], tab_ == i)) tab_ = i;
        y += rh + 12;
        if (tab_ == 0) drawStageTab(x, y, w, rh);
        else drawCharTab(tab_ - 1, x, y, w, rh);
        ui_.overlays();
    }

    void drawStageTab(float x, float y, float w, float rh) {
        if (!names_.empty()) {
            if (ui_.dropdown("stage", {x, y, w, rh}, "Stage", names_, nameIdx_)) load(names_[size_t(nameIdx_)]);
            y += rh + 6;
        }
        ui_.text("save", {x, y, w, rh}, "Save as", saveName_);
        y += rh + 6;
        if (ui_.stepper({x, y, w, rh}, "Zoom", def_.zoom, 0.05, 0.05, 10, "%.2f")) dirty_ = true;
        y += rh + 6;
        if (ui_.stepper({x, y, w, rh}, "Camera speed", def_.camSpeed, 0.1, 0, 20, "%.1f")) dirty_ = true;
        y += rh + 6;
        if (ui_.check({x, y, w, rh}, "Pixel stage", def_.pixel)) dirty_ = true;
        y += rh + 6;
        if (ui_.check({x, y, w, rh}, "Hide girlfriend", def_.hideGf)) {
            dirty_ = true;
            buildScene(saveName_);
        }
        y += rh + 6;
        ui_.check({x, y, w * 0.5f, rh}, "Camera frame", showCam_);
        std::vector<std::string> who = {"BF", "GF", "Dad"};
        ui_.choice({x + w * 0.5f, y, w * 0.5f, rh}, "", who, camWho_, 0);
        y += rh + 12;
        gfx::text(FontId::Ui, def_.objects.empty() ? "Sprites: from the stage's Lua script" : TextFormat("%d objects in the JSON", int(def_.objects.size())),
                  x, y, 16, kDim);
        y += 26;
        if (ui_.button({x, y, w * 0.5f - 4, rh + 4}, "Save")) save();
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh + 4}, "Reload")) load(saveName_);
    }

    void drawCharTab(int i, float x, float y, float w, float rh) {
        gfx::text(FontId::Ui, kWho[i], x, y, 22, kGold);
        y += 34;
        ui_.text("who", {x, y, w - 92, rh}, "Character", who_[i]);
        if (ui_.button({x + w - 86, y, 86, rh}, "Show")) {
            who_[i] = trim(who_[i]);
            buildScene(saveName_);
        }
        y += rh + 6;
        gfx::text(FontId::Ui, "Only shown here: songs choose their characters.", x, y, 15, kDim);
        y += 26;
        float* p = pos(i);
        float* c = camOff(i);
        auto step = [&](const char* label, float& f, bool moves) {
            double d = f;
            if (ui_.stepper({x, y, w, rh}, label, d, 1, -20000, 20000, "%.0f")) {
                f = float(d);
                dirty_ = true;
                if (moves) place(i);
            }
            y += rh + 6;
        };
        step("Position X", p[0], true);
        step("Position Y", p[1], true);
        step("Camera X", c[0], false);
        step("Camera Y", c[1], false);
        camWho_ = i;
    }

    void save() {
        const std::string name = fileName(saveName_);
        if (name.empty()) {
            status_.set("Type a name to save as", true);
            return;
        }
        const std::vector<std::pair<std::string, std::string>> fields = {
            {"directory", jsonStr(def_.directory)},
            {"defaultZoom", jnum(def_.zoom)},
            {"isPixelStage", def_.pixel ? "true" : "false"},
            {"boyfriend", pair(def_.bf[0], def_.bf[1])},
            {"girlfriend", pair(def_.gf[0], def_.gf[1])},
            {"opponent", pair(def_.dad[0], def_.dad[1])},
            {"hide_girlfriend", def_.hideGf ? "true" : "false"},
            {"camera_boyfriend", pair(def_.camBf[0], def_.camBf[1])},
            {"camera_opponent", pair(def_.camDad[0], def_.camDad[1])},
            {"camera_girlfriend", pair(def_.camGf[0], def_.camGf[1])},
            {"camera_speed", jnum(def_.camSpeed)},
        };
        const std::string path = mod_.roots[0] + "stages/" + name + ".json";
        std::string err;
        if (!saveText(path, writeObject(fields, loadedText_), err)) {
            status_.set(err, true);
            return;
        }
        scene::readFile(path, loadedText_);
        dirty_ = leaveArmed_ = false;
        if (std::find(names_.begin(), names_.end(), name) == names_.end()) names_.push_back(name);
        status_.set("Saved " + path);
    }
};

// =========================================================== event editor ==

struct EventTemplate {
    const char* name;
    const char* v1;
    const char* v2;
    const char* body;
};

const EventTemplate kTemplates[] = {
    {"Empty", "", "", "\t-- what the event does\n"},
    {"Camera flash", "Duration (s)", "Colour (FFFFFF)",
     "\tlocal colour = value2 ~= '' and value2 or 'FFFFFF'\n\tcameraFlash('game', colour, tonumber(value1) or 0.5, true)\n"},
    {"Fade the HUD", "Alpha (0 to 1)", "Duration (s)",
     "\tdoTweenAlpha('eventHudFade', 'camHUD', tonumber(value1) or 0, tonumber(value2) or 0.5, 'linear')\n"},
    {"Camera shake", "Intensity (0.01)", "Duration (s)",
     "\tcameraShake('game', tonumber(value1) or 0.01, tonumber(value2) or 0.3)\n\tcameraShake('hud', (tonumber(value1) or 0.01) / 2, tonumber(value2) or 0.3)\n"},
    {"Zoom the camera", "Zoom (0.9)", "Duration (s)",
     "\tdoTweenZoom('eventZoom', 'camGame', tonumber(value1) or 0.9, tonumber(value2) or 0.5, 'quadOut')\n\tsetProperty('defaultCamZoom', tonumber(value1) or 0.9)\n"},
    {"Play a sound", "Sound (sounds/...)", "Volume (0 to 1)", "\tplaySound(value1, tonumber(value2) or 1)\n"},
    {"Scroll speed", "Multiplier", "Duration (s)", "\ttriggerEvent('Change Scroll Speed', value1, value2)\n"},
};

class EventEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        useMod(sc_, mod_);
        refresh();
    }
    bool keyReserved(int) const override { return !ui_.focus.empty(); }

    void update(double) override {
        App& a = app();
        feedUi(ui_);
        if (!ui_.focus.empty()) return;
        if (a.input.pressed(KEY_ESCAPE)) a.change(makeEditorMenu());
    }

    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x6C8CFF, 0);
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.45f));
        gfx::text(FontId::Title, "EVENT EDITOR", 40, 18, 40, kGold, Align::Left, 2.0f);
        gfx::text(FontId::Ui, "Custom events of " + modLabel(mod_) + ": custom_events/<name>.txt (what the chart editor shows) and .lua",
                  40, 70, 18, kIvory);
        drawList();
        drawForm();
        ui_.overlays();
        hints("Click an event to see it   ESC back");
        status_.draw();
        gfx::endCanvas();
    }

private:
    scene::Scene sc_;
    Ui ui_;
    Status status_;
    ModChoice mod_;
    std::vector<std::string> names_;
    int sel_ = -1, template_ = 0;
    float scroll_ = 0;
    std::string name_, desc_, v1_, v2_, script_;

    void refresh() {
        std::set<std::string> all;
        for (const char* ext : {".txt", ".lua"})
            for (const std::string& n : listNames(sc_, "custom_events/", ext)) all.insert(n);
        names_.assign(all.begin(), all.end());
    }

    std::string mine(const std::string& ext) const { return mod_.roots[0] + "custom_events/" + fileName(name_) + ext; }

    void open(int i) {
        sel_ = i;
        name_ = names_[size_t(i)];
        desc_.clear();
        script_.clear();
        std::string text;
        if (const std::string p = sc_.files.find("custom_events/" + name_ + ".txt"); !p.empty() && scene::readFile(p, text)) desc_ = trim(text);
        if (const std::string p = sc_.files.find("custom_events/" + name_ + ".lua"); !p.empty()) scene::readFile(p, script_);
    }

    void drawList() {
        const Rectangle box{40, 104, 400, gfx::H - 160};
        DrawRectangleRounded(box, 0.02f, 6, kPanel);
        const float row = 26;
        const int rows = int((box.height - 8) / row), n = int(names_.size());
        if (CheckCollisionPointRec(ui_.mouse, box) && ui_.wheel != 0) scroll_ = clampv(scroll_ - ui_.wheel * 3, 0.0f, float(std::max(0, n - rows)));
        if (n == 0) gfx::text(FontId::Ui, "No custom events yet", box.x + 12, box.y + 10, 18, kDim);
        const int first = int(scroll_);
        for (int k = 0; k < rows && first + k < n; ++k) {
            const int i = first + k;
            const Rectangle r{box.x + 4, box.y + 4 + k * row, box.width - 8, row - 2};
            const bool hov = ui_.hover(r);
            if (i == sel_ || hov) DrawRectangleRec(r, i == sel_ ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
            gfx::text(FontId::Ui, names_[size_t(i)], r.x + 8, r.y + 3, 18, kInk);
            if (hov && ui_.click) {
                ui_.used = true;
                open(i);
            }
        }
    }

    void drawForm() {
        const float x = 470, w = gfx::W - x - 40, rh = 30;
        float y = 104;
        DrawRectangleRounded({x - 10, y - 8, w + 20, gfx::H - 152}, 0.02f, 6, kPanel);
        ui_.text("name", {x, y, w, rh}, "Name", name_);
        y += rh + 8;
        ui_.text("desc", {x, y, w, rh}, "Description", desc_);
        y += rh + 8;
        ui_.text("v1", {x, y, w, rh}, "Value 1", v1_);
        y += rh + 8;
        ui_.text("v2", {x, y, w, rh}, "Value 2", v2_);
        y += rh + 8;
        std::vector<std::string> tnames;
        for (const EventTemplate& t : kTemplates) tnames.push_back(t.name);
        if (ui_.dropdown("tmpl", {x, y, w, rh}, "Does", tnames, template_)) {
            const EventTemplate& t = kTemplates[template_];
            if (v1_.empty() || isTemplateHint(v1_)) v1_ = t.v1;
            if (v2_.empty() || isTemplateHint(v2_)) v2_ = t.v2;
        }
        y += rh + 12;
        if (ui_.button({x, y, 220, rh + 4}, "Create / save")) create();
        if (ui_.button({x + 230, y, 160, rh + 4}, "Open script")) openFile(".lua");
        if (ui_.button({x + 400, y, 160, rh + 4}, "Open folder")) {
            std::error_code ec;
            fs::create_directories(toPath(mod_.roots[0] + "custom_events"), ec);
            if (!openWithSystem(mod_.roots[0] + "custom_events")) status_.set("This system cannot open folders from the game", true);
        }
        y += rh + 18;
        gfx::text(FontId::Ui, "Script:", x, y, 18, kDim);
        y += 24;
        // The script as it is (the first lines).
        int line = 0;
        size_t p = 0;
        const std::string shown = script_.empty() ? scriptFor() : script_;
        while (p < shown.size() && line < 14) {
            size_t e = shown.find('\n', p);
            if (e == std::string::npos) e = shown.size();
            std::string l = shown.substr(p, e - p);
            for (size_t t; (t = l.find('\t')) != std::string::npos;) l.replace(t, 1, "    ");
            gfx::text(FontId::Ui, l, x, y, 16, script_.empty() ? kDim : kInk);
            y += 20;
            ++line;
            p = e + 1;
        }
    }

    static bool isTemplateHint(const std::string& s) {
        for (const EventTemplate& t : kTemplates)
            if (s == t.v1 || s == t.v2) return true;
        return false;
    }

    std::string scriptFor() const {
        const std::string n = trim(name_).empty() ? std::string("My Event") : trim(name_);
        std::string s = "-- " + n + (desc_.empty() ? "" : ": " + desc_) + "\n";
        if (!v1_.empty()) s += "-- Value 1: " + v1_ + "\n";
        if (!v2_.empty()) s += "-- Value 2: " + v2_ + "\n";
        std::string quoted;
        for (char c : n) quoted += c == '\'' ? std::string("\\'") : std::string(1, c);
        s += "function onEvent(name, value1, value2)\n\tif name ~= '" + quoted + "' then return end\n";
        s += kTemplates[template_].body;
        s += "end\n";
        return s;
    }

    void create() {
        const std::string n = fileName(name_);
        if (n.empty()) {
            status_.set("Type the event's name", true);
            return;
        }
        // The description file holds what the chart editor shows for it.
        std::string desc = desc_;
        if (!v1_.empty()) desc += "\nValue 1: " + v1_;
        if (!v2_.empty()) desc += "\nValue 2: " + v2_;
        std::string err;
        if (!saveText(mine(".txt"), desc + "\n", err)) {
            status_.set(err, true);
            return;
        }
        std::error_code ec;
        const bool hasScript = fs::exists(toPath(mine(".lua")), ec);
        if (!hasScript && !saveText(mine(".lua"), scriptFor(), err)) {
            status_.set(err, true);
            return;
        }
        refresh();
        auto it = std::find(names_.begin(), names_.end(), n);
        if (it != names_.end()) open(int(it - names_.begin()));
        status_.set(hasScript ? "Saved the description (the script was there: it is kept)" : "Made " + mine(".txt") + " and its .lua");
    }

    void openFile(const std::string& ext) {
        std::string p = sc_.files.find("custom_events/" + trim(name_) + ext);
        if (p.empty()) {
            status_.set("Create the event first", true);
            return;
        }
        if (!openWithSystem(p)) status_.set("This system cannot open files from the game: " + p, true);
    }
};

// ======================================================== visuals editor ==
// visuals.json: timed shader/FX clips for one song (the .RAID stays notes-
// only). Left: the clip list. Centre: a live preview - the song's audio
// through the mixer, a demo playfield through the active shader chain.
// Right: the selected clip's timing, target camera, effect and uniforms.

class VisualsEditorState : public State {
public:
    // Opened from a song (key 8 in gameplay): start on that song/diff.
    void openFor(const std::string& songId, const std::string& diff) {
        wantSongId_ = songId;
        wantDiff_ = diff;
    }

    void enter() override {
        mod_ = currentMod();
        useMod(sc_, mod_);
        buildSongList();
        int pick = -1;
        if (!wantSongId_.empty()) {
            const auto& songs = app().library.songs();
            for (int idx : songIdx_)
                if (songs[size_t(idx)].id == wantSongId_) { pick = idx; break; }
        }
        if (pick < 0) pick = freeplayPick_ >= 0 ? freeplayPick_ : (songIdx_.empty() ? -1 : songIdx_[0]);
        pickSong(pick, wantDiff_);
    }
    bool keyReserved(int key) const override {
        return !ui_.focus.empty() || key == KEY_SPACE;
    }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        if (audioReady_) t.keys = {{"PLAY", KEY_SPACE}, {"SAVE", KEY_F2}, {"BACK", KEY_ESCAPE}};
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (job_.valid()) {
            if (job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                Prepared p = job_.get();
                if (p.ok) {
                    prep_ = std::move(p);
                    audioReady_ = true;
                    a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
                    a.mixer.setRate(1.0);
                    dirty_ = false;
                } else {
                    status_.set(p.error.empty() ? "cannot load the song" : p.error, true);
                }
            }
            return;
        }
        if (!audioReady_) {
            if (in.back()) exitEditor();
            return;
        }
        if (playing_) {
            curMs_ = a.mixer.time(a.now) * 1000.0;
            if (a.mixer.ended() || curMs_ > songLenMs() + 1500) {
                a.mixer.pause();
                playing_ = false;
                curMs_ = songLenMs();
            }
        }
        sc_.update(dt, chartStepMs(), false);  // keeps shader clocks alive
        if (in.pressed(KEY_SPACE)) togglePlay();
        if (in.pressed(KEY_F2)) save();
        if (in.pressed(KEY_ESCAPE)) {
            if (dirty_ && !leaveArmed_) {
                status_.set("Unsaved changes: ESC again to leave without saving (F2 saves)", true);
                leaveArmed_ = true;
            } else {
                exitEditor();
            }
        }
        syncFields();
    }

    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x6C8CFF, 0);
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.45f));
        gfx::text(FontId::Title, "VISUALS EDITOR", 40, 18, 40, kGold, Align::Left, 2.0f);
        drawHeader();
        if (!audioReady_) {
            gfx::text(FontId::Ui, "Loading " + (song_ ? song_->title : std::string("song")) + "...", gfx::W / 2, gfx::H / 2, 26, kIvory, Align::Center);
            hints("ESC back");
            ui_.overlays();
            status_.draw();
            gfx::endCanvas();
            return;
        }
        drawClipList();
        drawPreview();
        drawTimeline();
        drawFields();
        ui_.overlays();
        hints("SPACE play/pause   Click timeline: seek / select clip   F2 save   ESC back");
        status_.draw();
        gfx::endCanvas();
    }

private:
    scene::Scene sc_;
    SceneRenderer renderer_;
    Ui ui_;
    Status status_;
    ModChoice mod_;
    std::vector<int> songIdx_;       // indices into library.songs()
    int songPos_ = 0;
    const SongEntry* song_ = nullptr;
    std::string diff_;
    std::future<Prepared> job_;
    Prepared prep_;
    bool audioReady_ = false, playing_ = false, dirty_ = false, leaveArmed_ = false;
    double curMs_ = 0;
    std::vector<VisualClip> clips_;
    std::string visualsPath_;
    int sel_ = -1;
    // Selected clip's editable fields (strings; synced both ways).
    std::string fStart_, fEnd_;
    int fTarget_ = 0, fFx_ = 0;
    std::vector<std::pair<std::string, std::string>> fParams_;
    std::vector<std::string> fxNames_;
    RenderTexture2D prevRt_{};
    float scroll_ = 0;
    int syncSel_ = -2;
    size_t syncN_ = ~size_t(0);

    int freeplayPick_ = -1;
    std::string wantSongId_, wantDiff_;

    void buildSongList() {
        const auto& songs = app().library.songs();
        for (int i = 0; i < int(songs.size()); ++i) {
            const SongEntry& e = songs[size_t(i)];
            if (e.builtin || e.diffs.empty()) continue;
            if (wantSongId_.empty() && !mod_.name.empty() && e.pack != mod_.name) continue;
            songIdx_.push_back(i);
        }
        if (songIdx_.empty()) {
            for (int i = 0; i < int(songs.size()); ++i)
                if (!songs[size_t(i)].builtin && !songs[size_t(i)].diffs.empty()) songIdx_.push_back(i);
        }
        // The song chosen in Freeplay wins when it is in this mod.
        const int fp = app().freeplaySel;
        if (fp >= 0 && fp < int(songs.size()) && !songs[size_t(fp)].builtin &&
            (mod_.name.empty() || songs[size_t(fp)].pack == mod_.name))
            freeplayPick_ = fp;
    }

    void pickSong(int libIdx, const std::string& diff = "") {
        const auto& songs = app().library.songs();
        if (libIdx < 0 || libIdx >= int(songs.size())) return;
        auto it = std::find(songIdx_.begin(), songIdx_.end(), libIdx);
        songPos_ = it != songIdx_.end() ? int(it - songIdx_.begin()) : 0;
        song_ = &songs[size_t(libIdx)];
        diff_ = diff;
        if (diff_.empty() || std::find(song_->diffs.begin(), song_->diffs.end(), diff_) == song_->diffs.end())
            diff_ = song_->diffs.empty() ? "" : song_->diffs.front();
        fxNames_ = fx::names();
        for (const std::string& n : customShaders()) fxNames_.push_back(n);
        loadClips();
        const SongEntry copy = *song_;
        const std::string d = diff_;
        job_ = std::async(std::launch::async, [copy, d] { return app().library.prepare(copy, d, false); });
        audioReady_ = false;
        playing_ = false;
        sel_ = -1;
        fParams_.clear();
    }

    void cycleSong(int d) {
        if (songIdx_.empty() || job_.valid()) return;
        songPos_ = ((songPos_ + d) % int(songIdx_.size()) + int(songIdx_.size())) % int(songIdx_.size());
        pickSong(songIdx_[size_t(songPos_)]);
    }

    std::string visualsFile() const { return song_ && !song_->dataDir.empty() ? song_->dataDir + "/visuals.json" : ""; }

    void loadClips() {
        clips_.clear();
        visualsPath_ = visualsFile();
        std::string text;
        if (!visualsPath_.empty() && scene::readFile(visualsPath_, text)) {
            std::string err;
            if (!parseVisuals(text, clips_, err)) status_.set("visuals.json: " + err, true);
        }
        dirty_ = false;
        leaveArmed_ = false;
    }

    double songLenMs() const {
        double len = prep_.chart.lengthMs;
        if (prep_.inst) len = std::max(len, double(prep_.inst->frames()) * 1000.0 / Mixer::kRate);
        return std::max(1000.0, len);
    }

    double chartStepMs() const { return prep_.chart.tempo.empty() ? 500.0 : prep_.chart.stepMs(std::max(0.0, curMs_)); }

    void togglePlay() {
        App& a = app();
        if (playing_) {
            a.mixer.pause();
            playing_ = false;
            return;
        }
        a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
        a.mixer.setRate(1.0);
        a.mixer.play(std::max(0.0, curMs_) / 1000.0);
        playing_ = true;
    }

    void seek(double ms) {
        curMs_ = clampv(ms, 0.0, songLenMs());
        app().mixer.seek(curMs_ / 1000.0);  // works paused and playing
    }


    // .frag files shipped with the song or visible in its asset roots.
    std::vector<std::string> customShaders() {
        std::vector<std::string> out;
        std::set<std::string> seen;
        auto scan = [&](const std::string& dir) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(toPath(dir), ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string n = pathStr(e.path().filename());
                const std::string ext = pathStr(e.path().extension());
                if (!equalsI(ext, ".frag") && !equalsI(ext, ".glsl")) continue;
                if (n.size() > ext.size()) n.resize(n.size() - ext.size());
                if (seen.insert(n).second) out.push_back(n);
            }
        };
        if (song_) {
            scan(song_->dataDir + "/shaders");
            scan(song_->dataDir);
            for (const std::string& r : song_->assetRoots) scan(r + "shaders");
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // Resolves a clip's fx name to a shader source in the editor's scene.
    bool ensureShaderSource(const std::string& fxName, std::string& nameOut) {
        std::string name = fxName;
        if (fx::source(name).empty() && name.rfind("__fx_", 0) != 0 && !fx::source("__fx_" + name).empty())
            name = "__fx_" + name;
        if (!fx::source(name).empty()) {
            fx::install(sc_, name);
            nameOut = name;
            return true;
        }
        if (sc_.shaderSources.count(name)) { nameOut = name; return true; }
        // Custom: shaders/<name>.frag in the song folder or asset roots.
        std::vector<std::string> tries;
        if (song_) {
            tries.push_back(song_->dataDir + "/shaders/" + name + ".frag");
            tries.push_back(song_->dataDir + "/" + name);
            for (const std::string& r : song_->assetRoots) tries.push_back(r + "shaders/" + name + ".frag");
        }
        for (const ModChoice& m : modChoices())
            for (const std::string& r : m.roots) tries.push_back(r + "shaders/" + name + ".frag");
        for (const std::string& p : tries) {
            scene::ShaderSource src;
            if (!scene::readFile(p, src.frag) || src.frag.empty()) continue;
            const std::string vp = p.substr(0, p.size() - 5) + ".vert";
            scene::readFile(vp, src.vert);
            sc_.shaderSources[name] = std::move(src);
            nameOut = name;
            return true;
        }
        return false;
    }

    // Shader chain active at curMs_, as ShaderInstances for drawFiltered.
    std::vector<scene::ShaderPtr> activeShaders() {
        std::vector<scene::ShaderPtr> out;
        for (const VisualClip& c : clips_) {
            const bool open = c.endMs <= c.startMs || curMs_ < c.endMs;
            if (!(curMs_ >= c.startMs && open)) continue;
            std::string name;
            if (!ensureShaderSource(c.fx, name)) continue;
            auto inst = std::make_shared<scene::ShaderInstance>();
            inst->name = name;
            inst->timeName = "uTime";
            inst->floats["uTime"] = {float(app().now)};
            inst->timeRate = 0;  // uTime is set per frame above
            for (const auto& [k, v] : c.params) inst->floats[k] = {float(v)};
            out.push_back(inst);
        }
        return out;
    }

    void drawDemoPlayfield() {
        // A stand-in for the playfield: dark stage, lane strips, receptors and
        // falling blocks - enough for shaders to have something to chew on.
        DrawRectangleRec({0, 0, float(prevRt_.texture.width), float(prevRt_.texture.height)}, Color{16, 12, 26, 255});
        const float w = float(prevRt_.texture.width), h = float(prevRt_.texture.height);
        const int lanes = prep_.chart.keys > 0 ? prep_.chart.keys : 4;
        const float lw = w / float(lanes);
        const double t = app().now;
        for (int side = 0; side < 2; ++side) {
            const float base = side == 0 ? 0.0f : w * 0.5f;
            const float lw2 = w * 0.5f / float(lanes);
            for (int l = 0; l < lanes; ++l) {
                const float x = base + l * lw2;
                DrawRectangleRec({x + 2, h * 0.08f, lw2 - 4, h * 0.84f}, Color{30, 24, 48, 255});
                // receptors
                const float ry = side == 0 ? h * 0.9f : h * 0.1f;
                DrawRectangleRec({x + lw2 * 0.22f, ry - 8, lw2 * 0.56f, 16},
                                 side == 0 ? Color{120, 200, 255, 220} : Color{255, 120, 160, 220});
                // a few falling notes per lane, looping
                for (int n = 0; n < 3; ++n) {
                    const double ph = std::fmod(t * 0.35 + double(l * 7 + n * 13 + side * 31), 3.0);
                    const float ny = float(ph / 3.0) * h * 0.8f + h * 0.05f;
                    DrawRectangleRec({x + lw2 * 0.3f, ny, lw2 * 0.4f, 10},
                                     side == 0 ? Color{90, 170, 255, 255} : Color{255, 110, 140, 255});
                }
            }
        }
    }

    void drawHeader() {
        const float y = 66;
        if (ui_.button({gfx::W - 330, y - 6, 60, 30}, "<")) cycleSong(-1);
        if (ui_.button({gfx::W - 264, y - 6, 60, 30}, ">")) cycleSong(1);
        const std::string label = song_ ? song_->title + "  (" + diff_ + ")" : "no song";
        gfx::text(FontId::Ui, label, gfx::W - 196, y, 20, kIvory, Align::Right);
        gfx::text(FontId::Ui, "visuals.json - shader clips for this song", 40, y, 18, kDim);
        if (ui_.button({gfx::W - 560, y - 6, 90, 30}, "Add clip")) addClip();
        if (ui_.button({gfx::W - 460, y - 6, 80, 30}, "Delete")) delClip();
        if (ui_.button({gfx::W - 120, y - 6, 100, 30}, "Save (F2)")) save();
    }

    Color targetColor(const std::string& t) const {
        if (t == "hud") return Color{90, 160, 255, 255};
        if (t == "both") return Color{120, 220, 140, 255};
        return Color{200, 130, 255, 255};
    }

    void drawClipList() {
        const Rectangle box{40, 110, 380, gfx::H - 190};
        DrawRectangleRounded(box, 0.02f, 6, kPanel);
        gfx::text(FontId::Ui, "CLIPS", box.x + 12, box.y + 6, 16, kDim);
        const float row = 30;
        const int rows = int((box.height - 40) / row), n = int(clips_.size());
        if (CheckCollisionPointRec(ui_.mouse, box) && ui_.wheel != 0)
            scroll_ = clampv(scroll_ - ui_.wheel * 3, 0.0f, float(std::max(0, n - rows)));
        if (n == 0) gfx::text(FontId::Ui, "No clips yet - Add clip", box.x + 12, box.y + 30, 18, kDim);
        const int first = int(scroll_);
        for (int k = 0; k < rows && first + k < n; ++k) {
            const int i = first + k;
            const VisualClip& c = clips_[size_t(i)];
            const Rectangle r{box.x + 4, box.y + 30 + k * row, box.width - 8, row - 3};
            const bool hov = ui_.hover(r);
            if (i == sel_ || hov) DrawRectangleRec(r, i == sel_ ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
            DrawRectangleRec({r.x + 4, r.y + 5, 8, r.height - 10}, targetColor(c.target));
            const std::string fxName = c.fx.rfind("__fx_", 0) == 0 ? c.fx.substr(5) : c.fx;
            gfx::text(FontId::Ui, TextFormat("%d. %.1f-%.1fs  %s", i + 1, c.startMs / 1000.0,
                                            c.endMs > c.startMs ? c.endMs / 1000.0 : -1.0, fxName.c_str()),
                      r.x + 20, r.y + 4, 17, kInk);
            if (hov && ui_.click) {
                ui_.used = true;
                selectClip(i);
            }
        }
    }

    void drawPreview() {
        const Rectangle vp{430, 110, 560, 315};
        DrawRectangleRounded(vp, 0.02f, 6, kPanel);
        gfx::text(FontId::Ui, "PREVIEW", vp.x + 12, vp.y + 6, 16, kDim);
        if (!prevRt_.id || prevRt_.texture.width != int(vp.width) || prevRt_.texture.height != int(vp.height)) {
            if (prevRt_.id) UnloadRenderTexture(prevRt_);
            prevRt_ = LoadRenderTexture(int(vp.width), int(vp.height));
        }
        // Texture mode can't nest inside the canvas camera (play_psych does
        // texture-outermost too): leave Mode2D, render the demo playfield and
        // the shader chain in screen space, then resume the canvas.
        gfx::endCanvas();
        gfx::beginTexture(prevRt_);
        ClearBackground(BLANK);
        drawDemoPlayfield();
        gfx::endTexture();
        std::vector<scene::ShaderPtr> chain = activeShaders();
        const float cs = gfx::canvasScale();
        const Vector2 co = gfx::canvasOrigin();
        renderer_.drawFiltered(sc_, chain, prevRt_,
                               Rectangle{co.x + vp.x * cs, co.y + vp.y * cs, vp.width * cs, vp.height * cs});
        gfx::beginCanvas();
        DrawRectangle(vp.x, vp.y, vp.width, 2, fade(kGold, 0.5f));
        // transport row under the viewport
        const float ty = vp.y + vp.height + 10;
        if (ui_.button({vp.x, ty, 90, 30}, playing_ ? "Pause" : "Play")) togglePlay();
        if (ui_.button({vp.x + 100, ty, 70, 30}, "Stop")) { seek(0); if (playing_) { app().mixer.pause(); playing_ = false; } }
        if (ui_.button({vp.x + 180, ty, 150, 30}, "Preview song")) previewSong();
        gfx::text(FontId::Ui, TextFormat("%.2f / %.2f s", curMs_ / 1000.0, songLenMs() / 1000.0),
                  vp.x + 340, ty + 5, 20, kIvory);
    }

    void drawTimeline() {
        const Rectangle tl{430, 470, 560, 34};
        DrawRectangleRounded(tl, 0.02f, 6, Color{24, 20, 34, 255});
        const double len = songLenMs();
        auto xOf = [&](double ms) { return tl.x + float(ms / len) * tl.width; };
        // ticks every ~10 s
        for (double s = 0; s <= len / 1000.0; s += 10) {
            const float x = xOf(s * 1000.0);
            DrawLineEx({x, tl.y + tl.height - 8}, {x, tl.y + tl.height}, 1, Color{70, 60, 100, 255});
            gfx::text(FontId::Ui, TextFormat("%d", int(s)), x + 2, tl.y + 4, 13, kDim);
        }
        for (size_t i = 0; i < clips_.size(); ++i) {
            const VisualClip& c = clips_[i];
            const float x0 = xOf(c.startMs), x1 = c.endMs > c.startMs ? xOf(c.endMs) : tl.x + tl.width;
            const bool sel = int(i) == sel_;
            DrawRectangleRec({x0 + 1, tl.y + 8, std::max(3.0f, x1 - x0 - 2), tl.height - 18},
                             fade(targetColor(c.target), sel ? 1.0f : 0.75f));
        }
        DrawLineEx({xOf(curMs_), tl.y}, {xOf(curMs_), tl.y + tl.height}, 2, kGold);
        if (ui_.hover(tl) && ui_.click) {
            ui_.used = true;
            const double ms = (ui_.mouse.x - tl.x) / tl.width * len;
            // A click on a clip selects it; on empty time it seeks.
            int hit = -1;
            for (size_t i = 0; i < clips_.size(); ++i) {
                const VisualClip& c = clips_[i];
                const float x0 = xOf(c.startMs), x1 = c.endMs > c.startMs ? xOf(c.endMs) : tl.x + tl.width;
                if (ui_.mouse.x >= x0 && ui_.mouse.x <= x1) { hit = int(i); break; }
            }
            if (hit >= 0) selectClip(hit);
            else seek(ms);
        }
    }

    void drawFields() {
        const float x = 1010, w = gfx::W - x - 40;
        float y = 110;
        DrawRectangleRounded({x - 10, y - 8, w + 20, gfx::H - 152}, 0.02f, 6, kPanel);
        if (sel_ < 0 || sel_ >= int(clips_.size())) {
            gfx::text(FontId::Ui, "Select a clip on the timeline or in the list.", x, y + 8, 18, kDim);
            return;
        }
        VisualClip& c = clips_[size_t(sel_)];
        const float rh = 30;
        gfx::text(FontId::Ui, TextFormat("Clip %d", sel_ + 1), x, y, 20, kGold);
        y += 30;
        if (ui_.text("fstart", {x, y, w * 0.45f, rh}, "Start (s)", fStart_)) { applyFields(); dirty_ = true; }
        if (ui_.text("fend", {x + w * 0.5f, y, w * 0.5f, rh}, "End (s, 0 = song end)", fEnd_)) { applyFields(); dirty_ = true; }
        y += rh + 8;
        static const char* tnames[3] = {"game (playfield)", "hud (receptors)", "both"};
        if (ui_.dropdown("ftarget", {x, y, w, rh}, "Camera", {tnames[0], tnames[1], tnames[2]}, fTarget_)) { applyFields(); dirty_ = true; }
        y += rh + 8;
        std::vector<std::string> opts = fxNames_;
        if (std::find(opts.begin(), opts.end(), c.fx) == opts.end()) opts.insert(opts.begin(), c.fx);
        if (ui_.dropdown("ffx", {x, y, w, rh}, "Effect", opts, fFx_)) { applyFields(); dirty_ = true; }
        y += rh + 12;
        gfx::text(FontId::Ui, "Uniforms (float)", x, y, 17, kDim);
        y += 24;
        for (size_t i = 0; i < fParams_.size(); ++i) {
            if (ui_.text("pk" + std::to_string(i), {x, y, w * 0.5f - 34, rh}, "", fParams_[i].first)) { applyFields(); dirty_ = true; }
            if (ui_.text("pv" + std::to_string(i), {x + w * 0.5f - 28, y, w * 0.5f - 60, rh}, "", fParams_[i].second)) { applyFields(); dirty_ = true; }
            if (ui_.button({x + w - 30, y + 3, 26, rh - 6}, "x")) {
                fParams_.erase(fParams_.begin() + i);
                applyFields();
                dirty_ = true;
                break;
            }
            y += rh + 6;
            if (y > gfx::H - 120) break;
        }
        if (y < gfx::H - 120 && ui_.button({x, y, 140, rh}, "+ uniform")) {
            fParams_.push_back({"uStrength", "0.5"});
            applyFields();
            dirty_ = true;
        }
    }

    void selectClip(int i) {
        sel_ = i;
        syncFields();
    }

    // Fields -> clip (called when a field reports a change and each draw for
    // the param rows, which have no change callback of their own).
    void applyFields() {
        if (sel_ < 0 || sel_ >= int(clips_.size())) return;
        VisualClip& c = clips_[size_t(sel_)];
        try {
            c.startMs = std::max(0.0, std::atof(fStart_.c_str()) * 1000.0);
            const double e = std::atof(fEnd_.c_str());
            c.endMs = e > 0 ? c.startMs + e * 1000.0 : 0;
        } catch (...) {}
        c.target = fTarget_ == 1 ? "hud" : fTarget_ == 2 ? "both" : "game";
        if (fFx_ >= 0 && fFx_ < int(fxNames_.size())) c.fx = fxNames_[size_t(fFx_)];
        c.params.clear();
        for (const auto& [k, v] : fParams_) {
            try {
                c.params[k] = std::stod(v);
            } catch (...) {}
        }
    }

    // Clip -> fields when the selection changes.
    void syncFields() {
        if (sel_ == syncSel_ && clips_.size() == syncN_) return;
        syncSel_ = sel_;
        syncN_ = clips_.size();
        if (sel_ < 0 || sel_ >= int(clips_.size())) return;
        const VisualClip& c = clips_[size_t(sel_)];
        fStart_ = TextFormat("%.3f", c.startMs / 1000.0);
        fEnd_ = c.endMs > c.startMs ? TextFormat("%.3f", (c.endMs - c.startMs) / 1000.0) : "0";
        fTarget_ = c.target == "hud" ? 1 : c.target == "both" ? 2 : 0;
        fFx_ = 0;
        for (size_t i = 0; i < fxNames_.size(); ++i)
            if (fxNames_[i] == c.fx || "__fx_" + fxNames_[i] == c.fx) { fFx_ = int(i); break; }
        fParams_.clear();
        for (const auto& [k, v] : c.params) fParams_.push_back({k, TextFormat("%g", v)});
    }

    void addClip() {
        VisualClip c;
        c.startMs = curMs_;
        c.endMs = 0;
        c.target = "game";
        c.fx = fxNames_.empty() ? "__fx_glitch" : fxNames_[0];
        clips_.push_back(c);
        sel_ = int(clips_.size()) - 1;
        dirty_ = true;
        syncFields();
    }

    void delClip() {
        if (sel_ < 0 || sel_ >= int(clips_.size())) return;
        clips_.erase(clips_.begin() + sel_);
        sel_ = -1;
        dirty_ = true;
    }

    // The real song with visuals.json live: notes hidden, characters and
    // cameras play normally (botplay hits the notes for it).
    void previewSong() {
        if (!song_ || !audioReady_) return;
        App& a = app();
        PlayRequest req;
        req.songId = song_->id;
        req.diff = diff_;
        req.visualsOnly = true;
        a.change(makePlay(req));
    }

    // Back to the song when opened from gameplay (key 8), else the menu.
    void exitEditor() {
        App& a = app();
        if (a.visualsReturn) {
            const PlayRequest req = *a.visualsReturn;
            a.visualsReturn.reset();
            a.change(makePlay(req));
            return;
        }
        a.change(makeEditorMenu());
    }

    void save() {
        if (visualsPath_.empty()) { status_.set("This song has no folder for visuals.json", true); return; }
        applyFields();
        const std::string json = visualsJson(clips_);
        std::string err;
        if (!saveText(visualsPath_, json, err)) { status_.set(err, true); return; }
        dirty_ = false;
        leaveArmed_ = false;
        status_.set("Saved " + visualsPath_);
    }
};

}  // namespace

std::unique_ptr<State> makeEditorMenu() { return std::make_unique<EditorMenuState>(); }
std::unique_ptr<State> makeCharacterEditor() { return std::make_unique<CharacterEditorState>(); }
std::unique_ptr<State> makeStageEditor() { return std::make_unique<StageEditorState>(); }
std::unique_ptr<State> makeEventEditor() { return std::make_unique<EventEditorState>(); }
std::unique_ptr<State> makeVisualsEditor() { return std::make_unique<VisualsEditorState>(); }
// ========================================================= modchart editor ==
// modchart.json: Codename-style modifier tracks for one song. Left: tracks
// and their properties. Centre: live preview (demo playfield + real audio).
// Bottom: the timeline - one row per property, keyframes as diamonds,
// click a lane to add a key, drag a diamond to retime it, Del removes it.
// "Preview song" launches the real song in visualsOnly mode where the mods
// apply live (notes hidden, characters/cameras/FX playing).

class ModchartEditorState : public State {
public:
    void openFor(const std::string& songId, const std::string& diff) {
        wantSongId_ = songId;
        wantDiff_ = diff;
    }

    void enter() override {
        mod_ = currentMod();
        useMod(sc_, mod_);
        buildSongList();
        int pick = -1;
        if (!wantSongId_.empty()) {
            const auto& songs = app().library.songs();
            for (int idx : songIdx_)
                if (songs[size_t(idx)].id == wantSongId_) { pick = idx; break; }
        }
        if (pick < 0) pick = freeplayPick_ >= 0 ? freeplayPick_ : (songIdx_.empty() ? -1 : songIdx_[0]);
        pickSong(pick, wantDiff_);
    }
    bool keyReserved(int key) const override {
        return !ui_.focus.empty() || key == KEY_SPACE || key == KEY_DELETE;
    }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        if (audioReady_) t.keys = {{"PLAY", KEY_SPACE}, {"SAVE", KEY_F2}, {"BACK", KEY_ESCAPE}};
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (job_.valid()) {
            if (job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                Prepared p = job_.get();
                if (p.ok) {
                    prep_ = std::move(p);
                    audioReady_ = true;
                    a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
                    a.mixer.setRate(1.0);
                    dirty_ = false;
                } else {
                    status_.set(p.error.empty() ? "cannot load the song" : p.error, true);
                }
            }
            return;
        }
        if (!audioReady_) {
            if (in.back()) exitEditor();
            return;
        }
        if (playing_) {
            curMs_ = a.mixer.time(a.now) * 1000.0;
            if (a.mixer.ended() || curMs_ > songLenMs() + 1500) {
                a.mixer.pause();
                playing_ = false;
                curMs_ = songLenMs();
            }
        }
        sc_.update(dt, chartStepMs(), false);
        // Keyframe drag: hold LMB on a diamond and move.
        if (dragTrack_ >= 0 && dragProp_ >= 0 && dragKey_ >= 0) {
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                dragTrack_ = dragProp_ = dragKey_ = -1;
            } else {
                const double ms = xToTime(ui_.mouse.x);
                ModProp& mp = mc_.tracks[size_t(dragTrack_)].props[size_t(dragProp_)];
                if (dragKey_ < int(mp.keys.size())) {
                    mp.keys[size_t(dragKey_)].tMs = clampv(ms, 0.0, songLenMs());
                    std::stable_sort(mp.keys.begin(), mp.keys.end(),
                                     [](const ModKey& a, const ModKey& b) { return a.tMs < b.tMs; });
                    dirty_ = true;
                }
            }
        }
        if (in.pressed(KEY_SPACE)) togglePlay();
        if (in.pressed(KEY_F2)) save();
        if (in.pressed(KEY_DELETE) && selKey_ >= 0) delKey();
        if (in.pressed(KEY_ESCAPE)) {
            if (dirty_ && !leaveArmed_) {
                status_.set("Unsaved changes: ESC again to leave without saving (F2 saves)", true);
                leaveArmed_ = true;
            } else {
                exitEditor();
            }
        }
        clampSel();
        const std::string sig = std::to_string(selTrack_) + "," + std::to_string(selProp_) + "," + std::to_string(selKey_);
        if (sig != selSig_) { selSig_ = sig; syncKeyFields(); }
    }

    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x6C8CFF, 0);
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.45f));
        gfx::text(FontId::Title, "MODCHART EDITOR", 40, 18, 40, kGold, Align::Left, 2.0f);
        drawHeader();
        if (!audioReady_) {
            gfx::text(FontId::Ui, "Loading " + (song_ ? song_->title : std::string("song")) + "...", gfx::W / 2, gfx::H / 2, 26, kIvory, Align::Center);
            hints("ESC back");
            ui_.overlays();
            status_.draw();
            gfx::endCanvas();
            return;
        }
        drawTrackPanel();
        drawPreview();
        drawTimeline();
        drawKeyFields();
        ui_.overlays();
        hints("SPACE play/pause   Timeline: click lane = add key, drag diamond = retime, DEL = delete key   F2 save   ESC back");
        status_.draw();
        gfx::endCanvas();
    }

private:
    scene::Scene sc_;
    Ui ui_;
    Status status_;
    ModChoice mod_;
    std::vector<int> songIdx_;
    int songPos_ = 0;
    const SongEntry* song_ = nullptr;
    std::string diff_;
    std::future<Prepared> job_;
    Prepared prep_;
    bool audioReady_ = false, playing_ = false, dirty_ = false, leaveArmed_ = false;
    double curMs_ = 0;
    Modchart mc_;
    std::string mcPath_;
    int selTrack_ = -1, selProp_ = -1, selKey_ = -1;
    int dragTrack_ = -1, dragProp_ = -1, dragKey_ = -1;
    int addTarget_ = 0;
    std::string fT_, fV_;
    int fEaseIdx_ = 0;
    std::string wantSongId_, wantDiff_;
    int freeplayPick_ = -1;
    std::string selSig_;

    static const char* kEaseNames[5];

    // ---- song / file plumbing (same shape as the Visuals Editor) ----

    void buildSongList() {
        const auto& songs = app().library.songs();
        for (int i = 0; i < int(songs.size()); ++i) {
            const SongEntry& e = songs[size_t(i)];
            if (e.builtin || e.diffs.empty()) continue;
            if (wantSongId_.empty() && !mod_.name.empty() && e.pack != mod_.name) continue;
            songIdx_.push_back(i);
        }
        if (songIdx_.empty())
            for (int i = 0; i < int(songs.size()); ++i)
                if (!songs[size_t(i)].builtin && !songs[size_t(i)].diffs.empty()) songIdx_.push_back(i);
        const int fp = app().freeplaySel;
        if (fp >= 0 && fp < int(songs.size()) && !songs[size_t(fp)].builtin &&
            (mod_.name.empty() || songs[size_t(fp)].pack == mod_.name))
            freeplayPick_ = fp;
    }

    void pickSong(int libIdx, const std::string& diff = "") {
        const auto& songs = app().library.songs();
        if (libIdx < 0 || libIdx >= int(songs.size())) return;
        auto it = std::find(songIdx_.begin(), songIdx_.end(), libIdx);
        songPos_ = it != songIdx_.end() ? int(it - songIdx_.begin()) : 0;
        song_ = &songs[size_t(libIdx)];
        diff_ = diff;
        if (diff_.empty() || std::find(song_->diffs.begin(), song_->diffs.end(), diff_) == song_->diffs.end())
            diff_ = song_->diffs.empty() ? "" : song_->diffs.front();
        loadChart();
        const SongEntry copy = *song_;
        const std::string d = diff_;
        job_ = std::async(std::launch::async, [copy, d] { return app().library.prepare(copy, d, false); });
        audioReady_ = false;
        playing_ = false;
        selTrack_ = selProp_ = selKey_ = -1;
    }

    void cycleSong(int d) {
        if (songIdx_.empty() || job_.valid()) return;
        songPos_ = ((songPos_ + d) % int(songIdx_.size()) + int(songIdx_.size())) % int(songIdx_.size());
        pickSong(songIdx_[size_t(songPos_)]);
    }

    std::string mcFile() const { return song_ && !song_->dataDir.empty() ? song_->dataDir + "/modchart.json" : ""; }

    void loadChart() {
        mc_ = Modchart{};
        mcPath_ = mcFile();
        std::string text;
        if (!mcPath_.empty() && scene::readFile(mcPath_, text)) {
            std::string err;
            if (!parseModchart(text, mc_, err)) status_.set("modchart.json: " + err, true);
        }
        dirty_ = false;
        leaveArmed_ = false;
        selTrack_ = selProp_ = selKey_ = -1;
    }

    double songLenMs() const {
        double len = prep_.chart.lengthMs;
        if (prep_.inst) len = std::max(len, double(prep_.inst->frames()) * 1000.0 / Mixer::kRate);
        return std::max(1000.0, len);
    }
    double chartStepMs() const { return prep_.chart.tempo.empty() ? 500.0 : prep_.chart.stepMs(std::max(0.0, curMs_)); }

    void togglePlay() {
        App& a = app();
        if (playing_) { a.mixer.pause(); playing_ = false; return; }
        a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
        a.mixer.setRate(1.0);
        a.mixer.play(std::max(0.0, curMs_) / 1000.0);
        playing_ = true;
    }
    void seek(double ms) {
        curMs_ = clampv(ms, 0.0, songLenMs());
        app().mixer.seek(curMs_ / 1000.0);
    }

    // ---- selection helpers ----

    void clampSel() {
        selTrack_ = clampv(selTrack_, -1, int(mc_.tracks.size()) - 1);
        if (selTrack_ < 0) { selProp_ = selKey_ = -1; return; }
        const ModTrack& tr = mc_.tracks[size_t(selTrack_)];
        selProp_ = clampv(selProp_, -1, int(tr.props.size()) - 1);
        if (selProp_ < 0 || selProp_ >= int(tr.props.size())) { selKey_ = -1; return; }
        selKey_ = clampv(selKey_, -1, int(tr.props[size_t(selProp_)].keys.size()) - 1);
    }

    // Timeline geometry shared by draw and hit-testing.
    float tlX() const { return 320; }
    float tlW() const { return gfx::W - tlX() - 20; }
    float tlY() const { return 500; }
    float rowH() const { return 22; }
    double xToTime(float x) const { return (x - tlX()) / tlW() * songLenMs(); }
    float timeToX(double ms) const { return tlX() + float(ms / songLenMs()) * tlW(); }

    struct RowRef { int track, prop; };  // prop -1 = the track's own row (shader span)
    std::vector<RowRef> rows() const {
        std::vector<RowRef> out;
        for (int t = 0; t < int(mc_.tracks.size()); ++t) {
            const ModTrack& tr = mc_.tracks[size_t(t)];
            if (tr.target == "shader" || tr.props.empty()) {
                out.push_back({t, -1});
            } else {
                for (int p = 0; p < int(tr.props.size()); ++p) out.push_back({t, p});
            }
        }
        return out;
    }

    // ---- drawing ----

    void drawHeader() {
        const float y = 66;
        if (ui_.button({gfx::W - 330, y - 6, 60, 30}, "<")) cycleSong(-1);
        if (ui_.button({gfx::W - 264, y - 6, 60, 30}, ">")) cycleSong(1);
        const std::string label = song_ ? song_->title + "  (" + diff_ + ")" : "no song";
        gfx::text(FontId::Ui, label, gfx::W - 196, y, 20, kIvory, Align::Right);
        gfx::text(FontId::Ui, "modchart.json - property tracks for this song", 40, y, 18, kDim);
        if (ui_.button({gfx::W - 240, y - 6, 110, 30}, "Preview song")) previewSong();
        if (ui_.button({gfx::W - 120, y - 6, 100, 30}, "Save (F2)")) save();
    }

    void drawTrackPanel() {
        const Rectangle box{40, 110, 260, gfx::H - 200};
        DrawRectangleRounded(box, 0.02f, 6, kPanel);
        gfx::text(FontId::Ui, "TRACKS", box.x + 12, box.y + 6, 16, kDim);
        std::vector<std::string> opts;
        for (const auto& [n, label] : modTargets()) opts.push_back(n);
        if (ui_.dropdown("addtarget", {box.x + 8, box.y + 26, box.width - 16, 30}, "", opts, addTarget_)) {
            addTrack(modTargets()[size_t(addTarget_)].first);
        }
        float y = box.y + 66;
        const auto rr = rows();
        for (const RowRef& r : rr) {
            const ModTrack& tr = mc_.tracks[size_t(r.track)];
            const bool isTrackRow = r.prop < 0;
            const float indent = isTrackRow ? 4.0f : 22.0f;
            const std::string name = isTrackRow ? tr.target
                : tr.target + "  " + tr.props[size_t(r.prop)].name;
            const Rectangle row{box.x + indent, y, box.width - indent - 8, 24};
            const bool sel = r.track == selTrack_ && (isTrackRow ? selProp_ < 0 : r.prop == selProp_);
            if (sel || ui_.hover(row)) DrawRectangleRec(row, sel ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
            gfx::text(FontId::Ui, name, row.x + 6, row.y + 3, 16, sel ? kAccent : kInk);
            if (!isTrackRow) {
                const ModProp& mp = tr.props[size_t(r.prop)];
                const double v = evalProp(mp, curMs_);
                gfx::text(FontId::Ui, TextFormat("%.2f", v), row.x + row.width - 62, row.y + 3, 15, kDim, Align::Right);
                if (!mp.keys.empty())
                    gfx::text(FontId::Ui, TextFormat("%d", int(mp.keys.size())), row.x + row.width - 8, row.y + 3, 14, kDim, Align::Right);
            }
            if (ui_.hover(row) && ui_.click) {
                ui_.used = true;
                selTrack_ = r.track;
                selProp_ = r.prop;
                selKey_ = -1;
            }
            y += 25;
            if (y > box.y + box.height - 8) break;
        }
        if (ui_.button({box.x + 8, box.y + box.height - 36, box.width - 16, 30}, "Delete track")) delTrack();
    }

    void drawPreview() {
        const Rectangle vp{300, 110, 760, 380};
        DrawRectangleRounded(vp, 0.02f, 6, kPanel);
        gfx::text(FontId::Ui, "PREVIEW", vp.x + 12, vp.y + 6, 16, kDim);
        DrawRectangleRec(vp, Color{14, 10, 24, 255});
        const int lanes = prep_.chart.keys > 0 ? prep_.chart.keys : 4;
        const double t = app().now;
        for (int side = 0; side < 2; ++side) {
            const float base = side == 0 ? vp.x : vp.x + vp.width * 0.5f;
            const float lw = vp.width * 0.5f / float(lanes);
            for (int l = 0; l < lanes; ++l) {
                const float x = base + l * lw;
                DrawRectangleRec({x + 2, vp.y + vp.height * 0.08f, lw - 4, vp.height * 0.84f}, Color{30, 24, 48, 255});
                const float ry = side == 0 ? vp.y + vp.height * 0.9f : vp.y + vp.height * 0.1f;
                DrawRectangleRec({x + lw * 0.22f, ry - 8, lw * 0.56f, 16},
                                 side == 0 ? Color{120, 200, 255, 220} : Color{255, 120, 160, 220});
                for (int n = 0; n < 3; ++n) {
                    const double ph = std::fmod(t * 0.35 + double(l * 7 + n * 13 + side * 31), 3.0);
                    const float ny = vp.y + vp.height * 0.05f + float(ph / 3.0) * vp.height * 0.8f;
                    DrawRectangleRec({x + lw * 0.3f, ny, lw * 0.4f, 10},
                                     side == 0 ? Color{90, 170, 255, 255} : Color{255, 110, 140, 255});
                }
            }
        }
        const float ty = vp.y + vp.height + 10;
        if (ui_.button({vp.x, ty, 90, 30}, playing_ ? "Pause" : "Play")) togglePlay();
        if (ui_.button({vp.x + 100, ty, 70, 30}, "Stop")) {
            seek(0);
            if (playing_) { app().mixer.pause(); playing_ = false; }
        }
        gfx::text(FontId::Ui, TextFormat("%.2f / %.2f s", curMs_ / 1000.0, songLenMs() / 1000.0), vp.x + 190, ty + 5, 20, kIvory);
    }

    void drawTimeline() {
        const Rectangle tl{tlX() - 4, tlY(), tlW() + 4, gfx::H - tlY() - 52};
        DrawRectangleRounded(tl, 0.02f, 6, Color{20, 16, 30, 255});
        const auto rr = rows();
        DrawRectangleRec({tlX(), tlY(), tlW(), 24}, Color{26, 20, 40, 255});
        for (double s = 0; s <= songLenMs() / 1000.0; s += 10) {
            const float x = timeToX(s * 1000.0);
            DrawLineEx({x, tlY() + 24}, {x, tlY() + tl.height}, 1, Color{60, 50, 90, 255});
            gfx::text(FontId::Ui, TextFormat("%d", int(s)), x + 2, tlY() + 4, 13, kDim);
        }
        if (ui_.hover({tlX(), tlY(), tlW(), 24}) && ui_.click) {
            ui_.used = true;
            seek(xToTime(ui_.mouse.x));
        }
        float y = tlY() + 30;
        for (const RowRef& r : rr) {
            const Rectangle lane{tlX(), y, tlW(), rowH() - 3};
            const bool rowSel = r.track == selTrack_ && r.prop == selProp_;
            if (rowSel || (ui_.hover(lane) && !ui_.click))
                DrawRectangleRec(lane, rowSel ? Color{90, 66, 140, 60} : Color{50, 38, 76, 40});
            DrawLineEx({tlX(), y + rowH() - 3}, {tlX() + tlW(), y + rowH() - 3}, 1, Color{45, 36, 70, 255});
            const ModTrack& tr = mc_.tracks[size_t(r.track)];
            std::vector<std::pair<int, int>> keyPos;
            if (r.prop >= 0) {
                const ModProp& mp = tr.props[size_t(r.prop)];
                for (int k = 0; k < int(mp.keys.size()); ++k) {
                    const float kx = timeToX(mp.keys[size_t(k)].tMs);
                    keyPos.push_back({k, int(kx)});
                    const bool ksel = rowSel && k == selKey_;
                    const Color kc = ksel ? kAccent : Color{140, 220, 255, 255};
                    DrawRectangleRec({kx - 4, y + 4, 8, rowH() - 11}, kc);
                    DrawRectangleRec({kx - 2, y + 6, 4, rowH() - 15}, Color{20, 16, 30, 255});
                }
            } else if (tr.target == "shader") {
                const float x0 = timeToX(tr.clip.startMs);
                const float x1 = tr.clip.endMs > tr.clip.startMs ? timeToX(tr.clip.endMs) : tlX() + tlW();
                DrawRectangleRec({x0, y + 5, std::max(4.0f, x1 - x0), rowH() - 13}, fade(Color{255, 170, 80, 255}, 0.8f));
            }
            if (ui_.hover(lane) && ui_.click) {
                ui_.used = true;
                int hit = -1;
                for (const auto& [ki, kx] : keyPos)
                    if (std::fabs(ui_.mouse.x - kx) <= 6) { hit = ki; break; }
                selTrack_ = r.track;
                selProp_ = r.prop;
                if (hit >= 0) {
                    selKey_ = hit;
                    dragTrack_ = r.track; dragProp_ = r.prop; dragKey_ = hit;
                } else if (r.prop >= 0) {
                    addKey(r.track, r.prop, xToTime(ui_.mouse.x));
                    selProp_ = r.prop;
                    selKey_ = int(mc_.tracks[size_t(r.track)].props[size_t(r.prop)].keys.size()) - 1;
                    dirty_ = true;
                } else {
                    selKey_ = -1;
                }
            }
            y += rowH();
            if (y > tl.y + tl.height - 4) break;
        }
        DrawLineEx({timeToX(curMs_), tlY()}, {timeToX(curMs_), tl.y + tl.height}, 2, kGold);
    }

    void drawKeyFields() {
        const float x = gfx::W - 300, w = 280;
        float y = tlY() + 8;
        if (selTrack_ < 0 || selTrack_ >= int(mc_.tracks.size())) return;
        ModTrack& tr = mc_.tracks[size_t(selTrack_)];
        DrawRectangleRounded({x - 8, y - 6, w + 16, 120}, 0.02f, 6, kPanel);
        gfx::text(FontId::Ui, tr.target, x, y, 18, kGold);
        y += 26;
        if (tr.target == "shader") {
            gfx::text(FontId::Ui, "fx: " + tr.clip.fx, x, y, 15, kDim);
            y += 20;
            if (ui_.text("shstart", {x, y, w * 0.5f, 28}, "start s", fT_)) { applyClipFields(); dirty_ = true; }
            if (ui_.text("shend", {x + w * 0.5f, y, w * 0.5f, 28}, "end s", fV_)) { applyClipFields(); dirty_ = true; }
            return;
        }
        if (selProp_ < 0 || selProp_ >= int(tr.props.size())) {
            gfx::text(FontId::Ui, "Select a property row, then a key.", x, y, 15, kDim);
            return;
        }
        ModProp& mp = tr.props[size_t(selProp_)];
        gfx::text(FontId::Ui, mp.name, x, y, 18, kIvory);
        y += 24;
        if (selKey_ >= 0 && selKey_ < int(mp.keys.size())) {
            ModKey& k = mp.keys[size_t(selKey_)];
            char id[32];
            std::snprintf(id, sizeof id, "k%d", selKey_);
            if (ui_.text("ktt" + std::string(id), {x, y, w * 0.32f, 28}, "t s", fT_)) { applyKeyFields(); dirty_ = true; }
            if (ui_.text("kvv" + std::string(id), {x + w * 0.36f, y, w * 0.3f, 28}, "v", fV_)) { applyKeyFields(); dirty_ = true; }
            static const char* en[5] = {"linear", "easeIn", "easeOut", "easeInOut", "smooth"};
            if (ui_.dropdown("kev" + std::string(id), {x + w * 0.7f, y, w * 0.3f, 28}, "", {en[0], en[1], en[2], en[3], en[4]}, fEaseIdx_)) {
                applyKeyFields();
                dirty_ = true;
            }
            gfx::text(FontId::Ui, std::string("value now: ") + TextFormat("%.3f", evalProp(mp, curMs_)), x, y + 32, 15, kDim);
        } else {
            gfx::text(FontId::Ui, TextFormat("%d keys - click a lane to add, drag to retime", int(mp.keys.size())), x, y, 15, kDim);
            gfx::text(FontId::Ui, std::string("value now: ") + TextFormat("%.3f", evalProp(mp, curMs_)), x, y + 20, 15, kDim);
            if (ui_.button({x, y + 44, w, 28}, "Add key at playhead")) { addKey(selTrack_, selProp_, curMs_); dirty_ = true; }
        }
    }

    // ---- editing ops ----

    void addTrack(const std::string& target) {
        ModTrack tr;
        tr.target = target;
        if (target == "shader") {
            tr.clip.startMs = curMs_;
            tr.clip.endMs = 0;
            tr.clip.target = "game";
            tr.clip.fx = "glitch";
        } else {
            for (const auto& [pn, nv] : modProps(target)) {
                ModProp mp;
                mp.name = pn;
                mp.neutral = nv;
                tr.props.push_back(mp);
            }
        }
        mc_.tracks.push_back(tr);
        selTrack_ = int(mc_.tracks.size()) - 1;
        selProp_ = tr.props.empty() ? -1 : 0;
        selKey_ = -1;
        dirty_ = true;
    }

    void delTrack() {
        if (selTrack_ < 0 || selTrack_ >= int(mc_.tracks.size())) return;
        mc_.tracks.erase(mc_.tracks.begin() + selTrack_);
        selTrack_ = selProp_ = selKey_ = -1;
        dirty_ = true;
    }

    void addKey(int t, int p, double ms) {
        if (t < 0 || t >= int(mc_.tracks.size())) return;
        ModProp& mp = mc_.tracks[size_t(t)].props[size_t(p)];
        ModKey k;
        k.tMs = clampv(ms, 0.0, songLenMs());
        k.value = evalProp(mp, k.tMs);
        k.ease = "linear";
        mp.keys.push_back(k);
        std::stable_sort(mp.keys.begin(), mp.keys.end(),
                         [](const ModKey& a, const ModKey& b) { return a.tMs < b.tMs; });
    }

    void delKey() {
        if (selTrack_ < 0 || selProp_ < 0 || selKey_ < 0) return;
        ModProp& mp = mc_.tracks[size_t(selTrack_)].props[size_t(selProp_)];
        if (selKey_ >= int(mp.keys.size())) return;
        mp.keys.erase(mp.keys.begin() + selKey_);
        selKey_ = -1;
        dirty_ = true;
    }

    void syncKeyFields() {
        if (selTrack_ < 0 || selProp_ < 0) { fT_.clear(); fV_.clear(); fEaseIdx_ = 0; return; }
        const ModTrack& tr = mc_.tracks[size_t(selTrack_)];
        if (tr.target == "shader") {
            fT_ = TextFormat("%.2f", tr.clip.startMs / 1000.0);
            fV_ = tr.clip.endMs > tr.clip.startMs ? TextFormat("%.2f", (tr.clip.endMs - tr.clip.startMs) / 1000.0) : "0";
            return;
        }
        const ModProp& mp = tr.props[size_t(selProp_)];
        if (selKey_ < 0 || selKey_ >= int(mp.keys.size())) { fT_.clear(); fV_.clear(); return; }
        const ModKey& k = mp.keys[size_t(selKey_)];
        fT_ = TextFormat("%.3f", k.tMs / 1000.0);
        fV_ = TextFormat("%g", k.value);
        fEaseIdx_ = 0;
        for (int i = 0; i < 5; ++i)
            if (k.ease == kEaseNames[i]) { fEaseIdx_ = i; break; }
    }

    void applyKeyFields() {
        if (selTrack_ < 0 || selProp_ < 0 || selKey_ < 0) return;
        ModKey& k = mc_.tracks[size_t(selTrack_)].props[size_t(selProp_)].keys[size_t(selKey_)];
        k.tMs = clampv(std::atof(fT_.c_str()) * 1000.0, 0.0, songLenMs());
        k.value = std::atof(fV_.c_str());
        k.ease = kEaseNames[clampv(fEaseIdx_, 0, 4)];
        ModProp& mp = mc_.tracks[size_t(selTrack_)].props[size_t(selProp_)];
        std::stable_sort(mp.keys.begin(), mp.keys.end(),
                         [](const ModKey& a, const ModKey& b) { return a.tMs < b.tMs; });
    }

    void applyClipFields() {
        if (selTrack_ < 0) return;
        ModTrack& tr = mc_.tracks[size_t(selTrack_)];
        if (tr.target != "shader") return;
        tr.clip.startMs = std::max(0.0, std::atof(fT_.c_str()) * 1000.0);
        const double e = std::atof(fV_.c_str());
        tr.clip.endMs = e > 0 ? tr.clip.startMs + e * 1000.0 : 0;
    }

    void previewSong() {
        if (!song_ || !audioReady_) return;
        App& a = app();
        PlayRequest req;
        req.songId = song_->id;
        req.diff = diff_;
        req.visualsOnly = true;
        a.change(makePlay(req));
    }

    void exitEditor() {
        App& a = app();
        if (a.visualsReturn) {
            const PlayRequest req = *a.visualsReturn;
            a.visualsReturn.reset();
            a.change(makePlay(req));
            return;
        }
        a.change(makeEditorMenu());
    }

    void save() {
        if (mcPath_.empty()) { status_.set("This song has no folder for modchart.json", true); return; }
        std::string err;
        if (!saveText(mcPath_, modchartJson(mc_), err)) { status_.set(err, true); return; }
        dirty_ = false;
        leaveArmed_ = false;
        status_.set("Saved " + mcPath_);
    }
};

const char* ModchartEditorState::kEaseNames[5] = {"linear", "easeIn", "easeOut", "easeInOut", "smooth"};

std::unique_ptr<State> makeVisualsEditorFor(const std::string& songId, const std::string& diff) {
    auto s = std::make_unique<VisualsEditorState>();
    s->openFor(songId, diff);
    return s;
}
std::unique_ptr<State> makeModchartEditor() { return std::make_unique<ModchartEditorState>(); }
std::unique_ptr<State> makeModchartEditorFor(const std::string& songId, const std::string& diff) {
    auto s = std::make_unique<ModchartEditorState>();
    s->openFor(songId, diff);
    return s;
}

}  // namespace gh
