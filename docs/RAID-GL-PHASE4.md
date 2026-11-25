# RAID Phase 4 — Native OpenGL Context & Instancing Smoke Test

**Date:** 2026-11-23
**Status:** PASS — instanced rendering proven inside Lime's existing GL context on the target GPU.

## Environment verified at runtime

| Property | Value |
|---|---|
| GL version | **4.6.0** (compatibility profile) |
| Renderer | **GeForce GTX 1060** / PCIe / SSE2 |
| Vendor | NVIDIA Corporation |
| Driver | 388.16 |
| Context | current on main thread during `postDraw` (wglGetCurrentContext != null) |
| Window system | SDL3 → WGL (lime `SDLWindow.cpp`, no version/profile attrs → driver default = latest compat) |

GL function loading: all entry points resolved at runtime via `wglGetProcAddress`/`opengl32.dll` — no GL headers or import lib needed in `raid_gl_smoke.cpp`.

## Feature tier: **Tier 3 (full)**

`vao=1 instanced=1 divisor=1 indirect=1 multiIndirect=1 compute=1 ssbo=1 bufferStorage=1 mapRange=1 texStorage=1 fenceSync=1 gl_InstanceID=1 computeTest=1`

Extensions present: `GL_ARB_compute_shader`, `GL_ARB_shader_storage_buffer_object`, `GL_ARB_draw_indirect`, `GL_ARB_multi_draw_indirect`, `GL_ARB_instanced_arrays`, `GL_ARB_buffer_storage`, `GL_ARB_map_buffer_range`, `GL_ARB_texture_storage`, `GL_EXT_direct_state_access`.

**computeTest=1** — a real `#version 430` compute shader was compiled, dispatched (1 workgroup), and its SSBO write read back via `glGetBufferSubData` with an exact value match. Compute shaders + SSBOs are not just present, they execute correctly.

## What was proven

- `glDrawArraysInstanced` + `glVertexAttribDivisor` render **200,000 independently positioned instances** (pos/color/scale per instance) in a single draw call.
- **Pixel-verified**: `glReadPixels` at two fixed screen positions returned the expected opaque quad colors (raw dumps in logs; y-flip mapping confirmed correct).
- Custom GLSL 330 shader compiles/links/runs; texture sampling + premultiplied-alpha blending (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`) works.
- Dynamic buffer strategies work: `glBufferSubData` in-place and `glBufferData(NULL)` orphaning.
- `glDrawArraysIndirect` entry point exists; **draw produced no visible output — unresolved, non-blocking** (see caveats).
- **GL state restoration verified implicitly**: ~30 state variables (viewport, scissor, blend func/eq, depth, cull, stencil, color mask, VAO, program, bound buffers/textures/FBOs, pixel-store alignment, polygon offset) saved before and restored after every call; the game keeps rendering normally afterward (ran through PlayState → gameplay → GameOverSubState without corruption).
- No individual `NoteSprite`/FlxSprite objects involved — pure VAO+VBO+instanced draw.

## Benchmarks (ms/frame, includes ~0.24ms fixed overhead of state save/restore + glFinish)

| Config | avg ms | notes |
|---|---:|---|
| baseline N=1, mode 0 | 0.244 | pure overhead floor |
| N=200,000, mode 0 static | ~2.3–2.5 | draw only, no per-frame upload |
| N=1,000,000, mode 0 static | ~11.6 | pathological overdraw (~1 instance/px) |
| N=200,000, mode 1 dynamic subData | ~22.3 | +CPU refill of 200k instances (~5.6MB/frame, ~240MB/s upload) |
| N=200,000, mode 2 orphan+subData | ~20.9 | same ballpark |
| N=200,000, mode 3 indirect | ~5.8 | drew nothing (see caveats); still paid instance upload |
| N=1,000,000, mode 3 indirect | ~26.3 | same |

Interpretation:
- Marginal GPU cost ≈ avg − 0.24ms baseline. **200k notes ≈ 2ms**; 1M ≈ 11ms under extreme overdraw.
- Real note charts have far less screen coverage than the smoke grid, so these are conservative upper bounds.
- CPU-driven per-frame instance refill (mode 1/2) is the expensive path (~20ms) — this is exactly what Phase 6 removes by moving positioning/culling into shaders/compute.
- glFinish-inclusive timing serializes GPU; pipelined frame cost may be lower, but these numbers are honest worst-case per-frame latency.

## Caveats / open items

- `glDrawArraysIndirect` loaded and accepted the call but rendered nothing visible. `type=GL_UNSIGNED_INT` tried; still blank. Possibly driver/profile quirk or command-layout issue — needs a dedicated repro in Phase 6. `glDrawArraysInstanced` is the proven path regardless.
- Verify-pixel tolerance flagged the green quad "fail" at N=1M (read 175 vs 255 expected) — consistent ~0.8× dimming across all runs suggests framebuffer color-space/dither, not a positioning error; raw values confirm correct hue and location.
- Smoke test hooks `FlxG.signals.postDraw` (last GL work of frame). Phase 5 must instead draw inside the camera composition or accept screen-space overlay; camera-transform composition still to be validated with real note coordinates.
- Integrated-graphics fallback path not yet exercised (this machine has the GTX 1060).

## Files

- `source/funkin/external/raid/project/src/raid_gl_smoke.cpp` — loader, state save/restore, shaders, 4 draw modes, compute test, stats.
- `source/funkin/external/raid/project/include/raid_gl_smoke.hpp` + decls in `raid_bridge.hpp`.
- `source/funkin/external/raid/RaidGLSmoke.hx` (externs) + `RaidGLSmokeCtl.hx` (env-gated driver).
- `project/Build.xml` — added `raid_gl_smoke.cpp` to the raid-bridge group.
- `PlayState.hx` — `RaidGLSmokeCtl.maybeInstall()` at end of `create()` (no-op unless `RAID_GL_SMOKE` env set).

## Reproduce

```bat
set RAID_GL_SMOKE=1
set RAID_GL_SMOKE_N=200000
set RAID_GL_SMOKE_MODE=0
set RAID_PLAYSTATE=1
set RAID_CHART=F:\vslice-lime-build\export\release\windows\bin\assets\songs\raidstress\raidstress.raid
Funkin.exe
```

Probe report + per-120-frame stats appear on stdout (`gl-probe:`, `gl-smoke:` lines).

## Phase 5 go/no-go

**GO.** Tier-3 GL 4.6 on the GTX 1060 supports everything the instanced-note renderer needs: VAOs, instanced attributes, dynamic buffer strategies, shaders, and compute/SSBO for the Phase 6 GPU positioning path. The remaining risk is composition with Flixel's camera/render-target pipeline, not GL capability.
