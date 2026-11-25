# RAID GPU Compatibility — AMD / Vendor-Neutral Audit

Status: **verified on NVIDIA GTX 1060 and AMD RX 580 (Polaris)**.
AMD results from real hardware (Adrenalin 26.5.2, GL 4.6 Compatibility
Profile). RX 6000/7000 and Intel iGPU remain untested but use the same
capability paths. See §6 for the full matrix.

## 1. Architecture audit

Two GL code paths exist:

| Path | File | GL usage | Vendor risk |
|---|---|---|---|
| Production note renderer | `RaidGLNotes.hx` | None — OpenFL `Graphics.drawTriangles` + `beginBitmapFill` inside Flixel's camera draw stack | **None.** Whatever GL driver Lime/OpenFL uses renders it. No raw GL entry points, no shaders of our own, no extensions. |
| Phase 4 smoke / future 5D–5F | `raid_gl_smoke.cpp` | Runtime-loaded GL 3.3+/4.3+ entry points | Audited below |

`RaidGLNotes` positions quads on the CPU today (Phase 5D will move that
into a vertex shader). Until then the production path needs **zero** GL
capabilities beyond what OpenFL already requires — it is the universal
fallback by construction.

## 2. Findings & fixes applied

### FIXED — `#version ... core` profile keyword (AMD compat-context risk)

Lime creates the GL context with a legacy `wglCreateContext`, which yields
a **compatibility-profile** context on both vendors. GLSL shaders written
as `#version 330 core` / `#version 430 core` explicitly request the core
profile; strict drivers (AMD's is stricter than NVIDIA's) can reject them
in a compatibility context.

- `kVertSrc` / `kFragSrc`: `#version 330 core` → `#version 330`
- compute test shader: `#version 430 core` → `#version 430`

Omitting the profile keyword makes the shader adopt the context's profile —
valid in both core and compatibility contexts on all vendors.

Probe now reports `profile=core|compat|none` via `GL_CONTEXT_PROFILE_MASK`,
the GLSL shading-language version (`glsl=`), and hardware limits
(`maxTexSize`, `maxVertAttribs`, `maxSSBOBindings`,
`maxComputeInvocations`, `maxUniformBlock`) so all of this can be
confirmed on any machine.

### ADDED — shader-compile fallback

`ensureInit` now tries the `#version 330` shader pair first; if
compilation fails with a version-related error it retries a
`#version 120` pair (`attribute`/`varying`, `gl_FragColor`,
`texture2D`, no `layout()` qualifiers, no `gl_InstanceID` — the smoke
wobble is cosmetic). Attribute locations 0–4 are bound by name via
`glBindAttribLocation` for both variants, so the fallback is
deterministic. The probe reports `shaderLevel=330|120` (or
`shaderErr=...`), making shader compilation itself part of AMD
validation. The compute test remains `#version 430`-only — its failure
is already a graceful capability bit (`COMPUTE_OK`), not fatal.

### VERIFIED CLEAN — function loading

`glproc()` resolves every entry point through `wglGetProcAddress` with an
`opengl32.dll` `GetProcAddress` fallback, and all post-1.1 functions are
loaded as optional pointers checked before use. This is the standard
portable mechanism — identical on NVIDIA, AMD, and Intel ICDs. No
vendor-string branching anywhere.

### VERIFIED CLEAN — state save/restore

`GLState`/`saveState`/`restoreState` query ~30 state variables by enum and
restore them. All queried enums are GL 1.x–3.x core state present on any
driver that can run OpenFL. No vendor extensions touched.

### VERIFIED CLEAN — smoke draw paths

- Instanced path: `glDrawArraysInstanced` + `glVertexAttribDivisor` —
  core GL 3.3 / ARB_instanced_arrays, supported by every AMD GPU since
  TeraScale 2 (HD 5000+) and all GCN/RDNA parts.
- Indirect path (mode 3): gated on `glDrawArraysIndirect != nullptr`;
  falls back to instanced automatically.
- Compute/SSBO test: gated on pointer presence **and** an end-to-end
  dispatch+readback check (`computeTest()`), not just symbol resolution.

## 3. Capability detection — `rs_gl_caps()`

New vendor-neutral bitmask (bit positions documented in
`raid_gl_smoke.hpp` / `RaidGLSmoke.hx`):

| Bit | Capability | Detection method |
|---:|---|---|
| 0 | VAO | `glGenVertexArrays` pointer |
| 1 | Instanced draw | `glDrawArraysInstanced` pointer |
| 2 | Attrib divisor | `glVertexAttribDivisor` pointer |
| 3 | Indirect draw | `glDrawArraysIndirect` pointer |
| 4 | Multi indirect | `glMultiDrawArraysIndirect` pointer |
| 5 | Compute shader | `glDispatchCompute` pointer |
| 6 | SSBO | `glBindBufferBase` pointer |
| 7 | Buffer storage | `glBufferStorage` pointer |
| 8 | MapBufferRange | `glMapBufferRange` + `glUnmapBuffer` pointers |
| 9 | TexStorage | `glTexStorage2D` pointer |
| 10 | Fence sync | `glFenceSync` + `glClientWaitSync` + `glDeleteSync` |
| 11 | **Compute verified** | live dispatch + SSBO readback match |
| 12 | Persistent-map capable | `glMapBufferRange` pointer (GL 4.4/ARB semantics assumed; callers must still handle failure) |
| 13 | Core profile | `GL_CONTEXT_PROFILE_MASK` query |
| 14 | GLSL ≥ 3.30 | parsed `GL_VERSION` |

Detection is per-entry-point and per-extension — **never** keyed on
`GL_VENDOR`/`GL_RENDERER` strings. Vendor strings are reported for
diagnostics only.

### ADDED — `rs_gl_limit(which)` hardware-limit API

Programmatic access to the limits shown in the probe, for renderers
that must size buffers or clamp dispatches at runtime:

| `which` | Limit |
|---:|---|
| 0 | `GL_MAX_TEXTURE_SIZE` |
| 1 | `GL_MAX_VERTEX_ATTRIBS` |
| 2 | `GL_MAX_SHADER_STORAGE_BLOCK_BINDINGS` |
| 3 | `GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS` |
| 4 | `GL_MAX_UNIFORM_BLOCK_SIZE` |

Returns -1 when unsupported. Errors are checked via `glGetError` so a
bogus pname on an old driver yields -1 rather than garbage.

## 4. Fallback matrix (for Phase 5D–5F consumers)

| Feature wanted | Requires | Fallback when absent |
|---|---|---|
| GPU-side note positioning | bits 0,1,2,14 | CPU quad build (current `RaidGLNotes` path — always works) |
| Persistent instance buffer | bits 7,8,12 | `glBufferData`/`glBufferSubData` per frame (smoke modes 0–2) |
| Indirect draw | bit 3 | `glDrawArraysInstanced` (bit 1) — proven path |
| Compute compaction/culling | bits 5,6,11 | CPU-side visibility window (current head-pointer culling) |
| Buffer mapping | bit 8 | `glGetBufferSubData` readback or CPU-side state |
| Sync/fencing | bit 10 | `glFinish` (already used in smoke; slower but correct) |

Every fallback preserves visual correctness; only performance differs.

## 5. AMD-specific risk notes (theoretical)

- **GLSL profile keyword** — fixed (see §2).
- **`glBufferStorage` on GL < 4.4**: exposed via `ARB_buffer_storage` on
  AMD GCN; pointer check covers it. RX 580 (Polaris) and newer are GL 4.5/4.6.
- **Compute shaders**: require GL 4.3+ or `ARB_compute_shader`; AMD has
  shipped both since GCN 1.0 (Southern Islands, 2012). The verified
  end-to-end `computeTest` guards this regardless.
- **`GL_SYNC` timeouts**: `glClientWaitSync` with `TIMEOUT_IGNORED` can
  block; future code should use finite timeouts + `glFenceSync` polling.
- **Driver bugs**: AMD's Windows OpenGL driver historically lags on
  bleeding-edge extensions; all features we use are years old and stable.
- **AMD iGPU+dGPU (Ryzen APU)**: same Optimus-style split as NVIDIA —
  handled by the OS/driver, not our code. `NvOptimusEnablement` is NVIDIA-
  specific but harmless; AMD equivalents
  (`AmdPowerXpressRequestHighPerformance`) are already exported by lime.

## 6. Validation status

| Check | NVIDIA GTX 1060 | AMD RX 580 (verified) |
|---|---|---|
| Probe/tier/features | GL 4.6, `glsl=4.60`, tier3, all features=1, computeTest=1 | GL 4.6 **compat profile**, `glsl=4.60`, tier3, all features=1, **computeTest=1** |
| Shader compile | `shaderLevel=330` | `shaderLevel=330` — compiles on compat profile |
| Profile detection | `profile=none` (NVIDIA doesn't report mask) | `profile=compat` — confirms the `core`-keyword fix was required |
| Hardware limits | maxTex=32768 vertAttribs=16 ssboBind=96 computeInvoc=1536 ubo=64KB | maxTex=16384 vertAttribs=32 ssboBind=96 computeInvoc=1024 ubo=4MB |
| Pixel verify | `verify=0ok/2fail` (known color-space dimming, Phase 4) | **`verify=2ok/0fail`** — pixel-perfect |
| Instancing smoke | ~1.2–1.9ms avg @100k instances | ~7.9ms avg @200k instances |
| Production renderer (`RaidGLNotes`) | verified (dense-zone 4–8ms EMA) | verified — 60fps locked through dense zone, worst frame 17ms |
| Dense-chart gameplay | ~4–8ms EMA, ~125–250fps | `frame=16.6ms` vsync-locked 60fps, worst=17ms, sprites ~49/side |
| Song completion | results screen, clean exit | results screen, clean exit |
| Regression: normal charts | unchanged — `.raid`-only path | unchanged |

### To validate on AMD hardware

1. Run `Funkin.exe` with `RAID_GL_SMOKE=1 RAID_PLAYSTATE=1
   RAID_CHART=<chart>.raid RAID_BOTPLAY=1`.
2. Record the `gl-probe` line: expect `version=4.x`, `renderer=AMD Radeon
   ...`, `glsl=4.x`, `profile=compat` (or core), `tier: tier3` on
   RX 5000+, plus the `max*=...` limits. `shaderLevel=330` expected;
   `shaderLevel=120` means the fallback engaged (still functional).
3. Confirm `computeTest=1` and `verify=2ok/0fail` in `gl-smoke` stats.
4. Run a dense `.raid` chart and compare `frame=` EMA vs the GTX 1060
   numbers in `RAID-GL-PHASE4.md` / the Phase 7 dense-zone log.
