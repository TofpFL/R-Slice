# RAID GPU Benchmark — GTX 1060 Laptop vs RX 580 Desktop

Date: 2025-11-23. Build: `-DRAID` lime export, judge + GL-notes pipeline.
Chart: Fraudulent `.raid` (59,579 events), botplay, dense zone 44–72s.

## Systems

| | NVIDIA (laptop) | AMD (desktop) |
|---|---|---|
| GPU | GeForce GTX 1060 6GB | Radeon RX 580 2048SP |
| CPU | Intel i7-7700HQ (4c/8t, mobile, thermal-limited) | AMD Ryzen 5 1400 (4c/8t, desktop) |
| GL context | 4.6.0, driver 388.16 | 4.6.0 **Compatibility Profile**, Adrenalin 26.5.2.260413 |
| Display path | Optimus (iGPU scanout + dGPU render) | Direct dGPU |

## GL capability probe

| Metric | GTX 1060 | RX 580 |
|---|---|---|
| Tier | tier3 | tier3 |
| Instancing/divisor/indirect/multiIndirect | all =1 | all =1 |
| Compute + SSBO (`computeTest`) | 1 (verified end-to-end) | **1 (verified end-to-end)** |
| bufferStorage/mapRange/texStorage/fenceSync | all =1 | all =1 |
| ARB extensions | all present | all present |
| `shaderLevel` | 330 | **330** (compat profile — `core`-keyword fix validated) |
| Pixel verify | 2ok/0fail (latest run) | **2ok/0fail** |
| maxTexSize / maxVertAttribs | 32768 / 16 | 16384 / 32 |
| maxSSBOBindings | 96 | 96 |
| maxComputeInvocations | 1536 | 1024 |
| maxUniformBlock | 64 KB | 4 MB |

## Instancing smoke (`RAID_GL_SMOKE`, mode 0 static)

| | GTX 1060 | RX 580 |
|---|---|---|
| Instance count | 50k–200k | 200k |
| Frame time (CPU submit + glFinish) | ~1.0–1.9 ms avg | ~7.9 ms avg |
| Per-instance cost | ~0.02 µs | ~0.04 µs |

NVIDIA ~2× per-instance throughput — expected (Pascal vs Polaris) and
partly CPU-submit-bound; not representative of gameplay load.

## Gameplay — Fraudulent dense zone (44–72s)

| Metric | GTX 1060 (vsync uncapped run) | RX 580 (vsync-locked run) |
|---|---|---|
| `frame=` EMA, dense zone | min 4.2 / med 6.1 / max 7.9 ms | 16.6 ms constant (vsync cap) |
| Effective FPS, dense zone | ~127–238 fps | 60 fps locked, `worst=16–17ms` |
| Peak live sprites (P+O) | 73 + 243 = 316 | ~49 + 49 = 98 |
| Judge records processed/side | thousands (playerData/oppData) | up to ~12k |
| ev/s peak | ~1,300–2,200 | ~2,182 |
| pump/conv | ≈0 ms | ≈0 ms |
| Song completion | results screen, clean exit | results screen, clean exit |

### RX 580 uncapped (user-reported, vsync off)

- ~400+ fps typical, **350+ fps in dense sections**
- ~40% GPU utilization — GPU is not the bottleneck; frame time is
  CPU-bound (judge + stream + per-frame batch rebuild)

## Interpretation

- **Correctness: equal.** Both vendors pass every capability check,
  compile `shaderLevel=330`, verify pixels, and complete the chart.
  AMD's compat-profile context confirms the `#version core` removal
  was a required fix, not cosmetic.
- **The GPU is never the bottleneck.** RX 580 at 40% util / 350+fps
  dense vs GTX 1060 at ~4–8ms frames shows frame time is dominated by
  CPU-side work (RaidNoteJudge, chart stream, GL batch rebuild).
- **Laptop vs desktop confound:** the i7-7700HQ thermal-throttles; the
  Ryzen 1400 holds clocks. The FPS gap is mostly CPU, not GPU —
  same-resolution A/B would be needed for a clean GPU comparison,
  but it doesn't matter for the architecture: both GPUs are idle-ish.
- **Headroom:** ~60% GPU util headroom on the *oldest* target (Polaris)
  means Phase 5D GPU-side positioning / 5E-5F culling have room to
  grow without making the GPU the limit.

## Files

- NVIDIA probe/gameplay: `logs/probe-verify2.log`,
  `logs/raidjudge-fraud6.log`, `logs/amdcompat-probe-nvidia.log`
- AMD probe/gameplay: `raid-amd-probe.log`, `raid-amd-gameplay.log`
  (repo root, copied from RX 580 machine)
- Compatibility audit: `docs/RAID-GPU-COMPAT.md`
