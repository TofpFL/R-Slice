// V-Slice UI kit implementation - see vslice_ui.hpp.
#include "vslice_ui.hpp"

#include "gfx.hpp"

namespace gh::vs {

namespace {
struct Mouse {
    Vector2 p;
    bool click;
};
Mouse mouse() {
    return {gfx::toCanvas(GetMousePosition()), IsMouseButtonPressed(MOUSE_BUTTON_LEFT)};
}
bool inside(const Mouse& m, Rectangle r) { return CheckCollisionPointRec(m.p, r); }
}

void panel(Rectangle r, const char* title) {
    DrawRectangleRounded(r, 0.05f, 8, Color{6, 8, 22, 160});
    DrawRectangleRoundedLinesEx(r, 0.05f, 8, 2, kPanelEdge);
    if (title) {
        const float tw = gfx::measure(gfx::FontId::Ui, title, 22).x;
        DrawRectangleRounded({r.x + 18, r.y - 13, tw + 28, 26}, 0.4f, 6, Color{12, 14, 34, 240});
        DrawRectangleRoundedLinesEx({r.x + 18, r.y - 13, tw + 28, 26}, 0.4f, 6, 1, kPanelEdge);
        gfx::text(gfx::FontId::Ui, title, r.x + 32, r.y - 16, 20, kAccent);
    }
}

bool button(Rectangle r, const std::string& label, bool sel, bool accent, float size) {
    const Mouse m = mouse();
    const bool hov = inside(m, r);
    const Color fill = accent ? kAccent : (sel || hov) ? Color{52, 60, 110, 255} : Color{24, 28, 60, 255};
    DrawRectangleRounded(r, 0.18f, 8, fill);
    DrawRectangleRoundedLinesEx(r, 0.18f, 8, sel ? 3 : 2, sel || hov ? kAccent : kPanelEdge);
    gfx::text(gfx::FontId::Ui, label, r.x + r.width / 2, r.y + r.height / 2 - size * 0.62f, size,
              accent ? Color{20, 14, 30, 255} : (sel ? kText : kDim), gfx::Align::Center);
    return hov && m.click;
}

void toggle(Rectangle r, bool on, bool sel) {
    DrawRectangleRounded(r, 0.5f, 8, on ? kAccent : Color{40, 46, 80, 255});
    const float kr = r.height * 0.36f;
    const float kx = on ? r.x + r.width - kr - 3 : r.x + kr + 3;
    DrawCircleV({kx, r.y + r.height / 2}, kr, sel ? WHITE : kText);
    DrawRectangleRoundedLinesEx(r, 0.5f, 8, sel ? 3 : 2, sel ? kAccent : kPanelEdge);
}

int tabs(float x, float y, float w, float h, const std::vector<std::string>& labels, int sel) {
    const Mouse m = mouse();
    int hit = -1;
    float ty = y;
    for (int i = 0; i < int(labels.size()); ++i) {
        const Rectangle r{x, ty, w, h};
        const bool on = i == sel;
        const bool hov = inside(m, r);
        DrawRectangleRounded(r, 0.12f, 8, on ? Color{52, 60, 110, 255} : Color{20, 24, 50, 200});
        DrawRectangleRoundedLinesEx(r, 0.12f, 8, on ? 3 : 1, on || hov ? kAccent : kPanelEdge);
        gfx::text(gfx::FontId::Ui, labels[size_t(i)], r.x + r.width / 2, r.y + r.height / 2 - 12, 22,
                  on ? kText : kDim, gfx::Align::Center);
        if (hov && m.click) hit = i;
        ty += h + 8;
    }
    return hit;
}

Rectangle songCard(float cx, float cy, float w, float h, const Color& tint, float alpha) {
    const Rectangle r{cx - w / 2, cy - h / 2, w, h};
    DrawRectangleRounded(r, 0.04f, 8, gfx::fade(kPanel, alpha));
    DrawRectangleRoundedLinesEx(r, 0.04f, 8, 2, gfx::fade(tint, alpha));
    return r;
}

bool bigButton(float cx, float cy, float w, float h, const std::string& label, bool sel) {
    const Mouse m = mouse();
    const Rectangle r{cx - w / 2, cy - h / 2, w, h};
    const bool hov = inside(m, r);
    DrawRectangleRounded(r, 0.22f, 10, sel || hov ? kAccent : Color{30, 34, 70, 255});
    DrawRectangleRoundedLinesEx(r, 0.22f, 10, sel ? 4 : 2, sel || hov ? WHITE : kAccent);
    gfx::text(gfx::FontId::Title, label, cx, cy - 30, 44, sel || hov ? Color{30, 20, 40, 255} : kText, gfx::Align::Center);
    return hov && m.click;
}

}  // namespace gh::vs
