#pragma once

#include <cstdint>

// Phase 4 smoke test: prove the Lime/OpenFL OpenGL context can do
// instanced draw calls without touching the Flixel renderer.
// All entry points are safe to call from the Haxe main thread while a
// GL context is current; they save and restore every GL state they use.

// Capabilities report as a static string (valid until next call):
// GL version/renderer/vendor, context-current flag, feature tier and
// per-feature availability. NULL-safe: returns "no-gl" when the loader
// or a current context is missing.
const char* rs_gl_probe();

// Runs one instanced-draw frame and returns milliseconds (CPU submit +
// glFinish). count = instance count, mode selects the buffer strategy:
//   0 static    - instance data uploaded once, draw only
//   1 dynamic   - CPU refills + glBufferSubData every frame
//   2 orphan    - CPU refills + glBufferData(NULL) + glBufferSubData
//   3 indirect  - static data, glDrawArraysIndirect (if available)
// Returns -1 when the smoke renderer failed to initialize.
double rs_gl_smoke_frame(int count, int mode);

// Rolling stats as a static string: frames, avg/max ms, upload MB/s,
// last error. Valid until the next call.
const char* rs_gl_smoke_stats();

// Vendor-neutral capability bitmask for renderer fallback decisions.
// Bit layout: 0 VAO, 1 INSTANCED, 2 DIVISOR, 3 INDIRECT, 4 MULTI_INDIRECT,
// 5 COMPUTE, 6 SSBO, 7 BUFFER_STORAGE, 8 MAP_RANGE, 9 TEX_STORAGE,
// 10 FENCE_SYNC, 11 COMPUTE_OK (verified end-to-end), 12 PERSISTENT_MAP,
// 13 CORE_PROFILE, 14 GLSL_3_30. 0 when the GL loader fails.
int rs_gl_caps();

// Hardware limits for renderer sizing/dispatch clamping. `which`:
//   0=MAX_TEXTURE_SIZE 1=MAX_VERTEX_ATTRIBS 2=MAX_SHADER_STORAGE_BLOCK_BINDINGS
//   3=MAX_COMPUTE_WORK_GROUP_INVOCATIONS 4=MAX_UNIFORM_BLOCK_SIZE
// Returns -1 when the limit is unsupported or GL is unavailable.
double rs_gl_limit(int which);

// ---------------------------------------------------------------------------
// Phase 5D note renderer — instanced GPU-side note positioning.
// Slot-based: rs_notes_create returns 0..7 or -1. All functions are
// context-current-safe (save/restore GL state) and no-ops on bad handles.
// ---------------------------------------------------------------------------

// Create a note renderer (compiles shaders, allocates buffers). -1 on failure.
int rs_notes_create();
void rs_notes_destroy(int h);
void rs_notes_reset(int h);

// Upload the noteskin atlas as a straight-alpha RGBA texture.
// pixPtr is the raw pixel address passed as a double bit-pattern
// (Haxe lime.utils.DataPointer). Returns 1 on success.
int rs_notes_setTexture(int h, double pixPtr, int w, int hh);

// Per-direction render constants, 16 floats packed per direction:
//   e0=(fw,fh,sx,sy) e1=(ox,oy,offX,offY) e2=(xBase,u0,v0,u1) e3=(v1,0,0,0)
void rs_notes_setDir(int h, int dir,
                     double v0, double v1, double v2, double v3,
                     double v4, double v5, double v6, double v7,
                     double v8, double v9, double v10, double v11,
                     double v12, double v13, double v14, double v15);

void rs_notes_add(int h, float time, int dir);
void rs_notes_kill(int h, int index);
void rs_notes_removePrefix(int h, int n);

// Draw one frame. now=conductor time ms, pxPerMs=PIXELS_PER_MS*scrollSpeed,
// dirSign=+1 downscroll/-1 up, strum/scroll in world px, r00..r12 = view->
// device-px affine (two rows), cull* = view-space bounds (same as CPU path).
// Returns 1 when submitted, 0 when not ready (caller uses CPU fallback).
int rs_notes_draw(int h,
                  double now, double pxPerMs, double dirSign,
                  double strumX, double strumY, double scrollX, double scrollY,
                  double r00, double r01, double r02,
                  double r10, double r11, double r12,
                  double cullX0, double cullY0, double cullX1, double cullY1);
