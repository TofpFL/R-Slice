// Title, main menu, story, freeplay and credits screens.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "characters.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "raylib.h"
#include "scene.hpp"
#include "title_atlas.hpp"
#include "video.hpp"
#include "vslice_ui.hpp"

#ifndef GH_VERSION
#define GH_VERSION "dev"  // set by CMake from project(VERSION)
#endif

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {

const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};

// assets/menu/raid_engine_logo.png, loaded once; id == 0 when absent.
Texture2D menuLogo() {
    static Texture2D tex = [] {
        Texture2D t{};
        const std::string path = app().base + "assets/menu/raid_engine_logo.png";
        if (FileExists(path.c_str())) t = LoadTexture(path.c_str());
        return t;
    }();
    return tex;
}

void drawLogo(float cx, float top, float w) {
    const Texture2D logo = menuLogo();
    if (!logo.id) return;
    const float h = w * float(logo.height) / float(logo.width);
    DrawTexturePro(logo, {0, 0, float(logo.width), float(logo.height)}, {cx - w / 2, top, w, h}, {0, 0}, 0, WHITE);
}

Color diffColor(const std::string& d) {
    const std::string l = toLower(d);
    if (l == "easy") return Color{120, 220, 140, 255};
    if (l == "normal") return Color{255, 214, 100, 255};
    if (l == "hard") return Color{255, 120, 90, 255};
    if (l == "infernal") return Color{255, 50, 70, 255};
    return Color{190, 160, 255, 255};
}

void hints(const char* s) {
    if (app().touchMode) return;  // the on-screen buttons speak for themselves
    DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
    gfx::text(FontId::Ui, s, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
}

double lastBeatTime() {
    const double b = app().menuBeat();
    const double frac = b - std::floor(b);
    return app().now - frac * 60.0 / app().menuBpm();
}

std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush = [&] {
        if (cur.size() + word.size() + (cur.empty() ? 0 : 1) > width && !cur.empty()) {
            lines.push_back(cur);
            cur.clear();
        }
        if (!word.empty()) cur += (cur.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Background jobs that must not block the UI (or the exit) while composing.
std::atomic<int> g_jobs{0};
struct PcmJob {
    struct Slot {
        std::mutex m;
        bool ready = false;
        PcmPtr pcm;
    };
    std::shared_ptr<Slot> slot;
    template <class F> void start(F fn) {
        slot = std::make_shared<Slot>();
        auto s = slot;
        ++g_jobs;
        std::thread([s, fn] {
            PcmPtr r = fn();
            {
                std::lock_guard<std::mutex> lk(s->m);
                s->pcm = r;
                s->ready = true;
            }
            --g_jobs;
        }).detach();
    }
    bool running() const { return slot != nullptr; }
    bool ready() const {
        if (!slot) return false;
        std::lock_guard<std::mutex> lk(slot->m);
        return slot->ready;
    }
    PcmPtr take() {
        PcmPtr r;
        if (slot) {
            std::lock_guard<std::mutex> lk(slot->m);
            r = slot->pcm;
        }
        slot.reset();
        return r;
    }
};

}  // namespace

void waitForBackgroundJobs() {
    for (int i = 0; i < 600 && g_jobs.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// -------------------------------------------------------------- Startup --

// Boot cutscene: plays assets/videos/startup.* through VideoPlayer (an .mp4
// is converted to MPEG-1 once by ffmpeg into cache/videos/). ENTER skips;
// on skip or end the screen fades to black and hands off to the title.

class StartupState : public State {
public:
    StartupState() { files_.setRoots({app().base + "assets"}); }

    void enter() override {
        retryAt_ = 0;
        resolve();
    }

    bool wantsMenuMusic() const override { return false; }  // the video carries its own audio

    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_ENTER;
        return t;
    }

    void update(double dt) override {
        App& a = app();
        if (leaving_) {
            fade_ = std::min(1.0f, fade_ + float(dt) / 0.6f);
            if (fade_ >= 1.0f) a.change(makeTitle());
            return;
        }
        if (!player_) {
            if (failed_) { leaving_ = true; return; }
            if (a.now >= retryAt_) resolve();
            return;
        }
        player_->update(dt);
        if (player_->ended() || a.input.accept()) {
            leaving_ = true;
            fade_ = 0.0f;
        }
    }

    void draw() override {
        gfx::beginCanvas();
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, BLACK);
        if (player_ && player_->texture().id) {
            const Texture2D& t = player_->texture();
            const float sc = std::min(gfx::W / float(t.width), gfx::H / float(t.height));
            const float w = t.width * sc, h = t.height * sc;
            DrawTexturePro(t, {0, 0, float(t.width), float(t.height)},
                           {(gfx::W - w) * 0.5f, (gfx::H - h) * 0.5f, w, h}, {0, 0}, 0, WHITE);
            if (!leaving_)
                gfx::text(FontId::Ui, "PRESS ENTER TO SKIP", gfx::W - 30, gfx::H - 40, 20,
                          fade(WHITE, 0.45f), Align::Right);
        } else if (!failed_) {
            gfx::text(FontId::Ui, "Loading intro...", gfx::W / 2, gfx::H / 2 - 15, 30,
                      fade(WHITE, 0.8f), Align::Center);
        }
        if (fade_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, fade_));
        gfx::endCanvas();
    }

private:
    void resolve() {
        App& a = app();
        retryAt_ = a.now + 0.5;  // poll while ffmpeg converts in the background
        const VideoSource src = findVideo(files_, "startup", a.base);
        if (src.converting) return;
        if (!src.error.empty()) { failed_ = true; return; }  // no video/ffmpeg: straight to title
        player_ = std::make_unique<VideoPlayer>();
        if (!player_->open(src.playable, false, true)) {
            player_.reset();
            failed_ = true;
            return;
        }
        player_->setVolume(a.settings.musicVol);
    }

    scene::Files files_;
    std::unique_ptr<VideoPlayer> player_;
    double retryAt_ = 0;
    bool leaving_ = false, failed_ = false;
    float fade_ = 0;
};

// ------------------------------------------------------------------ Title --

// V-Slice title screen (mikolka/vslice/ui/title/TitleState.hx): solid dark
// backdrop, the GF dance sprite beat-synced on the speaker stack at
// bottom-right, the game logo top-left with a drop shadow, and the
// "PRESS ENTER" strip flashing cyan/blue. Enter plays the pressed frames,
// flashes white, then hands off to the main menu.

struct TitleAssets {
    TitleAtlas gf, enter, stage, speakers;
};

// "RAID Engine Bop" frame sequence (assets/title/bop/f001.png..fNNN.png),
// extracted from the .mov at 960x540/30fps. Loaded once on first use.
const std::vector<Texture2D>& bopFrames() {
    static std::vector<Texture2D> frames = [] {
        std::vector<Texture2D> v;
        const std::string dir = app().base + "assets/title/bop/";
        for (int i = 1;; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "f%03d.png", i);
            const std::string path = dir + name;
            if (!FileExists(path.c_str())) break;
            Texture2D t = LoadTexture(path.c_str());
            if (t.id) {
                SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
                v.push_back(t);
            }
            if (i > 600) break;
        }
        return v;
    }();
    return frames;
}

const TitleAssets& titleAssets() {
    static TitleAssets a = [] {
        TitleAssets t;
        const std::string dir = app().base + "assets/title/";
        std::string err;
        loadAtlasXml(dir + "gfDanceTitle.png", dir + "gfDanceTitle.xml", t.gf, &err);
        loadAtlasXml(dir + "titleEnter.png", dir + "titleEnter.xml", t.enter, &err);
        loadAtlasJson(dir + "stage_full.png", dir + "stage_full.json", t.stage, &err);
        loadAtlasJson(dir + "speakers.png", dir + "speakers.json", t.speakers, &err);
        return t;
    }();
    return a;
}

// Bottom-center anchored blit: `h` is the drawn pixel height, `cy` the
// horizontal center, `bottom` the ground line the sprite stands on.
void drawAtlasFrame(const TitleAtlas& at, const char* name, float cx, float bottom, float h,
                    Color tint) {
    const AtlasFrame* f = at.find(name);
    if (!f || f->src.width <= 0 || f->src.height <= 0) return;
    const float s = h / f->src.height;
    DrawTexturePro(at.tex, f->src,
                   {cx - f->src.width * s * 0.5f, bottom - f->src.height * s, f->src.width * s,
                    f->src.height * s},
                   {0, 0}, 0, tint);
}

class TitleState : public State {
public:
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_ENTER;
        return t;
    }

    void enter() override { logoT0_ = app().now; }

    void update(double) override {
        App& a = app();
        flash_ = std::max(0.0f, flash_ - float(a.dt) * 1.8f);
        if (a.input.pressed(KEY_F3)) debug_ = !debug_;
        // Beat-synced dance: each beat flips GF's direction and restarts the
        // frame cycle, like TitleState.hx's beatHit().
        const long beat = (long)std::floor(a.menuBeat());
        if (beat != beatSeen_) {
            beatSeen_ = beat;
            danceRight_ = !danceRight_;
            animT_ = a.now;
        }
        fps_ = fps_ <= 0 ? 1.0 / std::max(a.dt, 1e-4) : fps_ * 0.92 + (1.0 / std::max(a.dt, 1e-4)) * 0.08;
        if (!leaving_ && (a.input.accept() || a.input.firstPressed() != 0) && !a.input.pressed(KEY_F11)) {
            leaving_ = true;
            leaveAt_ = a.now;
            flash_ = 1.0f;
            sfxPlay(Sfx::Confirm);
        }
        if (leaving_ && a.now - leaveAt_ > 0.75) a.change(makeMainMenu());
    }

    void draw() override {
        App& a = app();
        const double now = a.now;
        gfx::beginCanvas();
        DrawRectangle(0, 0, gfx::W, gfx::H, Color{9, 9, 13, 255});

        const TitleAssets& ta = titleAssets();

        // Speaker stack, bottom-right, flush with the window's bottom edge.
        if (ta.speakers.tex.id) drawAtlasFrame(ta.speakers, "0000", 1140, 712, 430, WHITE);

        // GF dance: two 15-frame cycles swapped every beat at 24 fps. She is
        // drawn large in front of the speakers, feet near the bottom edge.
        if (ta.gf.tex.id) {
            static const int kLeft[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
            static const int kRight[] = {15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
            const int* list = danceRight_ ? kRight : kLeft;
            const int fi = int((now - animT_) * 24.0) % 15;
            char name[32];
            std::snprintf(name, sizeof(name), "gfDance%04d", list[fi]);
            // Dark cyan halo + light blue core glow behind GF.
            gfx::glow({950, 430}, 300, fade(Color{20, 90, 110, 255}, 0.55f));
            gfx::glow({950, 430}, 190, fade(Color{90, 200, 235, 255}, 0.35f));
            gfx::glow({990, 330}, 120, fade(Color{140, 230, 255, 255}, 0.30f));
            drawAtlasFrame(ta.gf, name, 950, 705, 560, WHITE);
        }

        // RAID logo, top-left, with a soft drop shadow and a cyan glow so it
        // stands out on the black backdrop. It bops once per menu-music beat
        // (menuPulse peaks at 1.0 on the beat and decays) - tempo-locked, so
        // it looks identical at any frame rate.
        const std::vector<Texture2D>& bop = bopFrames();
        const Texture2D logo = menuLogo();
        const float logoPulse = menuPulse();
        if (!bop.empty()) {
            // The .mov bop: 30 fps frame sequence, looped, with the same cyan
            // glow treatment the static logo got.
            const int fi = int((now - logoT0_) * 30.0) % int(bop.size());
            const Texture2D& f = bop[size_t(fi)];
            const float w = 570.0f, h = w * float(f.height) / float(f.width);
            gfx::glow({100, 80}, 320 + 40 * logoPulse, fade(Color{24, 110, 140, 255}, 0.55f + 0.2f * logoPulse));
            gfx::glow({100, 80}, 200, fade(Color{110, 215, 245, 255}, 0.30f));
            DrawTexturePro(f, {0, 0, float(f.width), float(f.height)}, {62, 62, w, h}, {0, 0}, 0, WHITE);
        } else if (logo.id) {
            const float w = 460.0f * (1.0f + 0.035f * logoPulse);
            const float h = w * float(logo.height) / float(logo.width);
            const float lx = 92.0f - 8.0f * logoPulse, ly = 58.0f - 7.0f * logoPulse;
            const Rectangle src{0, 0, float(logo.width), float(logo.height)};
            gfx::glow({110, 70}, 320 + 40 * logoPulse, fade(Color{24, 110, 140, 255}, 0.55f + 0.2f * logoPulse));
            gfx::glow({110, 70}, 200, fade(Color{110, 215, 245, 255}, 0.30f));
            DrawTexturePro(logo, src, {lx + 7, ly + 7, w, h}, {0, 0}, 0, Color{0, 0, 0, 150});
            DrawTexturePro(logo, src, {lx, ly, w, h}, {0, 0}, 0, WHITE);
        } else {
            gfx::glowText(FontId::Title, "RAID ENGINE", 280, 120, 84, Color{140, 230, 255, 255},
                          Color{40, 140, 190, 255});
        }

        // "PRESS ENTER TO BEGIN" - ENTER IDLE flashes cyan/blue like the
        // vanilla titleTextColors swap; ENTER PRESSED shows while leaving.
        if (ta.enter.tex.id) {
            if (leaving_) {
                const char* pn = (std::fmod(float(now) * 11.0f, 2.0f) < 1.0f) ? "ENTER PRESSED0000"
                                                                           : "ENTER PRESSED0001";
                drawAtlasFrame(ta.enter, pn, gfx::W / 2, 648, 40, WHITE);
            } else {
                // Smooth cyan<->blue fade with a breathing alpha and a soft
                // glow pass instead of the old hard color blink.
                const float t = 0.5f + 0.5f * std::sin(float(now) * 2.4f);
                Color tc{uint8_t(90 + (140 - 90) * t), uint8_t(90 + (240 - 90) * t),
                         uint8_t(235 + (255 - 235) * t), 255};
                tc.a = uint8_t(150 + 105 * t);
                drawAtlasFrame(ta.enter, "ENTER IDLE0000", gfx::W / 2, 648, 46,
                               Color{24, 120, 160, 70});
                drawAtlasFrame(ta.enter, "ENTER IDLE0000", gfx::W / 2, 648, 40, tc);
            }
        } else {
            const float blink = leaving_ ? (std::fmod(float(now) * 12.0f, 2.0f) < 1.0f ? 1.0f : 0.2f)
                                         : 0.55f + 0.45f * std::sin(float(now) * 3.5f);
            gfx::text(FontId::Ui, a.touchMode ? "TAP TO BEGIN" : "PRESS ENTER TO BEGIN",
                      gfx::W / 2, 596, 40, fade(WHITE, blink), Align::Center, 2.0f);
        }

        gfx::text(FontId::Ui, "RAID Engine V0.5", 24, gfx::H - 58, 20, fade(Color{120, 220, 250, 255}, 0.85f), Align::Left, 1.0f);
        gfx::text(FontId::Ui, "R-Slice V0.1", 24, gfx::H - 32, 20, fade(kIvory, 0.7f), Align::Left, 1.0f);
        if (!a.mixer.audioOk())
            gfx::text(FontId::Ui, "No audio device found - the game will run silently", gfx::W / 2, 520, 22,
                      Color{255, 160, 140, 255}, Align::Center, 1.5f);

        if (debug_) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "FPS: %.0f | %.0f MB", fps_, platform::processMemoryMB());
            gfx::text(FontId::Ui, buf, 10, 8, 14, Color{255, 255, 255, 110}, Align::Left, 1.0f);
        }
        if (flash_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(WHITE, flash_ * 0.8f));
        gfx::endCanvas();
    }

private:
    bool leaving_ = false, debug_ = false, danceRight_ = true;
    double leaveAt_ = 0, animT_ = 0, logoT0_ = 0;
    long beatSeen_ = -1;
    float flash_ = 0, fps_ = 0;
};

// -------------------------------------------------------------- Main menu --

class MainMenuState : public State {
public:
    void enter() override { anim_ = float(app().mainSel); }
    void update(double dt) override {
        App& a = app();
        int& sel = a.mainSel;
        anim_ = approach(anim_, float(sel), 14.0f, float(dt));
        if (chosen_ >= 0) {
            if (a.now - chosenAt_ > 0.55) {
                switch (chosen_) {
                    case 0: a.change(makeStory()); break;
                    case 1: a.change(makeFreeplay()); break;
                    case 2: a.change(makeOptions()); break;
                    case 3: a.change(makeCredits()); break;
                    default: a.quit = true; break;
                }
            }
            return;
        }
        if (a.input.up()) { sel = (sel + 4) % 5; sfxPlay(Sfx::Scroll); }
        if (a.input.down()) { sel = (sel + 1) % 5; sfxPlay(Sfx::Scroll); }
        if (a.input.accept()) {
            chosen_ = sel;
            chosenAt_ = a.now;
            sfxPlay(sel == 4 ? Sfx::Cancel : Sfx::Confirm);
        }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeTitle()); }
        // 7: the editors (chart, character, stage, events), as Psych's Master Editor Menu.
        if (a.input.pressed(KEY_SEVEN) || a.input.pressed(KEY_KP_7)) {
            sfxPlay(Sfx::Confirm);
            a.change(makeEditorMenu());
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float pulse = menuPulse();
        static const char* items[5] = {"STORY MODE", "FREEPLAY", "OPTIONS", "CREDITS", "QUIT"};
        gfx::beginCanvas();
        drawMenuBackground(now, 0x8C6CFF, pulse);
        CharacterState h;
        h.id = CharId::Herald;
        h.beatAt = lastBeatTime();
        gfx::glow({1080, 500}, 230 + 20 * pulse, fade(kGold, 0.3f));
        drawCharacter(h, {1080, 640}, 0.85f, now, true);
        if (menuLogo().id) drawLogo(180, 26, 240);
        else gfx::text(FontId::Title, "RAID ENGINE", 60, 40, 44, kGold, Align::Left, 2.0f);
        for (int i = 0; i < 5; ++i) {
            const bool on = i == a.mainSel;
            const float y = 165.0f + i * 92.0f - (on ? 6.0f : 0.0f);
            float size = on ? 66.0f + 4.0f * pulse : 50.0f;
            Color c = on ? kGold : fade(kIvory, 0.55f);
            if (chosen_ >= 0 && on && std::fmod(float(now - chosenAt_) * 10.0f, 2.0f) < 1.0f) c = WHITE;
            if (chosen_ >= 0 && !on) c = fade(c, std::max(0.0f, 1.0f - float(now - chosenAt_) * 3.0f));
            if (on) gfx::glowText(FontId::Title, items[i], gfx::W / 2, y, size, c, Color{255, 170, 60, 255});
            else gfx::text(FontId::Title, items[i], gfx::W / 2, y + 6, size, c, Align::Center, 2.0f);
            if (on) {
                // V-Slice selection marker: an orange bar each side
                const float w = gfx::measure(FontId::Title, items[i], size).x * 0.5f + 44 + 6 * pulse;
                DrawRectangleRec({gfx::W / 2 - w - 26, y + size * 0.42f, 14, 14}, vs::kAccent);
                DrawRectangleRec({gfx::W / 2 + w + 12, y + size * 0.42f, 14, 14}, vs::kAccent);
            }
        }
        // H-Slice version stack, bottom left
        gfx::text(FontId::Ui, "RAID Engine v" GH_VERSION, 16, gfx::H - 78, 16, fade(kIvory, 0.7f));
        gfx::text(FontId::Ui, "P-Slice inspired UI  -  raylib", 16, gfx::H - 58, 16, fade(kIvory, 0.7f));
        gfx::text(FontId::Ui, a.library.report(), 16, gfx::H - 38, 16, fade(kIvory, 0.6f));
        hints("UP/DOWN select   ENTER confirm   7 editors   ESC back   F11 fullscreen");
        gfx::endCanvas();
    }

private:
    float anim_ = 0;
    int chosen_ = -1;
    double chosenAt_ = 0;
};

// ------------------------------------------------------------------ Story --

class StoryState : public State {
public:
    void update(double) override {
        App& a = app();
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        int& d = a.storyDiff;
        d = clampv(d, 0, 2);
        if (a.input.left()) { d = (d + 2) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.right()) { d = (d + 1) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); }
        if (a.input.accept() && !go_) {
            go_ = true;
            sfxPlay(Sfx::Confirm);
            PlayRequest r;
            r.songId = "builtin:principality";
            r.diff = diffs[d];
            r.story = true;
            r.storyQueue = {"builtin:godshand", "builtin:demise"};
            a.change(makePlay(r));
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        gfx::beginCanvas();
        drawMenuBackground(now, 0xFF8A5C, menuPulse());
        gfx::gradientRect({0, 70, gfx::W, 330}, Color{10, 6, 20, 220}, Color{40, 18, 40, 220});
        const CharId ids[3] = {CharId::Principality, CharId::Hand, CharId::Demise};
        const double bt = lastBeatTime();
        for (int i = 0; i < 3; ++i) {
            CharacterState c;
            c.id = ids[i];
            c.beatAt = bt;
            const float x = 330.0f + i * 310.0f;
            drawCharacter(c, {x, 370}, i == 2 ? 0.62f : 0.68f, now, false);
            gfx::text(FontId::Ui, charDisplayName(ids[i]), x, 372, 24, charColor(ids[i]), Align::Center, 1.5f);
        }
        gfx::text(FontId::Title, "WEEK I", 60, 14, 40, kGold, Align::Left, 2.0f);
        gfx::text(FontId::Title, "THE ASCENSION", gfx::W - 60, 14, 40, kIvory, Align::Right, 2.0f);
        gfx::text(FontId::Ui, "TRACKS", 120, 440, 28, kGold);
        const char* tracks[3] = {"Principality", "Godshand", "Demise"};
        for (int i = 0; i < 3; ++i) gfx::text(FontId::Ui, tracks[i], 120, 478.0f + i * 40, 34, kIvory, Align::Left, 1.5f);
        const std::string d = diffs[clampv(a.storyDiff, 0, 2)];
        const Color dc = diffColor(d);
        const float bob = std::sin(float(now) * 4.0f) * 4.0f;
        gfx::drawNote(gfx::kLeft, {760 - bob, 500}, 54);
        gfx::drawNote(gfx::kRight, {1160 + bob, 500}, 54);
        gfx::glowText(FontId::Title, toLower(d) == "hard" ? "HARD" : (d == "Easy" ? "EASY" : "NORMAL"), 960, 470, 56, dc, dc);
        const ScoreEntry* best = a.scores.get(Scores::key("week:ascension", d));
        gfx::text(FontId::Ui, best ? ("WEEK SCORE: " + fmtThousands(best->score)) : std::string("WEEK SCORE: -"), 960, 560,
                  28, kIvory, Align::Center, 1.5f);
        hints("LEFT/RIGHT difficulty   ENTER begin the ascension   ESC back");
        gfx::endCanvas();
    }

private:
    bool go_ = false;
};

// --------------------------------------------------------------- Freeplay --

class FreeplayState : public State {
public:
    void enter() override {
        rebuild();
        anim_ = float(app().freeplaySel);
        selChangedAt_ = app().now;
        bg_ = currentColor();
    }
    bool wantsMenuMusic() const override { return !previewPlaying_; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        if (!modsOpen_) t.keys = {{"MODIFIERS", KEY_TAB}, {"MOD PACKS", KEY_M}, {"CHART EDITOR", KEY_SEVEN}, {"RESCAN", KEY_R}};
        if (!modsOpen_ && platform::kAndroid) t.keys.insert(t.keys.begin() + 2, {"IMPORT ZIP", KEY_I});
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        int& sel = a.freeplaySel;
        const int n = int(list_.size());
        anim_ = approach(anim_, float(sel), 12.0f, float(dt));
        const Color target = currentColor();
        bg_ = gfx::mix(bg_, target, saturate(float(dt) * 4.0f));

        if (modsOpen_) {
            Modifiers& m = a.mods;
            if (in.up()) { modSel_ = (modSel_ + kModRows - 1) % kModRows; sfxPlay(Sfx::Scroll); }
            if (in.down()) { modSel_ = (modSel_ + 1) % kModRows; sfxPlay(Sfx::Scroll); }
            const int dir = in.left() ? -1 : (in.right() ? 1 : 0);
            if (dir || in.accept()) {
                sfxPlay(Sfx::Scroll);
                if (modSel_ == 0) m.rate = clampv(std::round((m.rate + 0.05 * (dir ? dir : 1)) * 20.0) / 20.0, 0.5, 2.0);
                if (modSel_ == 1) m.botplay = !m.botplay;
                if (modSel_ == 2) m.practice = !m.practice;
                if (modSel_ == 3) m.mirror = !m.mirror;
                if (modSel_ == 4) {
                    m.scrollType = (m.scrollType + (dir ? dir : 1) + 3) % 3;
                    m.scrollValue = m.scrollType == 2 ? 2.5 : 1.0;
                }
                if (modSel_ == 5 && m.scrollType != 0) {
                    const double step = m.scrollType == 1 ? 0.05 : 0.1;
                    m.scrollValue = std::round((m.scrollValue + step * (dir ? dir : 1)) / step) * step;
                    m.scrollValue = m.scrollType == 1 ? clampv(m.scrollValue, 0.25, 5.0) : clampv(m.scrollValue, 0.5, 10.0);
                }
                if (modSel_ == 6) m.patternLog = !m.patternLog;
            }
            if (in.back() || in.pressed(KEY_TAB)) modsOpen_ = false;
            return;
        }
        if (n > 0 && !previewPlaying_) {  // H-Slice: selection is locked while a preview plays
            if (in.up()) { sel = (sel + n - 1) % n; changed(); }
            if (in.down()) { sel = (sel + 1) % n; changed(); }
            const int nd = int(list_[size_t(sel)]->diffs.size());
            if (nd > 0) {
                if (in.left()) { a.freeplayDiff = (diffIndex() + nd - 1) % nd; sfxPlay(Sfx::Scroll); }
                if (in.right()) { a.freeplayDiff = (diffIndex() + 1) % nd; sfxPlay(Sfx::Scroll); }
            }
        }
        // MusicPlayer transport: SPACE toggles the preview, LEFT/RIGHT seek.
        if (n > 0 && in.pressed(KEY_SPACE)) {
            if (previewPlaying_) {
                if (previewPaused_) { a.mixer.resume(); previewPaused_ = false; }
                else { a.mixer.pause(); previewPaused_ = true; }
            } else if (!job_.running() && a.now - selChangedAt_ > 0.2) {
                const SongEntry* e = list_[size_t(sel)];
                pendingId_ = e->id;
                const SongEntry copy = *e;
                job_.start([copy] { return app().library.preview(copy); });
            }
        }
        if (previewPlaying_ && !previewPaused_) {
            previewCur_ = a.mixer.time(a.now) * 1000.0;
            const bool l = in.isDown(KEY_LEFT), r = in.isDown(KEY_RIGHT);
            if (in.left()) seekPreview(-1000);
            if (in.right()) seekPreview(1000);
            if (l || r) {
                scrubHold_ += dt;
                if (scrubHold_ > 0.5) seekPreview((r ? 1.0 : -1.0) * 40000.0 * dt);
            } else scrubHold_ = 0;
        }
        if (in.pressed(KEY_TAB)) { modsOpen_ = true; sfxPlay(Sfx::Scroll); }
        // Android: mod zips picked in the system file browser are copied to mods/.
        if (platform::kAndroid && in.pressed(KEY_I) && !importing_) {
            importing_ = platform::pickModZips();
            note(importing_ ? "Pick one or more mod .zip files" : "The file picker is not available");
        }
        bool rescan = in.pressed(KEY_R);
        if (importing_) {
            int count = 0;
            const platform::Import st = platform::importStatus(&count);
            if (st == platform::Import::Done || st == platform::Import::Nothing) {
                importing_ = false;
                platform::resetImport();
                if (st == platform::Import::Done) {
                    rescan = true;
                    note(TextFormat("Imported %d mod zip(s)", count));
                } else {
                    note("Nothing imported (mods must be .zip files)");
                }
            } else if (st == platform::Import::Copying) {
                note("Copying...");
            }
        }
        if (rescan) {
            const std::string id = n > 0 ? list_[size_t(sel)]->id : "";
            a.library.scan(a.base, installProgress);
            rebuild();
            for (int i = 0; i < int(list_.size()); ++i)
                if (list_[size_t(i)]->id == id) sel = i;
            sfxPlay(Sfx::Confirm);
        }
        if (in.back()) {
            if (previewPlaying_) { stopPreview(); return; }
            sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); return;
        }
        if (in.pressed(KEY_M)) { sfxPlay(Sfx::Confirm); a.change(makeModPacks()); return; }
        if ((in.pressed(KEY_SEVEN) || in.pressed(KEY_KP_7)) && n > 0) {  // chart editor, as in Psych
            const SongEntry* e = list_[size_t(sel)];
            if (!e->diffs.empty()) {
                sfxPlay(Sfx::Confirm);
                a.change(makeChartEditor(e->id, e->diffs[size_t(diffIndex())]));
                return;
            }
        }
        if (in.accept() && n > 0 && !previewPlaying_) {
            const SongEntry* e = list_[size_t(sel)];
            if (!e->diffs.empty()) {
                sfxPlay(Sfx::Confirm);
                previewPlaying_ = previewPaused_ = false;
                PlayRequest r;
                r.songId = e->id;
                r.diff = e->diffs[size_t(diffIndex())];
                a.change(makePlay(r));
                return;
            }
        }
        // song preview job (started by SPACE)
        if (job_.ready()) {
            PcmPtr pcm = job_.take();
            const SongEntry* e = n > 0 ? list_[size_t(sel)] : nullptr;
            if (pcm && e && pendingId_ == e->id && !previewPlaying_) {
                a.stopMenuMusic();
                a.mixer.setTracks(pcm, nullptr, nullptr);
                const double len = double(pcm->frames()) / Mixer::kRate;
                const double start = e->builtin ? len * 0.38 : len * 0.3;
                a.mixer.setLoop(true, start, std::min(len, start + 20.0));
                a.mixer.setRate(1.0);
                a.mixer.play(start);
                previewPlaying_ = true;
                previewPaused_ = false;
                previewLen_ = len * 1000.0;
                previewCur_ = start * 1000.0;
            }
            previewId_ = pendingId_;
        }
    }

    void draw() override {
        App& a = app();
        const double now = a.now;
        gfx::beginCanvas();
        const unsigned hex = (unsigned(bg_.r) << 16) | (unsigned(bg_.g) << 8) | bg_.b;
        drawMenuBackground(now, hex, previewPlaying_ ? 0.0f : menuPulse());
        const int n = int(list_.size());
        // ---- song carousel: H-Slice's Alphabet titles + HealthIcons --------
        // item.x = 90 + (i - lerp) * 20 ; item.y = 320 + (i - lerp) * 156
        const float drawDist = 4.0f;
        for (int i = std::max(0, int(anim_) - 5); i < std::min(n, int(anim_) + 6); ++i) {
            const float d = float(i) - anim_;
            if (std::fabs(d) > drawDist + 0.6f) continue;
            const float y = 320.0f + d * 156.0f;
            const float x = 110.0f + d * 20.0f;
            const bool on = i == a.freeplaySel;
            const SongEntry* e = list_[size_t(i)];
            const float ts = 58.0f;
            const Color tc = on ? WHITE : fade(kIvory, 0.6f);
            gfx::text(FontId::Title, e->title, x, y, ts, fade(tc, on ? 1.0f : std::max(0.25f, 0.6f - std::fabs(d) * 0.12f)), Align::Left, 2.0f);
            const float tw = gfx::measure(FontId::Title, e->title, ts).x;
            if (on && !e->builtin)
                gfx::text(FontId::Ui, e->subtitle, x + 4, y + 46, 20, fade(kGold, 0.8f), Align::Left, 1.0f);
            // HealthIcon: character icon just right of the title
            const CharId cid = e->builtin ? charFromName(e->opponent) : CharId::Shade;
            drawIcon(cid, gfx::rgb(e->color), {x + tw + 14, y - 18}, on ? 72.0f : 56.0f, false, now);
        }
        // ---- top-right: scoreText/diffText or the MusicPlayer bar -----------
        const float px = gfx::W * 0.7f;  // 896
        if (n > 0) {
            const SongEntry* e = list_[size_t(a.freeplaySel)];
            const std::string d = e->diffs.empty() ? "" : e->diffs[size_t(diffIndex())];
            if (!previewPlaying_) {
                // scoreBG: black bar behind the right-aligned personal best
                DrawRectangleRec({px - 6, 0, gfx::W - px + 6, 72}, Color{0, 0, 0, 153});
                const ScoreEntry* best = d.empty() ? nullptr : a.scores.get(Scores::key(e->id, d));
                const std::string bs = best ? (fmtThousands(best->score) + " (" + TextFormat("%.2f%%", best->acc * 100.0) + ")")
                                            : "-";
                gfx::text(FontId::Ui, "PERSONAL BEST: " + bs, gfx::W - 6, 5, 30, WHITE, Align::Right, 1.5f);
                if (!d.empty()) {
                    std::string du = d;
                    for (char& c : du) c = char(std::toupper((unsigned char)c));
                    gfx::text(FontId::Ui, e->diffs.size() > 1 ? "< " + du + " >" : du, px + 140, 42, 24,
                              diffColor(d), Align::Center, 1.5f);
                }
            } else {
                // playbackBG + songTxt/timeTxt/progressBar (MusicPlayer)
                DrawRectangleRec({px - 6, 0, gfx::W - px + 6, 105}, Color{0, 0, 0, 153});
                gfx::text(FontId::Ui, std::string("PLAYING: ") + e->title + (previewPaused_ ? " (PAUSED)" : ""),
                          gfx::W - 6, 5, 30, WHITE, Align::Right, 1.5f);
                const double len = previewLen_;
                const double cur = previewCur_;
                auto fmtT = [](double ms) {
                    const int s = int(ms / 1000);
                    return TextFormat("%d:%02d", s / 60, s % 60);
                };
                gfx::text(FontId::Ui, std::string(fmtT(cur)) + " / " + fmtT(len), gfx::W - 6, 44, 30, WHITE, Align::Right, 1.5f);
                const float bx = px - 6, bw = gfx::W - px;
                DrawRectangleRec({bx, 82, bw, 10}, Color{0, 0, 0, 255});
                if (len > 0) DrawRectangleRec({bx, 82, bw * float(clampv(cur / len, 0.0, 1.0)), 10}, WHITE);
                gfx::text(FontId::Ui, "<", px + 40, 30, 30, WHITE, Align::Center, 1.5f);
                gfx::text(FontId::Ui, ">", px + 200, 30, 30, WHITE, Align::Center, 1.5f);
            }
        }
        if (a.library.customCount() == 0) {
            const std::string tip = a.touchMode
                ? "Tip: get mods in MOD PACKS, or copy them to " + platform::displayPath(a.base + "mods") + " and tap RESCAN"
                : std::string("Tip: drop FNF / Psych Engine songs into the 'songs' or 'mods' folder, then press R");
            gfx::text(FontId::Ui, tip, 40, 596, 18, fade(kIvory, 0.65f));
        }
        if (!note_.empty() && a.now - noteAt_ < 4.0)
            gfx::text(FontId::Ui, note_, 40, 556, 22, fade(kGold, float(std::min(1.0, 4.0 - (a.now - noteAt_)))), Align::Left, 1.5f);
        if (modsOpen_) drawMods();
        // bottomBG: 26px black bar + the controls tip (H-Slice's bottomText)
        DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
        gfx::text(FontId::Ui, a.touchMode
                      ? "Tap a song   difficulty arrows   MODIFIERS   MOD PACKS   RESCAN   BACK"
                      : "SPACE listen   ENTER play   LEFT/RIGHT difficulty (seek while listening)   TAB modifiers   7 chart editor   M mod packs   R rescan   ESC back",
                  gfx::W / 2, gfx::H - 33, 18, Color{220, 214, 235, 255}, Align::Center);
        gfx::endCanvas();
    }

private:
    void rebuild() {
        list_.clear();
        for (const SongEntry& e : app().library.songs()) list_.push_back(&e);
        app().freeplaySel = clampv(app().freeplaySel, 0, std::max(0, int(list_.size()) - 1));
    }
    void changed() {
        sfxPlay(Sfx::Scroll);
        selChangedAt_ = app().now;
    }
    void note(const std::string& s) {
        note_ = s;
        noteAt_ = app().now;
    }
    // Unpacking big zips takes a while: keep the screen alive.
    static void installProgress(const std::string& zip, double f) {
        BeginDrawing();
        ClearBackground(Color{12, 8, 20, 255});
        const char* t1 = TextFormat("Installing %s", zip.c_str());
        const int w = GetScreenWidth(), h = GetScreenHeight();
        DrawText(t1, (w - MeasureText(t1, 24)) / 2, h / 2 - 30, 24, RAYWHITE);
        DrawRectangle(w / 4, h / 2 + 10, w / 2, 10, Color{60, 50, 80, 255});
        DrawRectangle(w / 4, h / 2 + 10, int(w / 2 * f), 10, Color{200, 160, 255, 255});
        EndDrawing();
        PollInputEvents();
        SwapScreenBuffer();
    }
    int diffIndex() const {
        if (list_.empty()) return 0;
        const int nd = int(list_[size_t(app().freeplaySel)]->diffs.size());
        return nd > 0 ? clampv(app().freeplayDiff, 0, nd - 1) : 0;
    }
    Color currentColor() const {
        if (list_.empty()) return kGold;
        return gfx::rgb(list_[size_t(app().freeplaySel)]->color);
    }
    void seekPreview(double ms) {
        previewCur_ = clampv(previewCur_ + ms, 0.0, previewLen_ > 0 ? previewLen_ : 1e18);
        app().mixer.seek(previewCur_ / 1000.0);
    }
    void stopPreview() {
        App& a = app();
        a.mixer.stop();
        previewPlaying_ = previewPaused_ = false;
        previewCur_ = 0;
        a.startMenuMusic();
        sfxPlay(Sfx::Cancel);
    }
    void drawMods() {
        const Modifiers& m = app().mods;
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 150});
        DrawRectangleRounded({370, 140, 540, 430}, 0.08f, 12, Color{16, 10, 30, 245});
        gfx::text(FontId::Title, "MODIFIERS", 640, 158, 38, kGold, Align::Center, 2.0f);
        static const char* scrollTypes[3] = {"the chart's", "multiplied", "constant"};
        const std::string rows[kModRows] = {
            TextFormat("Playback rate   %.2fx", m.rate), std::string("Botplay   ") + (m.botplay ? "ON" : "OFF"),
            std::string("Practice (no death)   ") + (m.practice ? "ON" : "OFF"),
            std::string("Play as the opponent   ") + (m.mirror ? "ON" : "OFF"), std::string("Scroll speed   ") + scrollTypes[m.scrollType],
            m.scrollType == 0 ? std::string("Scroll value   -")
                              : std::string(TextFormat(m.scrollType == 1 ? "Scroll value   x%.2f" : "Scroll value   %.1f", m.scrollValue)),
            std::string("Pattern log (.raid)   ") + (m.patternLog ? "ON" : "OFF")};
        for (int i = 0; i < kModRows; ++i)
            gfx::text(FontId::Ui, rows[i], 640, 210.0f + i * 46, 30, i == modSel_ ? kGold : (i == 5 && m.scrollType == 0 ? fade(kIvory, 0.4f) : kIvory),
                      Align::Center, 1.5f);
        gfx::text(FontId::Ui, "Scores are only saved at 1.00x or faster, without botplay or practice", 640, 532, 18,
                  fade(kIvory, 0.6f), Align::Center);
    }

    std::vector<const SongEntry*> list_;
    float anim_ = 0;
    Color bg_ = kGold;
    double selChangedAt_ = 0;
    PcmJob job_;
    std::string pendingId_, previewId_;
    bool previewPlaying_ = false, previewPaused_ = false;
    double previewLen_ = 0, previewCur_ = 0, scrubHold_ = 0;
    bool modsOpen_ = false;
    static constexpr int kModRows = 7;
    int modSel_ = 0;
    bool importing_ = false;
    std::string note_;
    double noteAt_ = 0;
};

// ---------------------------------------------------------------- Credits --

class CreditsState : public State {
public:
    void enter() override { t0_ = app().now; }
    void update(double) override {
        if (app().input.back() || app().input.accept()) {
            sfxPlay(Sfx::Cancel);
            app().change(makeMainMenu());
        }
    }
    void draw() override {
        static const char* lines[] = {
            "#RAID ENGINE",
            "the .RAID chart engine",
            "",
            "@A RHYTHM DUEL",
            "Created for deatuamae67-crypto",
            "",
            "@ENGINE, MUSIC, ART AND CHARTS",
            "Written from scratch in C++20",
            "Every song is synthesized and every character is drawn in code",
            "Built with Claude Code",
            "",
            "@LIBRARIES",
            "raylib 5.5 by Ramon Santamaria (zlib license)",
            "",
            "@FONTS",
            "Cinzel Decorative by Natanael Gama (SIL Open Font License)",
            "Rajdhani by Indian Type Foundry (SIL Open Font License)",
            "",
            "@THE CAST",
            "The Herald - a mortal voice",
            "The Principality - keeper of the first gate",
            "The Hand - author of the world",
            "Demise - the end of all principalities",
            "",
            "Thank you for playing.",
        };
        const double now = app().now;
        gfx::beginCanvas();
        drawMenuBackground(now, 0x6C8CFF, menuPulse());
        float y = 640 - float(now - t0_) * 42.0f;
        const float total = float(sizeof(lines) / sizeof(lines[0])) * 46 + 700;
        while (y < -total + 640) y += total;
        for (const char* l : lines) {
            if (l[0] == '#') gfx::glowText(FontId::Title, l + 1, gfx::W / 2, y, 64, kGold, Color{255, 180, 80, 255});
            else if (l[0] == '@') gfx::text(FontId::Title, l + 1, gfx::W / 2, y + 10, 30, kGold, Align::Center, 2.0f);
            else gfx::text(FontId::Ui, l, gfx::W / 2, y + 10, 28, kIvory, Align::Center, 1.5f);
            y += l[0] == '#' ? 80 : 46;
        }
        hints("ESC back");
        gfx::endCanvas();
    }

private:
    double t0_ = 0;
};

std::unique_ptr<State> makeStartup() { return std::make_unique<StartupState>(); }
std::unique_ptr<State> makeTitle() { return std::make_unique<TitleState>(); }
std::unique_ptr<State> makeMainMenu() { return std::make_unique<MainMenuState>(); }
std::unique_ptr<State> makeStory() { return std::make_unique<StoryState>(); }
std::unique_ptr<State> makeFreeplay() { return std::make_unique<FreeplayState>(); }
std::unique_ptr<State> makeCredits() { return std::make_unique<CreditsState>(); }

}  // namespace gh
