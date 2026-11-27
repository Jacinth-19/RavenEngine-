# RavenEngine — Performance Budget & the 1080p/1440p @ 60/120 FPS Plan

This document is the mathematical contract for "AAA look on low-end hardware": every
feature that costs GPU/CPU time has a **low-resource sibling**, and every target
(resolution × refresh rate × quality tier) must pass a derived budget — or the tier
is rejected for that target.

Reproduce any number below with:

```sh
python3 scripts/perf_budget.py --target 1080p --fps 60 --tier medium --gpu igpu
python3 scripts/perf_budget.py --target all --fps 60 --tier all --gpu mid
```

Constants are a *forward-rendering floor model*, calibrated to be plausible for this
engine's PBR forward path. Calibrate on your hardware with `Renderer::GetFrameStats()`
(pass timings, draw calls, triangles) — see §6.

## 1. Targets

| Target | Pixels | Frame budget @60 | Frame budget @120 |
|--------|-------:|-----------------:|------------------:|
| 1920×1080 | 2,073,600 | 16.67 ms (14.17 ms usable, 15% headroom) | 8.33 ms (7.08 ms usable) |
| 2560×1440 | 3,686,400 | 16.67 ms (14.17 ms usable) | 8.33 ms (7.08 ms usable) |

1440p costs **1.78×** the pixels of 1080p. Going 60→120 Hz halves the budget. These are
the two multipliers every tier decision must respect.

## 2. The budget equation

```
t_frame  = t_forward + t_shadow + t_upscale + t_bloom/ssao/taa/ssr
t_forward = max(t_mem, t_alu)                      # memory- or ALU-bound
t_mem     = px_internal × bytes_per_px × overdraw × msaa_rop_factor / bandwidth
t_alu     = px_internal × bytes flop_per_px × overdraw × msaa_shade_factor / (gflops × efficiency)
t_shadow  = shadow_size² × 8 / (gflops × efficiency)
t_cpu     = draw_calls × ~5 µs                     # submission cost, matters at 120 Hz
```

per-tier constants (in `scripts/perf_budget.py` and `engine/include/raven/renderer/Quality.h`):

| Constant | Low | Medium | High | Ultra |
|---|---|---|---|---|
| internal resolution scale | 0.50 | 0.667 | 1.00 | 1.00 |
| internal px @1080p target | 960×540 | 1280×720 | 1920×1080 | 1920×1080 |
| internal px @1440p target | 1280×720 | 1706×960 | 2560×1440 | 2560×1440 |
| overdraw k (forward PBR) | 1.4 | 1.8 | 2.2 | 2.4 |
| bytes/px (tex + color + depth) | 70 | 95 | 150 | 190 |
| flop/px | 25 | 45 | 70 | 90 |
| shadow map | 512² | 1024² | 2048² | 4096² |
| MSAA | off | 2× | 4× | 8× |
| draw-call cap | 150 | 300 | 600 | 1000 |
| particles | 512 | 1024 | 4096 | 8192 |
| feature gates | none | bloom | bloom+ssao+taa | bloom+ssao+taa+ssr |
| max point lights | 2 | 8 | 16 | 32 |
| physics tick / substeps | 30 Hz / 2 | 60 Hz / 4 | 60 Hz / 4 | 120 Hz / 8 |

GPU classes: `igpu` ≈ 1200 GFLOPS @ 35 GB/s (Iris Xe-class), `mid` ≈ 8 TFLOPS @ 200 GB/s
(RTX 4060/RX 7600-class), `high` ≈ 40 TFLOPS @ 700 GB/s (4090/7900 XTX-class).

## 3. Worked verdicts (this engine's forward PBR, model output)

Run `perf_budget.py --target all --fps 60 --tier all --gpu <class>`; headline numbers:

**1080p60 — the realistic "AAA on low-end" target**
- `igpu` + Medium (720p internal, 2× MSAA): ≈ **6.1 ms** GPU + 1.5 ms CPU → **PASS** (≈130+ fps vsync-off).
  This is the flagship configuration for the low-end goal: native 1080p presentation,
  upscaled internal, AAA-quality features deferred to the tier ("Medium+" profile).
- `igpu` + High (native 1080p, 2048² shadow): ≈ **40 ms → FAIL** — do not ship native on iGPUs.

**1080p120** (7.08 ms usable)
- `igpu` + Medium ≈ 6.1 ms → **PASS, tight** — reduce resolutionScale to 0.6 or drop bloom for margin.
- `mid` + High ≈ 6.9 ms → **PASS, tight**.

**1440p60**
- `igpu` + Medium (1706×960 internal): ≈ **10.8 ms → PASS** with headroom.
- `mid` + High (native): ≈ **9.9 ms → PASS**; Ultra ≈ 26 ms → FAIL.

**1440p120** (7.08 ms usable) — the hardest honest case
- `mid` + High native ≈ 12.5 ms → **FAIL**; fix = High tier with `resolutionScale ≈ 0.75`
  (1920×1080 internal → ≈ 7.0 ms) — i.e. "High-lite". This is exactly the DLSS/FSR
  Quality-class trade the engine now supports via `RendererConfig::resolutionScale`.
- `high` + Ultra native ≈ 8.2 ms → TIGHT; Ultra-lite required.

So the answer to "how will the game fare": **1080p60 and 1440p60 at "AAA look" are
comfortable on target hardware; 1080p120 is fine; 1440p120 requires upscaled internal
resolution or high-end dGPU.** Native 1440p120 with every pass on is not mathematically
possible at these budgets on anything but enthusiast silicon — the tier system exists so
the game degrades gracefully instead of dropping frames.

## 4. LOD math (the other half of the triangle budget)

A bounding sphere of radius *r* at distance *d* covers

```
h_px = (2 · atan(r / d) / fov_vertical) × viewport_height
```

Worked example (unit tests verify it): r = 0.5 m, d = 10 m, fov = 60°, viewport 1080 px

```
h = (2 · atan(0.05) / 1.0472) × 1080 ≈ 34.9 px
```

`ScreenSpaceHeight(0.5, 10, 60, 1080)` in `Quality.h` is checked against this value.
`SelectLod()` picks the deepest LOD whose threshold isn't crossed (defaults
128/64/24 px, `lodBias` scales the thresholds). Because triangle throughput on iGPUs
is ~10× lower than discrete parts, LOD1 switching at ~128 px can be the difference
between 60 and 40 fps on Iris Xe — see `LodComponent` in `Components.h` and
`tests/test_quality.cpp`.

LOD triangle-savings rule of thumb: LOD1 ≈ ½ the tris, LOD2 ≈ ¼, LOD3 ≈ ⅛. A 100k-tri
hero mesh costs 100k / 50k / 25k / 12.5k across LOD0→3 — the camera decides which tier
by pixels, not by distance.

## 5. Where the other heavy systems get their light versions

| System | Heavy path | Light sibling (tier-gated) |
|---|---|---|
| Shadows | 2048–4096² directional | 512²/1024², 30–60 m range, distance-culled shadow casters (`castShadow=false` beyond range) |
| MSAA | 4×/8× | 1×/2× (iGPUs are ROP/shading limited, MSAA hurts) |
| Textures | 4K + aniso 16 | 512/1024 max edge, aniso 1–2 (`maxTextureSize` enforced at import) |
| Lighting | 16–32 point lights/forward pass | 2–8 point lights; forward pass loops capped |
| Post | SSAO+SSR+TAA+bloom | none / bloom-only on Medium (bloom is the classic "AAA tell" for 6% cost) |
| Physics | 60–120 Hz, unlimited substeps | 30 Hz tick, ≤2–4 substeps, bodies beyond `physicsSleepDistance` frozen |
| Audio | 48–64 voices | 16 voices (audible threshold) |
| Scripts | none | instruction/frame budget watchdog (`luaInstructionBudgetMs`) |
| Particles | 4k–8k | 512–1k |

## 6. Calibration procedure (how to trust these numbers)

1. `RAVEN_BUILD_TESTS=ON cmake --build build && ./build/bin/raven_tests` — includes `quality:` tests.
2. In-game: `Renderer::GetFrameStats()` → `gpuFrameMs`, `passTimes`, `drawCalls`, `triangles`.
3. Sweep `resolutionScale` 0.5 → 1.0 in 0.1 steps at the target resolution; record
   `gpuFrameMs` per step. Fit `bytes_per_px` to the measured slope (it's linear in pixels).
4. Sweep LOD distances; verify `triangles` drops and `gpuFrameMs` follows — if not, the
   bottleneck is CPU submission (`drawCalls`), not geometry — reduce draw calls first
   (instancing, merged meshes, batched particles).
5. Keep `gpuFrameMs ≤ usable_budget × 0.8` at the target's **worst** camera angles (LOD
   threshold junctions are where frame time jumps).

## 7. Non-goals / honesty notes

- The model predicts *throughput-bound* frames. It does not model pipeline bubbles, driver
  overheads, or shader compile hitches (async pipeline creation is future work).
- `fsr`/`dlss` upscalers are not vendored; `resolutionScale` + resolve is the supported
  low-cost path, not a free quality win — expect visible aliasing artifacts below scale 0.66
  without temporal accumulation.
- Physics lightening does not change determinism of the remainder — sleep distance and
  tick rate are part of the simulation contract for saves/replays.
