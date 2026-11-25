// V-Slice / H-Slice (base-game FNF) UI kit: chunky rounded panels, buttons,
// tab bars and song cards. Used by the menu states; canvas coordinates.
#pragma once

#include <string>
#include <vector>

#include "raylib.h"

namespace gh::vs {

// Base-game palette: near-black navy panels, white text, orange accent.
inline const Color kPanel{12, 14, 34, 225};
inline const Color kPanelEdge{70, 80, 140, 255};
inline const Color kAccent{255, 152, 48, 255};      // FNF orange
inline const Color kAccentDark{190, 100, 24, 255};
inline const Color kText{245, 246, 252, 255};
inline const Color kDim{150, 158, 185, 255};
inline const Color kGood{110, 230, 140, 255};
inline const Color kBad{255, 96, 110, 255};

// Rounded dark panel; optional title strip on top.
void panel(Rectangle r, const char* title = nullptr);

// Chunky button. `sel` = keyboard/gamepad highlight; `accent` = filled orange.
// Returns true when clicked (mouse). Keyboard selection is the caller's job.
bool button(Rectangle r, const std::string& label, bool sel, bool accent = false, float size = 30.0f);

// Small pill toggle drawn on the right of a row; returns the drawn state.
void toggle(Rectangle r, bool on, bool sel);

// Left-hand tab column (V-Slice options). Returns the index of a clicked tab.
int tabs(float x, float y, float w, float h, const std::vector<std::string>& labels, int sel);

// Song "album art" card for the freeplay carousel: colored frame, icon area
// caller fills, title strip underneath. Returns the card rectangle.
Rectangle songCard(float cx, float cy, float w, float h, const Color& tint, float alpha);

// Big PLAY-style action button centred at cx,cy.
bool bigButton(float cx, float cy, float w, float h, const std::string& label, bool sel);

}  // namespace gh::vs
