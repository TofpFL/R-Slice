#include "gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common.hpp"

namespace gh::gfx {

namespace {
Font g_title{}, g_ui{}, g_vcr{}, g_pixel{};
bool g_titleOwned = false, g_uiOwned = false, g_vcrOwned = false, g_pixelOwned = false;
Texture2D g_glow{};
Texture2D g_note[5]{};
Texture2D g_receptor[2]{};  // arrow, diamond
Texture2D g_receptorDir[4]{};  // HRK arrow per direction (white, tinted by caller)
Texture2D g_holdPiece[5]{}, g_holdEnd[5]{};  // HRK sustain sprites, baked per dir
bool g_hrk = false;

Color lerpColor(Color a, Color b, float t) {
    t = saturate(t);
    return Color{(unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                 (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t)};
}

float segDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0.0f;
    t = saturate(t);
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

// Distance from (u,v) to the centre line of a note glyph (unit square [-1,1]).
float glyphDist(int shape, float u, float v) {
    if (shape == 0) {  // arrow pointing left
        float d = segDist(u, v, -0.50f, 0.0f, 0.60f, 0.0f);
        d = std::min(d, segDist(u, v, -0.60f, 0.0f, -0.02f, -0.58f));
        d = std::min(d, segDist(u, v, -0.60f, 0.0f, -0.02f, 0.58f));
        return d;
    }
    // diamond with a centre gem
    const float r = 0.6f;
    float d = segDist(u, v, 0, -r, r, 0);
    d = std::min(d, segDist(u, v, r, 0, 0, r));
    d = std::min(d, segDist(u, v, 0, r, -r, 0));
    d = std::min(d, segDist(u, v, -r, 0, 0, -r));
    d = std::min(d, std::sqrt(u * u + v * v));
    return d;
}

Texture2D makeGlyph(int shape, Color base, bool receptor, int size) {
    Image img = GenImageColor(size, size, BLANK);
    Color* px = static_cast<Color*>(img.data);
    const float pw = 2.0f / float(size);
    const float r = 0.16f, o = 0.075f;
    const Color outline = receptor ? Color{205, 210, 230, 255} : Color{22, 12, 38, 255};
    const Color dark = receptor ? Color{40, 38, 56, 255} : lerpColor(base, BLACK, 0.45f);
    const Color light = receptor ? Color{118, 116, 140, 255} : lerpColor(base, WHITE, 0.55f);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) * pw - 1.0f, v = (y + 0.5f) * pw - 1.0f;
            const float d = glyphDist(shape, u, v);
            const float aOuter = saturate((r + o - d) / pw + 0.5f);
            const float aInner = saturate((r - d) / pw + 0.5f);
            const float k = saturate(1.0f - d / r);
            Color fill = lerpColor(dark, light, std::pow(k, 0.8f));
            fill = lerpColor(fill, WHITE, (receptor ? 0.05f : 0.18f) * saturate(-v));
            Color c = lerpColor(outline, fill, aInner);
            c.a = (unsigned char)(aOuter * (receptor ? 235.0f : 255.0f));
            px[y * size + x] = c;
        }
    }
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

Font loadFont(const std::string& path, int size, bool& owned) {
    owned = false;
    if (!FileExists(path.c_str())) return GetFontDefault();
    std::vector<int> cps;
    for (int c = 32; c < 256; ++c) cps.push_back(c);
    cps.push_back(0x2014);  // em dash
    cps.push_back(0x2026);  // ellipsis
    Font f = LoadFontEx(path.c_str(), size, cps.data(), int(cps.size()));
    if (!IsFontValid(f) || f.texture.id == 0) return GetFontDefault();
    GenTextureMipmaps(&f.texture);
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
    owned = true;
    return f;
}


// ------------------------------------------------------------ HRK notes --
// H-Slice's NOTE_assets-hrk atlas: note/hold sprites are RGB masks (pure
// red/green/blue texels each select one of three palette colours, like its
// RGBPalette shader) and the receptor arrows are plain grayscale. We bake
// the masks into ordinary RGBA textures tinted with this game's lane colours
// so no shader is needed. Missing files fall back to the procedural glyphs.

struct HrkFrame { std::string name; Rectangle rect; };
std::vector<HrkFrame> g_hrkFrames;
Image g_hrkImg{};

const HrkFrame* hrkFrame(const char* name) {
    for (const HrkFrame& f : g_hrkFrames)
        if (f.name == name) return &f;
    return nullptr;
}

Texture2D bakeHrk(const HrkFrame& f, const Color pal[3], bool white) {
    const int w = int(f.rect.width), h = int(f.rect.height);
    Image out = GenImageColor(w, h, BLANK);
    unsigned char* d = static_cast<unsigned char*>(out.data);
    const unsigned char* s = static_cast<const unsigned char*>(g_hrkImg.data);
    const int iw = g_hrkImg.width;
    const int x0 = int(f.rect.x), y0 = int(f.rect.y);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned char* sp = s + ((y0 + y) * iw + (x0 + x)) * 4;
            unsigned char* dp = d + (y * w + x) * 4;
            dp[3] = sp[3];
            if (white) {
                dp[0] = dp[1] = dp[2] = 255;
            } else {
                // out.rgb = tex.r*pal0 + tex.g*pal1 + tex.b*pal2 (per channel)
                const float ch[3] = {sp[0] / 255.0f, sp[1] / 255.0f, sp[2] / 255.0f};
                const unsigned char pc[3][3] = {{pal[0].r, pal[0].g, pal[0].b},
                                                {pal[1].r, pal[1].g, pal[1].b},
                                                {pal[2].r, pal[2].g, pal[2].b}};
                for (int c = 0; c < 3; ++c) {
                    const float v = ch[0] * pc[0][c] / 255.0f + ch[1] * pc[1][c] / 255.0f +
                                    ch[2] * pc[2][c] / 255.0f;
                    dp[c] = (unsigned char)(std::min(1.0f, v) * 255.0f + 0.5f);
                }
            }
        }
    }
    Texture2D t = LoadTextureFromImage(out);
    UnloadImage(out);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

void loadHrk(const std::string& baseDir) {
    const char* rel = "assets/notes/NOTE_assets-hrk";
    const std::string relPng = std::string(rel) + ".png";
    std::string png = baseDir + relPng;
    if (!FileExists(png.c_str())) png = relPng;
    if (!FileExists(png.c_str())) return;
    Image img = LoadImage(png.c_str());
    if (img.data == nullptr || img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        if (img.data) UnloadImage(img);
        return;
    }
    const std::string relXml = std::string(rel) + ".xml";
    std::string xmlPath = baseDir + relXml;
    if (!FileExists(xmlPath.c_str())) xmlPath = relXml;
    char* xml = FileExists(xmlPath.c_str()) ? LoadFileText(xmlPath.c_str()) : nullptr;
    g_hrkFrames.clear();
    if (xml) {
        for (const char* p = std::strstr(xml, "<SubTexture"); p; p = std::strstr(p, "<SubTexture")) {
            const char* tagEnd = std::strstr(p, "/>");
            if (!tagEnd) break;
            const char* np = std::strstr(p, "name=\"");
            if (!np || np > tagEnd) { p = tagEnd + 2; continue; }
            np += 6;
            const char* ne = std::strchr(np, '"');
            if (!ne || ne > tagEnd) { p = tagEnd + 2; continue; }
            HrkFrame f;
            f.name.assign(np, ne - np);
            auto attr = [&](const char* key) -> int {
                const char* a = std::strstr(p, key);
                if (!a || a > tagEnd) return 0;
                return std::atoi(a + std::strlen(key));
            };
            f.rect = {(float)attr(" x=\""), (float)attr(" y=\""),
                      (float)attr(" width=\""), (float)attr(" height=\"")};
            if (f.rect.width > 0 && f.rect.height > 0) g_hrkFrames.push_back(f);
            p = tagEnd + 2;
        }
        UnloadFileText(xml);
    }
    if (g_hrkFrames.empty()) { UnloadImage(img); return; }
    g_hrkImg = img;

    // palettes: H-Slice's arrowRGB triples (main, white highlight, dark shade)
    // in RGB-mask order; kCenter reuses the left/purple set.
    static const char* noteFrame[5] = {"purple0000", "blue0000", "green0000", "red0000", "purple0000"};
    static const char* arrowFrame[4] = {"arrowLEFT0000", "arrowDOWN0000", "arrowUP0000", "arrowRIGHT0000"};
    const Color hrkPal[5][3] = {
        {rgb(0xC24B99), WHITE, rgb(0x3C1F56)},  // kLeft   (H-Slice purple)
        {rgb(0x00FFFF), WHITE, rgb(0x1542B7)},  // kDown   (cyan)
        {rgb(0x12FA05), WHITE, rgb(0x0A4447)},  // kUp     (green)
        {rgb(0xF9393F), WHITE, rgb(0x651038)},  // kRight  (red)
        {rgb(0xC24B99), WHITE, rgb(0x3C1F56)},  // kCenter
    };
    for (int d = 0; d < 5; ++d) {
        const Color* pal = hrkPal[d];
        if (const HrkFrame* f = hrkFrame(noteFrame[d])) {
            if (g_note[d].id) UnloadTexture(g_note[d]);
            g_note[d] = bakeHrk(*f, pal, false);
        }
        if (const HrkFrame* f = hrkFrame("red hold piece0000")) {
            if (g_holdPiece[d].id) UnloadTexture(g_holdPiece[d]);
            g_holdPiece[d] = bakeHrk(*f, pal, false);
        }
        if (const HrkFrame* f = hrkFrame("red hold end0000")) {
            if (g_holdEnd[d].id) UnloadTexture(g_holdEnd[d]);
            g_holdEnd[d] = bakeHrk(*f, pal, false);
        }
    }
    for (int d = 0; d < 4; ++d) {
        if (const HrkFrame* f = hrkFrame(arrowFrame[d])) g_receptorDir[d] = bakeHrk(*f, hrkPal[d], false);
    }
    UnloadImage(g_hrkImg);
    g_hrkImg = {};
    g_hrk = true;
}

}  // namespace

const Font& fontOf(FontId f) {
    if (f == FontId::Title) return g_title;
    if (f == FontId::Vcr) return g_vcr;
    if (f == FontId::Pixel) return g_pixel;
    return g_ui;
}

float randf(float lo, float hi) { return lo + (hi - lo) * float(std::rand()) / float(RAND_MAX); }

void init(const std::string& baseDir) {
    g_title = loadFont(baseDir + "assets/fonts/CinzelDecorative-Bold.ttf", 112, g_titleOwned);
    g_ui = loadFont(baseDir + "assets/fonts/Rajdhani-Bold.ttf", 72, g_uiOwned);
    g_vcr = loadFont(baseDir + "assets/fonts/vcr.ttf", 72, g_vcrOwned);
    g_pixel = loadFont(baseDir + "assets/fonts/5by7.ttf", 72, g_pixelOwned);

    Image gi = GenImageColor(128, 128, BLANK);
    Color* px = static_cast<Color*>(gi.data);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const float dx = (x + 0.5f) / 64.0f - 1.0f, dy = (y + 0.5f) / 64.0f - 1.0f;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float a = saturate(1.0f - r);
            px[y * 128 + x] = Color{255, 255, 255, (unsigned char)(a * a * 255.0f)};
        }
    g_glow = LoadTextureFromImage(gi);
    UnloadImage(gi);
    GenTextureMipmaps(&g_glow);
    SetTextureFilter(g_glow, TEXTURE_FILTER_TRILINEAR);

    for (int d = 0; d < 5; ++d) g_note[d] = makeGlyph(d == kCenter ? 1 : 0, dirColor(d), false, 384);
    g_receptor[0] = makeGlyph(0, WHITE, true, 384);
    g_receptor[1] = makeGlyph(1, WHITE, true, 384);
    loadHrk(baseDir);
}

void shutdown() {
    if (g_titleOwned) UnloadFont(g_title);
    if (g_uiOwned) UnloadFont(g_ui);
    if (g_vcrOwned) UnloadFont(g_vcr);
    if (g_pixelOwned) UnloadFont(g_pixel);
    UnloadTexture(g_glow);
    for (auto& t : g_note) UnloadTexture(t);
    for (auto& t : g_receptor) UnloadTexture(t);
    for (auto& t : g_receptorDir) UnloadTexture(t);
    for (auto& t : g_holdPiece) UnloadTexture(t);
    for (auto& t : g_holdEnd) UnloadTexture(t);
    g_hrkFrames.clear();
}

// ---------------------------------------------------------------- canvas --

static int g_targetW = 0, g_targetH = 0;
static float g_viewW = W;
static RenderTexture2D g_frameRt{};  // rendering mode: frames drawn here
void setRenderTargetSize(int w, int h) {
    g_targetW = w > 0 ? w : (g_frameRt.id ? g_frameRt.texture.width : 0);
    g_targetH = h > 0 ? h : (g_frameRt.id ? g_frameRt.texture.height : 0);
}

void beginFrameTarget(const RenderTexture2D& rt) {
    g_frameRt = rt;
    setRenderTargetSize(0, 0);
    BeginTextureMode(rt);
}

void endFrameTarget() {
    EndTextureMode();
    g_frameRt = {};
    setRenderTargetSize(0, 0);
}

bool frameTargetActive() { return g_frameRt.id != 0; }

void beginTexture(const RenderTexture2D& rt) { BeginTextureMode(rt); }

void endTexture() {
    EndTextureMode();
    if (g_frameRt.id) BeginTextureMode(g_frameRt);  // back to the frame being rendered
}

int targetWidth() { return g_targetW > 0 ? g_targetW : GetScreenWidth(); }
int targetHeight() { return g_targetH > 0 ? g_targetH : GetScreenHeight(); }

void setViewWidth(float w) { g_viewW = std::clamp(w, 64.0f, W); }
float viewWidth() { return g_viewW; }

// A render texture holds the whole canvas; the window shows 0..g_viewW of it.
static float shownWidth() { return g_targetW > 0 ? W : g_viewW; }

float canvasScale() {
    const float sw = float(g_targetW > 0 ? g_targetW : GetScreenWidth()), sh = float(g_targetH > 0 ? g_targetH : GetScreenHeight());
    return std::min(sw / shownWidth(), sh / H);
}

Vector2 canvasOrigin() {
    const float sw = float(g_targetW > 0 ? g_targetW : GetScreenWidth()), sh = float(g_targetH > 0 ? g_targetH : GetScreenHeight());
    const float s = canvasScale();
    return {(sw - shownWidth() * s) * 0.5f, (sh - H * s) * 0.5f};
}

Vector2 toCanvas(Vector2 p) {
    const Vector2 o = canvasOrigin();
    const float s = canvasScale();
    return {(p.x - o.x) / s, (p.y - o.y) / s};
}

Camera2D screenCamera(float zoom, Vector2 focus, float rotationDeg) {
    const float s = canvasScale();
    const Vector2 o = canvasOrigin();
    Camera2D cam{};
    cam.offset = {o.x + s * W * 0.5f, o.y + s * H * 0.5f};
    cam.target = focus;
    cam.rotation = rotationDeg;
    cam.zoom = s * zoom;
    return cam;
}

void beginCanvas(float zoom, Vector2 focus, float rotationDeg) { BeginMode2D(screenCamera(zoom, focus, rotationDeg)); }
void endCanvas() { EndMode2D(); }

void drawLetterbox() {
    const float sw = float(targetWidth()), sh = float(targetHeight());
    const float s = canvasScale();
    const Vector2 o = canvasOrigin();
    const float right = o.x + g_viewW * s;
    if (o.x > 0.5f) DrawRectangleRec({0, 0, o.x + 1, sh}, BLACK);
    if (sw - right > 0.5f) DrawRectangleRec({right - 1, 0, sw - right + 2, sh}, BLACK);
    if (o.y > 0.5f) {
        DrawRectangleRec({0, 0, sw, o.y + 1}, BLACK);
        DrawRectangleRec({0, sh - o.y - 1, sw, o.y + 2}, BLACK);
    }
}

// ------------------------------------------------------------------ text --

Vector2 measure(FontId f, std::string_view s, float size) {
    const std::string str(s);
    const Font& font = fontOf(f);
    return MeasureTextEx(font, str.c_str(), size, f == FontId::Title ? size * 0.02f : 0.0f);
}

void text(FontId f, std::string_view s, float x, float y, float size, Color c, Align a, float outline, Color oc) {
    const std::string str(s);
    const Font& font = fontOf(f);
    const float spacing = f == FontId::Title ? size * 0.02f : 0.0f;
    float px = x;
    if (a != Align::Left) {
        const float w = MeasureTextEx(font, str.c_str(), size, spacing).x;
        px = a == Align::Center ? x - w * 0.5f : x - w;
    }
    if (outline > 0) {
        oc.a = (unsigned char)(oc.a * (c.a / 255.0f));
        static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        for (auto& d : dirs) DrawTextEx(font, str.c_str(), {px + d[0] * outline, y + d[1] * outline}, size, spacing, oc);
    }
    DrawTextEx(font, str.c_str(), {px, y}, size, spacing, c);
}

void glowText(FontId f, std::string_view s, float x, float y, float size, Color c, Color g, Align a) {
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < 6; ++i) {
        const float ang = float(i) * 1.047f;
        const float r = size * 0.04f;
        text(f, s, x + std::cos(ang) * r, y + std::sin(ang) * r, size, fade(g, 0.18f), a);
    }
    EndBlendMode();
    text(f, s, x, y, size, c, a, size * 0.035f);
}

// --------------------------------------------------------------- colours --

Color rgb(unsigned hex, unsigned char a) {
    return Color{(unsigned char)((hex >> 16) & 0xFF), (unsigned char)((hex >> 8) & 0xFF), (unsigned char)(hex & 0xFF), a};
}
Color fade(Color c, float a) { c.a = (unsigned char)(c.a * saturate(a)); return c; }
Color mix(Color a, Color b, float t) { return lerpColor(a, b, t); }
Color brighten(Color c, float amount) {
    return amount >= 0 ? lerpColor(c, Color{255, 255, 255, c.a}, amount) : lerpColor(c, Color{0, 0, 0, c.a}, -amount);
}

// ---------------------------------------------------------------- shapes --

void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0) DrawTriangle(a, c, b, col);
    else DrawTriangle(a, b, c, col);
}
void quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color col) {
    tri(a, b, c, col);
    tri(a, c, d, col);
}
void convex(const std::vector<Vector2>& pts, Color col) {
    for (size_t i = 1; i + 1 < pts.size(); ++i) tri(pts[0], pts[i], pts[i + 1], col);
}
void capsule(Vector2 a, Vector2 b, float r, Color col) {
    DrawLineEx(a, b, r * 2.0f, col);
    DrawCircleV(a, r, col);
    DrawCircleV(b, r, col);
}
void gradientRect(Rectangle r, Color top, Color bottom) { DrawRectangleGradientEx(r, top, bottom, top, bottom); }

Texture2D& glowTexture() { return g_glow; }

void glow(Vector2 c, float radius, Color col) {
    BeginBlendMode(BLEND_ADDITIVE);
    DrawTexturePro(g_glow, {0, 0, 128, 128}, {c.x, c.y, radius * 2, radius * 2}, {radius, radius}, 0, col);
    EndBlendMode();
}
void softCircle(Vector2 c, float radius, Color col) {
    DrawTexturePro(g_glow, {0, 0, 128, 128}, {c.x, c.y, radius * 2, radius * 2}, {radius, radius}, 0, col);
}

// ----------------------------------------------------------------- notes --

int laneDir(int keys, int lane) {
    static const int dirs[10][9] = {
        {},
        {kCenter},
        {kLeft, kRight},
        {kLeft, kCenter, kRight},
        {kLeft, kDown, kUp, kRight},
        {kLeft, kDown, kCenter, kUp, kRight},
        {kLeft, kUp, kRight, kLeft, kDown, kRight},
        {kLeft, kUp, kRight, kCenter, kLeft, kDown, kRight},
        {kLeft, kDown, kUp, kRight, kLeft, kDown, kUp, kRight},
        {kLeft, kDown, kUp, kRight, kCenter, kLeft, kDown, kUp, kRight},
    };
    keys = clampv(keys, 1, 9);
    return dirs[keys][clampv(lane, 0, keys - 1)];
}

Color dirColor(int dir) {
    switch (dir) {
        case kLeft: return Color{255, 92, 138, 255};
        case kDown: return Color{54, 181, 255, 255};
        case kUp: return Color{255, 201, 60, 255};
        case kRight: return Color{60, 224, 160, 255};
        default: return Color{201, 166, 255, 255};
    }
}

static float dirRotation(int dir) {
    switch (dir) {
        case kUp: return 90.0f;
        case kRight: return 180.0f;
        case kDown: return 270.0f;
        default: return 0.0f;
    }
}

void drawNote(int dir, Vector2 c, float size, float alpha, float scale, float extraRotDeg) {
    const Texture2D& t = g_note[clampv(dir, 0, 4)];
    const float s = size * scale;
    // HRK sprites are directional already (lane is shown by colour, like
    // H-Slice); the procedural glyphs need the per-direction rotation.
    const float rot = (g_hrk ? 0.0f : dirRotation(dir)) + extraRotDeg;
    DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {c.x, c.y, s, s}, {s * 0.5f, s * 0.5f},
                   rot, fade(WHITE, alpha));
}

void drawReceptor(int dir, Vector2 c, float size, Color tint, float scale, float extraRotDeg) {
    const float s = size * scale;
    if (g_hrk && dir >= kLeft && dir <= kRight && g_receptorDir[dir].id) {
        const Texture2D& t = g_receptorDir[dir];
        DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {c.x, c.y, s, s}, {s * 0.5f, s * 0.5f},
                       extraRotDeg, tint);
        return;
    }
    const Texture2D& t = g_receptor[dir == kCenter ? 1 : 0];
    DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {c.x, c.y, s, s}, {s * 0.5f, s * 0.5f},
                   dirRotation(dir) + extraRotDeg, tint);
}

void drawSustain(int dir, float x, float y0, float y1, float width, float alpha, bool capAtY1) {
    if (std::fabs(y1 - y0) < 0.5f) return;
    const int d = clampv(dir, 0, 4);
    if (g_hrk && g_holdPiece[d].id) {
        const float top = std::min(y0, y1), h = std::fabs(y1 - y0);
        DrawRectangleRec({x - width * 0.5f - 3, top, width + 6, h}, fade(Color{22, 12, 38, 255}, alpha * 0.85f));
        const Texture2D& p = g_holdPiece[d];
        const float tile = std::max(8.0f, width);  // 60px source tile drawn at width px: aspect kept
        for (float y = top; y < top + h; y += tile) {
            const float th = std::min(tile, top + h - y);
            DrawTexturePro(p, {0, 0, 60, 60}, {x - width * 0.5f, y, width, th}, {0, 0}, 0,
                           fade(WHITE, alpha * 0.95f));
        }
        if (capAtY1 && g_holdEnd[d].id) {
            // The sprite is a bar segment on top with the diamond tip below:
            // anchor its tip to the sustain end, flipping when the tail is up.
            const Texture2D& e = g_holdEnd[d];
            const float es = width * 1.5f;
            if (y1 >= y0) {
                DrawTexturePro(e, {0, 0, 60, 60}, {x - es * 0.5f, y1 - es, es, es}, {0, 0}, 0,
                               fade(WHITE, alpha));
            } else {
                DrawTexturePro(e, {0, 60, 60, -60}, {x - es * 0.5f, y1, es, es}, {0, 0}, 0,
                               fade(WHITE, alpha));
            }
        }
        return;
    }
    const Color base = dirColor(dir);
    const float top = std::min(y0, y1), h = std::fabs(y1 - y0);
    DrawRectangleRec({x - width * 0.5f - 3, top, width + 6, h}, fade(Color{22, 12, 38, 255}, alpha * 0.85f));
    DrawRectangleRec({x - width * 0.5f, top, width, h}, fade(brighten(base, -0.15f), alpha * 0.9f));
    DrawRectangleRec({x - width * 0.18f, top, width * 0.36f, h}, fade(brighten(base, 0.45f), alpha * 0.9f));
    if (capAtY1) {
        DrawCircleV({x, y1}, width * 0.5f + 3, fade(Color{22, 12, 38, 255}, alpha * 0.85f));
        DrawCircleV({x, y1}, width * 0.5f, fade(brighten(base, -0.15f), alpha * 0.9f));
        DrawCircleV({x, y1}, width * 0.18f, fade(brighten(base, 0.45f), alpha * 0.9f));
    }
}

// ------------------------------------------------------------- particles --

void Particles::update(float dt, float gravity) {
    for (Particle& p : ps_) {
        p.life -= dt;
        p.v.y += gravity * dt;
        p.p.x += p.v.x * dt;
        p.p.y += p.v.y * dt;
    }
    ps_.erase(std::remove_if(ps_.begin(), ps_.end(), [](const Particle& p) { return p.life <= 0; }), ps_.end());
}

void Particles::draw(bool additive) const {
    if (ps_.empty()) return;
    if (additive) BeginBlendMode(BLEND_ADDITIVE);
    for (const Particle& p : ps_) {
        const float t = saturate(p.life / p.maxLife);
        if (p.kind == 0) {
            const float s = p.size * (0.5f + 0.5f * t);
            DrawTexturePro(g_glow, {0, 0, 128, 128}, {p.p.x, p.p.y, s * 2, s * 2}, {s, s}, 0, fade(p.c, t));
        } else if (p.kind == 1) {
            const float len = std::sqrt(p.v.x * p.v.x + p.v.y * p.v.y) * 0.04f + 2.0f;
            const float inv = 1.0f / std::max(1e-3f, std::sqrt(p.v.x * p.v.x + p.v.y * p.v.y));
            DrawLineEx(p.p, {p.p.x - p.v.x * inv * len, p.p.y - p.v.y * inv * len}, p.size * t + 0.5f, fade(p.c, t));
        } else {
            const float r = p.size * (1.0f - t) + 4.0f;
            DrawRing(p.p, r, r + 3.0f * t + 1.0f, 0, 360, 36, fade(p.c, t));
        }
    }
    if (additive) EndBlendMode();
}

}  // namespace gh::gfx
