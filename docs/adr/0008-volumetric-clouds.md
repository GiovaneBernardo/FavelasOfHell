# 0008: Volumetric clouds

- Status: **Accepted** (project lead, 2026-10-09). Implements the README feature "volumetric clouds".
- Related: [ADR 0007](0007-precomputed-atmosphere.md) (the atmosphere tables the clouds are lit with), [ADR 0003](0003-renderer-render-graph-viewports.md) (render graph); AGENTS.md rules 4, 8
- References: Schneider, "The Real-Time Volumetric Cloudscapes of Horizon: Zero Dawn" (2015); Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (2020, multiple-scattering octaves); Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite" (2016, energy-conserving integration).

## Decision

One horizontal cloud layer (a slab between two altitudes, default 1500 m to 3300 m) is ray marched in the sky, lit by the sun through the Bruneton tables, and composited over the empty pixels of the HDR target. Cost is the first design constraint (min spec: RTX 2060, 1080p60), so everything is arranged around not marching rays:

| Trick | Effect |
|---|---|
| Half-resolution trace, one jittered ray per pixel | a quarter of the pixels |
| Temporal accumulation (history, reprojection, neighbourhood clamp) | the jitter converges over ~8 frames: low step counts look smooth |
| 2x2 depth early-out | pixels covered by geometry are never marched, so a city view pays only for the visible sky |
| Ray limited to the slab (and to 25 km) | 20-36 / 32-56 / 48-96 steps (quality low / medium / high), fewer looking up |
| Weather map first | where it says "no cloud" the 3D noise is never fetched |
| Detail noise + light march only inside cloud | the five-tap sun march runs only on samples with density |
| Noise volumes generated once | 128^3 + 32^3 + 256^2 RGBA8 = 9.4 MiB, three compute dispatches at start-up, nothing per frame |

### Passes (per viewport with the `Sky` flag and the atmosphere on; after `Deferred`, before `Grid`)
1. `CloudTrace` (`clouds.slang`, half resolution): reads depth, the atmosphere tables and the noise; writes radiance (rgb) and transmittance (a).
2. `CloudResolve` (`clouds_post.slang`): blends the trace into the viewport history with a reprojection of the middle of the slab along the same ray (`CloudConstants::prevViewProj`), the history clamped to the min/max of the 3x3 neighbourhood of the new sample (no ghost trails). Writes the other history texture of the pair.
3. `CloudComposite` (full resolution): premultiplied blend `rgb + dst * transmittance` onto the HDR color, skipping pixels with geometry (new `RgBlend::Premultiplied`).

The history pair is per viewport (half resolution RGBA16F, created on first use and on resize, imported into the graph); a viewport that was not rendered the previous frame starts over.

### Lighting
- Sun: `sunColor` times the Bruneton transmittance to the sun at the middle of the slab (reddens at dusk, goes dark at night), scattered with a dual-lobe Henyey-Greenstein phase (silver lining + soft back lobe), three "multiple scattering" octaves (scattering, extinction and anisotropy halved per octave), attenuated by a five-tap light march (30 to 480 m steps).
- `kSunScatterBoost = 2.4` calibrates the result: a thick cloud should return about E/pi of the sun irradiance, and the three octaves alone give about half of that. It is a fitted constant, not physics.
- Sky: the irradiance table (times the ambient multiplier) for the top, a ground-bounce term for the dark underside, mixed by height in the slab.
- Aerial perspective at the cloud's mean distance with the scattering tables. The in-scattering is weighted by the cloud's coverage, because the haze between the eye and the cloud is already in the sky behind it. Clouds fade out toward the 25 km limit.
- Integration is energy conserving (`T * (1 - exp(-sigma dt))`), the sun disc drawn by the sky pass is dimmed by the clouds through the transmittance.

### API and editor
- `CloudDesc` / `Renderer::setClouds` / `Renderer::clouds` (altitude, thickness, coverage, density, wind, feature size, detail, sky-light multiplier, quality). Live: no rebuild, no cache. Defaults are in `renderer.h`.
- Editor: a "Clouds" section in View > Atmosphere.
- Files: `engine/renderer/src/clouds.{h,cpp}` (noise textures, layouts, constants), `shaders/clouds*.slang`, `shaders/common/{clouds,noise}.slangh`, pass setup in `viewport_graph.cpp`.

## Measurements
RTX 5070 Ti, Debug build, 1600x900, all three passes: 0.07 ms (mostly looking up) to 0.17 ms (view full of clouds), measured by repeating the passes 30 times and timing the frames (the engine has no GPU timers yet). A scaling to an RTX 2060 at 1080p (roughly 3x slower card, 1.44x the pixels) gives about 0.3 to 0.8 ms: **an estimate, not a measurement on the minimum-spec hardware.**

## Consequences
- +9.4 MiB VRAM for the noise plus 2 x 0.25 x width x height x 8 bytes of history per cloud viewport (about 8 MiB at 1080p), and a transient half-resolution trace target.
- Not done: clouds do not shadow the ground or the lit scene (the sun is not attenuated in `deferred.slang`), no god rays, no ambient occlusion of the sky light by clouds, no second layer (cirrus), no precipitation, no moon / stars lighting at night (the clouds go black with the sky). Geometry hides clouds completely (no cloud in front of far terrain), which is right for a 1.5 km world and the layer altitude.
- The slab is flat (the atmosphere is not): fine up to the 25 km limit, would need a spherical shell for a space view.
- The sky and the clouds are lit from the same sun, so a time-of-day control moves both.
- The noise volumes are generated at start-up without a cache: `clouds_noise.slang` / `clouds_weather.slang` changes need a restart (the compute shaders are not re-run on hot reload).

## Update: weather system (2026-10-09)

The first version was one fixed noise pattern at one coverage: simplistic, thin, far away and never changing. Changes:

- **Weather map** (`clouds_weather.slang`, 256^2 RGBA16F; half float because the coverage curve amplifies it about 6x and 8 bits showed as terraces): R coverage blobs, G type noise, B column height noise, A wide clear / cloudy regions. Two independent layers (different scale, drifting in different directions) are sampled and cross-faded over minutes, so the pattern reshapes instead of only sliding.
- **Presets** (`CloudPreset`: Clear, Fair, Broken, Stratocumulus, Overcast, Storm) are templates of (coverage, type 0 stratus..1 cumulus, storminess, density). `Renderer::cloudWeather()` is what the sky shows now; it eases toward the target over `transitionSeconds`. With `autoCycle` (default on) a new preset is picked every `cycleSeconds` along a fixed pseudo-random walk, so the sky changes character by itself.
- **Per-column structure**: each column picks its own type around the global one and its own height (stratus hugs the bottom fifth of the slab, cumulus heaps up to about half, storm columns tower through most of it), with a height gradient per type. The shape volume also drifts upward ("boiling") and the detail erosion scrolls faster.
- **Thicker and closer**: default layer 900 m + 2600 m (was 1500 + 1800), denser cores (`d * 1.6`), wind 10 m/s, noise tiles shrunk so more structure is visible at once.
- **Lighting**: a diffusion tail on the sun transmittance (`max(Beer-Lambert, 0.35 / (1 + k * depth))`) and a floor on the sky light, because pure Beer-Lambert turned every thick cloud black; storms get darker undersides and a smaller tail.
- **Noise**: `perlinFbm` is stretched back to the full 0..1 range (gradient noise clusters around 0.5, so every threshold saw nearly the same value everywhere). The coverage field is re-centred in the shader so `coverage` is about the fraction of sky covered.
- Editor: preset picker, "change by itself", cycle / transition times, live readout of the current weather, coverage bias.
- Cost: one more weather-map fetch per density sample (also on the light taps). **Not re-measured** after this change; expect somewhat above the numbers in "Measurements".
- Not done: preset-specific cloud scale, cirrus layer, cloud shadows on the scene, rain.
