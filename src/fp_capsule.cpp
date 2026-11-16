// V-Slice freeplay song capsule — see fp_capsule.hpp and
// docs/VSLICE_FREEPLAY_LAYOUT.md for the layout this ports.
#include "fp_capsule.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "gfx.hpp"
#include "song_scan.hpp"
#include "title_atlas.hpp"

namespace gh {
namespace {

constexpr float kScale = 0.8f;        // CustomAnimControl::realScaled
constexpr float kCapsuleW = 612.0f;   // frameWidth in freeplayCapsule.xml
constexpr float kCapsuleH = 132.0f;   // frameHeight
constexpr float kSpineY = 45.0f;      // songText origin (design space)

TitleAtlas g_capsule, g_bigNums, g_smallNums, g_rank, g_icons[32], g_capsuleText;
Texture2D g_bpmText{}, g_diffText{};
std::string g_iconNames[32];
int g_iconCount = 0;
bool g_ready = false;

const char* kDigitName(int d) {
    static const char* n[10] = {"ZERO", "ONE", "TWO", "THREE", "FOUR",
                                "FIVE", "SIX", "SEVEN", "EIGHT", "NINE"};
    return n[d >= 0 && d <= 9 ? d : 0];
}

// "mp3 capsule w backing" + 4-digit frame, selected vs not.
std::string capsuleFrame(bool selected, int f) {
    return frameName(selected ? "mp3 capsule w backing" : "mp3 capsule w backing NOT SELECTED", f % 8);
}

// rank badge anim prefix per our S+/S/A/B/C/D ranks.
const char* rankBadge(const char* rank) {
    if (!rank || !*rank) return "";
    const std::string r = rank;
    if (r == "S+" || r == "S") return "PERFECT";
    if (r == "A") return "EXCELLENT";
    if (r == "B") return "GREAT";
    if (r == "C") return "GOOD";
    return "LOSS";
}

const TitleAtlas* iconFor(const SongEntry* e) {
    if (!e) return nullptr;
    std::string want;
    if (e->builtin && !e->opponent.empty()) {
        want = e->opponent;
        for (char& c : want) c = char(std::tolower((unsigned char)c));
    } else {
        want = "bf";
    }
    for (int i = 0; i < g_iconCount; ++i)
        if (g_iconNames[i] == want) return &g_icons[i];
    for (int i = 0; i < g_iconCount; ++i)
        if (g_iconNames[i] == "face") return &g_icons[i];
    return nullptr;
}

// Draws an atlas frame (or anim frame) scaled into a dest rect.
void drawFrame(const TitleAtlas& a, const std::string& name, Rectangle dest,
               Color tint = WHITE) {
    const AtlasFrame* f = a.find(name);
    if (!f || f->src.width <= 0) return;
    // trimmed sprite: content sits at (offsetX, offsetY) inside the logical
    // frame, drawn 1:1 — dest is the logical-frame rect we computed already,
    // so map src into the sub-rect it occupies.
    const Rectangle d{dest.x + f->offsetX * (dest.width / f->w),
                      dest.y + f->offsetY * (dest.height / f->h),
                      f->src.width * (dest.width / f->w),
                      f->src.height * (dest.height / f->h)};
    DrawTexturePro(a.tex, f->src, d, {0, 0}, 0, tint);
}

// First anim frame of `prefix` whose name starts with it ("idle0001" etc.).
std::string firstFrame(const TitleAtlas& a, const std::string& prefix) {
    for (const auto& kv : a.frames)
        if (kv.first.rfind(prefix, 0) == 0) return kv.first;
    return "";
}

// Small sprite-digit strip (bignumbers/smallnumbers are single frames).
void drawDigit(const TitleAtlas& a, int d, float x, float y, float h, Color c) {
    const AtlasFrame* f = a.find(std::string(kDigitName(d)) + "0000");
    if (!f) return;
    const float w = f->src.width * (h / f->src.height);
    drawFrame(a, std::string(kDigitName(d)) + "0000", {x, y, w, h}, c);
}

// ---- AtlasText port (mikolka.funkin.AtlasText) --------------------------
// Bitmap font atlas: one frame per glyph, bottom-aligned to maxHeight,
// advance = frame width, space = 5 design px (CAPSULE_TEXT.spaceWidth).
std::string atlasCharPrefix(char c) {
    // capsule-text.xml glyph names: literals are "X0000"; punctuation uses
    // dash-names. '-' itself is "-0000".
    switch (c) {
        case 39: return "-apostrophe-";
        case '!': return "-exclamation point-";
        case '?': return "-question mark-";
        case '.': return "-period-";
        case '"': return "-quote-";
        default: return std::string(1, c);
    }
}

const AtlasFrame* atlasCharFrame(const TitleAtlas& a, const std::string& prefix) {
    // frames are named "<prefix>0000"; fall back to prefix scan
    const AtlasFrame* f = a.find(prefix + "0000");
    if (f) return f;
    for (const auto& kv : a.frames)
        if (kv.first.rfind(prefix, 0) == 0) return &kv.second;
    return nullptr;
}

float atlasMaxHeight(const TitleAtlas& a) {
    float m = 0;
    for (const auto& kv : a.frames) m = std::max(m, kv.second.h > 0 ? kv.second.h : kv.second.src.height);
    return m;
}

// Draws atlas-font text with its top-left at screen point (ox,oy);
// `scale` is the group scale (realScaled). Design-space advances are
// multiplied by scale internally. Uppercases input (capsule-text is
// upper-only). Returns advance width in design px.
float drawAtlasText(const TitleAtlas& a, const std::string& textIn, float ox, float oy,
                    float scale, Color tint) {
    if (!a.tex.id || textIn.empty()) return 0;
    std::string text = textIn;
    for (char& c : text) c = char(std::toupper((unsigned char)c));
    const float maxH = atlasMaxHeight(a);
    float cx = 0.0f;
    for (char c : text) {
        if (c == ' ') { cx += 5.0f; continue; }
        if (c == 10) continue;  // single-line capsule tags
        const std::string p = atlasCharPrefix(c);
        const AtlasFrame* f = atlasCharFrame(a, p);
        if (!f) { cx += 5.0f; continue; }
        const float fw = f->w > 0 ? f->w : f->src.width;
        const float fh = f->h > 0 ? f->h : f->src.height;
        // bottom-align within maxHeight (AtlasText.hx appendTextCased)
        const float dx = ox + cx * scale;
        const float dy = oy + (maxH - fh) * scale;
        const float dw = fw * scale, dh = fh * scale;
        // trimmed sprite: content at offset within logical frame
        const float rx = dw / std::max(1.0f, fw), ry = dh / std::max(1.0f, fh);
        DrawTexturePro(a.tex, f->src,
                       {dx + f->offsetX * rx, dy + f->offsetY * ry,
                        f->src.width * rx, f->src.height * ry},
                       {0, 0}, 0, tint);
        cx += fw;
    }
    return cx;
}

}  // namespace

void fpCapsuleInit(const std::string& base) {
    if (g_ready) return;
    const std::string d = base + "assets/freeplay/";
    std::string err;
    g_ready = loadAtlasXml(d + "freeplayCapsule/capsule/freeplayCapsule.png",
                           d + "freeplayCapsule/capsule/freeplayCapsule.xml", g_capsule, &err) &&
              loadAtlasXml(d + "freeplayCapsule/bignumbers.png",
                           d + "freeplayCapsule/bignumbers.xml", g_bigNums) &&
              loadAtlasXml(d + "freeplayCapsule/smallnumbers.png",
                           d + "freeplayCapsule/smallnumbers.xml", g_smallNums) &&
              loadAtlasXml(d + "rankbadges.png", d + "rankbadges.xml", g_rank) &&
              loadAtlasXml(d + "fonts/capsule-text.png", d + "fonts/capsule-text.xml", g_capsuleText);
    if (!g_ready) return;
    {
        const std::string bp = d + "freeplayCapsule/bpmtext.png";
        const std::string dp = d + "freeplayCapsule/difficultytext.png";
        if (FileExists(bp.c_str())) { g_bpmText = LoadTexture(bp.c_str()); GenTextureMipmaps(&g_bpmText); SetTextureFilter(g_bpmText, TEXTURE_FILTER_TRILINEAR); }
        if (FileExists(dp.c_str())) { g_diffText = LoadTexture(dp.c_str()); GenTextureMipmaps(&g_diffText); SetTextureFilter(g_diffText, TEXTURE_FILTER_TRILINEAR); }
    }

    // Character pixel icons: every icons/*pixel.png+.xml pair.
    // file stems in icons/ (e.g. "bfpixel", "senpai-pixelpixel")
    static const char* stems[] = {"bfpixel", "dadpixel", "gfpixel", "picopixel",
                                  "mompixel", "monsterpixel", "parentspixel",
                                  "senpai-pixelpixel", "spirit-pixelpixel", "facepixel",
                                  "spookypixel", "tankmanpixel", "darnellpixel"};
    static const char* keys[] = {"bf", "dad", "gf", "pico", "mom", "monster",
                                 "parents", "senpai", "spirit", "face",
                                 "spooky", "tankman", "darnell"};
    for (size_t i = 0; i < sizeof(stems) / sizeof(stems[0]); ++i) {
        const std::string p = d + "icons/" + stems[i];
        if (!FileExists((p + ".png").c_str())) continue;
        TitleAtlas a;
        if (!loadAtlasXml(p + ".png", p + ".xml", a)) continue;
        if (g_iconCount < 32) {
            g_icons[g_iconCount] = std::move(a);
            g_iconNames[g_iconCount] = keys[i];
            ++g_iconCount;
        }
    }
}

void fpCapsuleShutdown() {
    auto free = [](TitleAtlas& a) { if (a.tex.id) UnloadTexture(a.tex); };
    free(g_capsule); free(g_bigNums); free(g_smallNums); free(g_rank); free(g_capsuleText);
    if (g_bpmText.id) UnloadTexture(g_bpmText);
    if (g_diffText.id) UnloadTexture(g_diffText);
    for (auto& a : g_icons) free(a);
    g_iconCount = 0;
    g_ready = false;
}

bool fpCapsuleReady() { return g_ready; }

void fpDrawCapsule(const SongEntry* e, const char* diff, const char* scoreRank,
                   int bpm, const char* week, float x, float y, bool selected,
                   double now, int animIndex, float squashX, float squashY) {
    if (!g_ready) return;
    (void)diff; (void)animIndex;

    // Whole capsule in 612x132 design space; squashX/Y are the final sprite
    // scale (0.8 rest, animated during jump-in).
    const float k = kScale;
    // V-Slice loops the 8-frame idle bounce; frozen here per user preference.
    const int frame = 0;
    // updateSelected(): capsule.offset.x = selected ? 0 : -5 (design px)
    const float capX = x - (selected ? 0.0f : 4.0f * (squashX / 0.8f));
    drawFrame(g_capsule, capsuleFrame(selected, frame),
              {capX, y, kCapsuleW * squashX, kCapsuleH * squashY},
              selected ? WHITE : gfx::fade(WHITE, 0.85f));

    // design-space -> screen offset helper
    const auto P = [&](float dx, float dy) { return Vector2{x + dx * k, y + dy * k}; };

    if (!e) {
        gfx::text(gfx::FontId::Pixel, "Random!", P(159, 45).x, P(159, 45).y, 32.0f * k,
                  selected ? WHITE : gfx::fade(WHITE, 0.6f), gfx::Align::Left);
        return;
    }

    // ---- title (CapsuleText): clip + ghost + marquee ---------------------
    // V-Slice: pos (capsule.width*0.26, 45), font 5by7 @ 40*realScaled.
    const float clipW = 255.0f * k;  // CapsuleText.clipWidth default
    const std::string title = e->title;
    const Vector2 ts = P(159, 45);
    const float textSize = 32.0f * k;
    const Color tc = selected ? WHITE : gfx::fade(WHITE, 0.6f);

    const Vector2 tm = gfx::measure(gfx::FontId::Pixel, title, textSize);
    float clipX = 0.0f;
    if (tm.x > clipW) {
        // CapsuleText rhythm: 0.6 idle, 2s right, 0.3 hold, 2s left, 0.3 hold.
        const float travel = tm.x - clipW;
        const float t = float(std::fmod(now, 5.2));
        auto sineInOut = [](float u) { return 0.5f - 0.5f * std::cos(3.14159265f * u); };
        if (t < 0.6f) clipX = 0.0f;
        else if (t < 2.6f) clipX = -travel * sineInOut((t - 0.6f) / 2.0f);
        else if (t < 2.9f) clipX = -travel;
        else if (t < 4.9f) clipX = -travel + travel * sineInOut((t - 2.9f) / 2.0f);
        else clipX = 0.0f;
    }
    // scissor needs framebuffer coords, not canvas coords; full glyph height
    const Rectangle clip{ts.x - 2, ts.y - textSize * 0.6f, clipW + 4, textSize * 2.4f};
    const Vector2 so = gfx::canvasOrigin();
    const float ss = gfx::canvasScale();
    BeginScissorMode(int(so.x + clip.x * ss), int(so.y + clip.y * ss),
                     int(clip.width * ss), int(clip.height * ss));
    const float txp = ts.x + clipX;
    if (selected) {
        // CapsuleText glow: blurredText glowColor ~0xFF00ccff behind whiteText,
        // approximated with additive offset passes (GlowFilter blur 5,5).
        static const Color kGlow{0, 204, 255, 255};
        BeginBlendMode(BLEND_ADDITIVE);
        for (int i = 0; i < 8; ++i) {
            const float ang = float(i) * 0.7853982f;
            const float r = textSize * 0.18f;
            gfx::text(gfx::FontId::Pixel, title, txp + std::cos(ang) * r, ts.y + std::sin(ang) * r,
                      textSize, gfx::fade(kGlow, 0.14f), gfx::Align::Left);
        }
        EndBlendMode();
        gfx::text(gfx::FontId::Pixel, title, txp, ts.y, textSize, tc, gfx::Align::Left,
                  textSize * 0.04f, gfx::fade(kGlow, 0.8f));
    } else {
        gfx::text(gfx::FontId::Pixel, title, txp, ts.y, textSize, tc, gfx::Align::Left);
    }
    EndScissorMode();

    // ---- pixel character icon (PixelatedIcon 60,14, scale 2, origin.x 25) --
    if (const TitleAtlas* ic = iconFor(e)) {
        const std::string idle = firstFrame(*ic, "idle");
        const AtlasFrame* f = idle.empty() ? nullptr : ic->find(idle);
        if (f) {
            // logical frame is 50x50, sprite scale 2 -> 100 design px,
            // origin.x=25 shifts left by 25*2=50 design px.
            const float dx = 60.0f - 50.0f, dy = 14.0f;
            drawFrame(*ic, idle, {x + dx * k, y + dy * k, 100.0f * k, 100.0f * k},
                      selected ? WHITE : gfx::fade(WHITE, 0.6f));
        }
    }

    // ---- rank badge (FreeplayRank 420,41, scale 0.9, additive) ------------
    const char* badge = rankBadge(scoreRank);
    if (badge && *badge) {
        std::string prefix = std::string(badge) + " rank";
        std::vector<std::string> frames;
        for (const auto& kv : g_rank.frames)
            if (kv.first.rfind(prefix, 0) == 0) frames.push_back(kv.first);
        std::sort(frames.begin(), frames.end());
        if (!frames.empty()) {
            const std::string fn = frames[size_t(int(now * 12.0)) % frames.size()];
            const AtlasFrame* f = g_rank.find(fn);
            if (f && f->w > 0) {
                const float bw = f->w * 0.9f * k, bh = f->h * 0.9f * k;
                BeginBlendMode(BLEND_ADDITIVE);
                drawFrame(g_rank, fn, {P(420, 41).x, P(420, 41).y, bw, bh},
                          selected ? WHITE : gfx::fade(Color{170, 170, 170, 255}, 0.7f));
                EndBlendMode();
            }
        }
    }

    // ---- BPM readout: bpmText sprite (144,87) + 3 small digits -------------
    if (bpm > 0) {
        if (g_bpmText.id) {
            // 49x28 -> setGraphicSize(w*0.9)=44 design -> *0.8 screen
            DrawTexturePro(g_bpmText, {0, 0, (float)g_bpmText.width, (float)g_bpmText.height},
                           {P(144, 87).x, P(144, 87).y, 44.0f * k, 44.0f * k * 28.0f / 49.0f},
                           {0, 0}, 0, gfx::fade(WHITE, selected ? 1.0f : 0.7f));
        } else {
            gfx::text(gfx::FontId::Pixel, "BPM", P(144, 87).x, P(144, 87).y, 20.0f * k,
                      gfx::fade(WHITE, 0.7f), gfx::Align::Left);
        }
        // CapsuleNumber: sprite CENTERED at (shiftX + i*11, 88.5); frame
        // 18x28 @ setGraphicSize(0.9) -> 16.2x25.2 design px, offset tweaks
        // for narrow digits (1: -4, 3: -1).
        const int shiftX = bpm >= 100 ? 186 : 191;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%03d", bpm > 999 ? 999 : bpm);
        for (int i = 0; i < 3; ++i) {
            const int d = buf[i] - '0';
            float ox = 0.0f;
            if (d == 1) ox = -4.0f; else if (d == 3) ox = -1.0f;
            const float ddx = float(shiftX + i * 11) + ox;
            drawFrame(g_smallNums, std::string(kDigitName(d)) + "0000",
                      {x + (ddx - 8.1f) * k, y + (88.5f - 12.6f) * k,
                       16.2f * k, 25.2f * k},
                      gfx::fade(WHITE, selected ? 0.95f : 0.6f));
        }
    }

    // ---- difficulty label sprite (414,87) ---------------------------------
    if (g_diffText.id && diff && *diff) {
        // 114x28 -> *0.9 design -> *0.8 screen; draw the word, not a rating
        DrawTexturePro(g_diffText, {0, 0, (float)g_diffText.width, (float)g_diffText.height},
                       {P(414, 87).x, P(414, 87).y, 114.0f * 0.72f, 28.0f * 0.72f},
                       {0, 0}, 0, gfx::fade(WHITE, selected ? 0.9f : 0.55f));
    }

    // ---- week/album tag ----------------------------------------------------
    if (week && *week) {
        const Color wc = gfx::fade(Color{220, 228, 245, 255}, selected ? 0.9f : 0.5f);
        drawAtlasText(g_capsuleText, week, P(298, 91).x, P(298, 91).y, k,
                      wc);
    }
}

}  // namespace gh
