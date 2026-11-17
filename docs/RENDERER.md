# Renderer Backend Abstraction — Design Note (P6)

Status: **design only** — no OpenGL compute rendering in this pass.

## Current state (P5 findings)

- Notes draw via `gfx::drawNote` → `DrawTexturePro` — **one GPU draw call per
  visible note**. raylib has no public instancing API.
- Culling already exists and is good: playback-window `lower/upper_bound` per
  lane, offscreen-Y reject, per-lane `minGap` pixel dedup, visibility/alpha
  early-outs, sustain head/tail culling, `rlDrawRenderBatchActive` batching in
  rotated mode. Sustains and gameplay notes are never dropped for looks.
- `ui.note()`/`ui.hold()` may take over drawing for Psych note skins — any
  batched path must keep that override intact.

## Proposed abstraction

```cpp
class NoteRenderer {
public:
    virtual ~NoteRenderer() = default;
    virtual void beginFrame() = 0;
    // One instance per visible note; impl decides CPU vs GPU path.
    virtual void addNote(const NoteInstance& inst) = 0;
    virtual void endFrame() = 0;   // flush / issue draws
};
struct NoteInstance {               // SoA-friendly, ~32 bytes
    float x, y, size, rotation, alpha, scale;
    uint16_t dir, kind; uint32_t texSlot;
};
```

Backends:
1. **RaylibNoteRenderer** (current): wraps existing draw calls — fallback for
   all hardware, used when compute/instancing unavailable.
2. **RlglInstancedRenderer** (near-term, no compute needed): packs visible
   instances into a reusable VBO, draws each direction texture with a custom
   shader + `glVertexAttribDivisor` — ~5 draw calls/frame instead of N.
   Requires GL 3.3+ (raylib baseline) — no backend swap needed.
3. **Gl43ComputeRenderer** (future): SSBO of instances → compute shader does
   viewport/window/lane culling + stream compaction → `glDrawElementsIndirect`.
   Needs GL 4.3+; feature-detected at init, falls back to (2)/(1).
4. **VulkanRenderer** (optional later): same instance interface, VkDrawIndexedIndirect.

## Rules preserved

- Only the active playback window is uploaded (instances generated from the
  same culling pass that already runs on CPU).
- Gameplay judgement never touches the renderer — invisible ≠ discarded.
- Fallback chain: compute → instanced → raylib immediate mode.
