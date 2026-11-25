// V-Slice freeplay song capsule — C++ port of SongMenuItem.
// Draws one capsule (612x132 design space) plus its sub-elements:
// title text w/ ghost + clip/marquee, pixel character icon, rank badge,
// BPM digits and the week/album tag. Positions per
// docs/VSLICE_FREEPLAY_LAYOUT.md.
#pragma once

#include <string>

#include "raylib.h"

namespace gh {

struct SongEntry;

// Loads all capsule assets once (call after gfx::init / audio init, with the
// same base dir). Safe to call repeatedly; no-op after first success.
void fpCapsuleInit(const std::string& base);
void fpCapsuleShutdown();
bool fpCapsuleReady();

// Draws a single capsule at design-space origin (x, y) — caller supplies the
// on-screen position (already includes the sinusoidal wiggle). `scale` is the
// V-Slice realScaled factor (0.8). Everything inside is drawn in the
// 612x132 group space, so pass unscaled coords.
//   e == nullptr -> the "Random!" capsule.
//   diff        -> currently selected difficulty name ("" = none).
//   scoreRank   -> rank badge name ("PERFECT" etc.) or "" for none.
//   bpm         -> song bpm; <=0 hides the BPM readout.
//   week        -> small tag text at the capsule's bottom (album/week).
//   selected    -> selected vs unselected visuals.
//   now         -> app time (seconds) for anim phases.
//   squashX/Y   -> final sprite scale (0.8 at rest; >/< during jump-in squash).
//                  Applies to the capsule art only, like V-Slice's
//                  CustomAnimControl — sub-elements are never squashed.
void fpDrawCapsule(const SongEntry* e, const char* diff, const char* scoreRank,
                   int bpm, const char* week, float x, float y, bool selected,
                   double now, int animIndex, float squashX = 0.8f, float squashY = 0.8f);

}  // namespace gh
