# ADR 0010: Post-processing (bloom, auto exposure, tonemap operators)

Status: Decided, implemented (2026-10-09)

## Context

The viewport graph ended with a single tonemap pass: fixed ACES, a manual exposure multiplier, no bloom. Outdoor scenes
span many stops (sun-lit streets to shaded alleys), so a fixed exposure cannot look right everywhere, and highlights had
no glow.

## Decision

All of it is render-graph passes between `Grid`/`PickIds` and the viewport output; settings live in `RenderSettings`
(editor: Render Graph panel, saved in the editor prefs). Code: `engine/renderer/src/post_process.{h,cpp}`
(layouts, sampler, `PostConstants`), pass setup in `viewport_graph.cpp`, shaders `bloom.slang`, `exposure.slang`,
`tonemap.slang`, `common/post.slangh`.

**Bloom** follows Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014):
`BloomDown` x N halves the target each step with the 13-tap filter (first step weights its five tap groups by
`1 / (1 + luma)`, the Karis average, so a single glint cannot flicker through the chain); `BloomUp` x (N - 1) walks back
with a 3x3 tent filter and *adds* onto the next larger level (blend One/One, `RgBlend::Additive`). The tonemap lerps the
scene toward the largest level by `bloomStrength` (energy conserving, no threshold: every pixel contributes). Levels are
R11G11B10_FLOAT; the first is half the scene resolution. 1..8 levels, clamped so the smallest keeps at least 4 texels.

**Auto exposure** is a luminance histogram: `ExposureClear` (a copy of zeros) -> `ExposureHistogram` (one thread per 2x2
block, one bilinear tap, 256 log2-luminance bins, optional center weighting) -> `ExposureAverage` (one thread: mean of the
bins between the 10th and 90th percentile, target = middle grey 0.18 / average x 2^compensation, exponential adaptation in
EV space, faster toward a brighter scene than a darker one). The adapted value is a 16-byte per-viewport buffer
(`Viewport::exposureState`) read by the tonemap pass, so every viewport adapts on its own; a viewport that was not drawn
the previous frame snaps to its target. The three passes use distinct resource states so the barriers are real state
changes rather than UAV-to-UAV hazards. Off: the tonemap uses the manual `exposure`.

**Tonemap operators** (`Tonemapper`): Linear (clamp), Reinhard, Reinhard extended (luminance + white point), Uncharted 2,
ACES (Narkowicz fit, the previous behavior and still the default), Khronos PBR Neutral.

## Consequences

- Cost (estimate, not measured on a 2060): bloom ~0.1-0.2 ms at 1080p; histogram reads the HDR image once at half
  resolution. Both are off-switchable; with bloom off there is no chain and the tonemap binds the scene in the bloom slot.
- Defaults changed the look: bloom and auto exposure are ON by default (a viewport without prefs now adapts instead of
  using exposure 1.0).
- The measured luminance and adapted EV are written to the exposure buffer but not read back to the CPU; an editor
  readout would need a readback.
- Not done: lens dirt / starburst, bloom threshold, spot / matrix metering, exposure-aware Karis weights, AgX.
