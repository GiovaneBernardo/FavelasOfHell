# 0007: Precomputed atmosphere (Bruneton)

- Status: **Accepted** (project lead, 2026-10-09). Implements the README feature "Atmosphere: precomputed (Bruneton) scattering".
- Related: [ADR 0003](0003-renderer-render-graph-viewports.md) (render graph, shaders); AGENTS.md rules 4, 5, 8
- References: Bruneton & Neyret 2008 (inria-00288758), Bruneton 2017 reference implementation (ebruneton/precomputed_atmospheric_scattering).

## Decision

The sky, the sun's color at the surface, the ambient light and the aerial perspective come from three lookup tables precomputed on the GPU with the Bruneton model (single + multiple scattering, ozone, ground bounce):

| Table | Size | Format | Content |
|---|---|---|---|
| transmittance | 256 x 64 | RGBA16F | light surviving the path to the top of the atmosphere |
| scattering | 256 x 128 x 32 (nu 8 x mu_s 32, mu 128, r 32) | RGBA16F | in-scattered radiance, Rayleigh rgb + single Mie in alpha ("combined" layout of the reference) |
| irradiance | 64 x 16 | RGBA16F | sky light reaching the ground |

8.1 MiB of GPU memory. The tables are baked for a sun of irradiance 1; the sun color of the scene (`DirectionalLight` color x intensity) scales them at runtime, so changing the sun never needs a rebuild.

### Runtime (per frame, no ray marching)
- `Sky` pass (`sky.slang`): reads the depth buffer first and shades only empty pixels (one `atmoSkyRadiance` = 2 filtered 3D fetches + 1 transmittance fetch), plus the sun disc and a lit planet surface below the horizon. The lighting pass shades the other pixels, so together they cover the target and no clear is needed.
- `Deferred` pass (`deferred.slang`): sun attenuated by `atmoTransmittanceToSun`, ambient from the irradiance table (hemisphere factor + ground bounce) instead of the old constant, then aerial perspective with `atmoSkyRadianceToPoint` (transmittance + two scattering lookups). Falls back to the old hemisphere ambient when the tables are not ready or the scene has no sun.
- Camera/world mapping: the planet center is straight below the camera; world meters become kilometers, `y` is the altitude above sea level (clamped to >= 1 m, float32 precision at r ~ 6360 km is ~0.5 m). The 1.5 km world is flat for this purpose, the atmosphere is not.
- Constants travel in `ViewConstants` (`atmosphere0..3`); `atmosphere1.y = 0` disables the atmosphere for a view (no sun, or no `Sky` flag).
- `Tonemap` now adds an interleaved-gradient-noise dither (< 1 LSB): the sky is a smooth gradient and RGBA8 output bands visibly without it.

### Generation (`engine/renderer/src/atmosphere.cpp`, `shaders/atmosphere_precompute.slang`)
- Compute shaders in the reference order: transmittance, direct irradiance, single scattering, then per extra order: scattering density, indirect irradiance, multiple scattering. 4 orders by default. Intermediate "delta" tables are RGBA32F and freed afterwards; the accumulated tables ping-pong between the final texture and a spare so no pass needs a read-modify-write UAV on a half-float format (typed UAV loads of RGBA16F are optional in D3D12).
- Heavy passes run in slices of the r axis (8 slices per dispatch) and one command list per order, so no dispatch comes near the GPU timeout on the minimum-spec card. Measured on an RTX 5070 Ti (Debug, validation on): 115-235 ms.
- Physical parameters (`AtmosphereDesc`): air density, haze (Mie), Mie anisotropy, ozone, ground albedo, scattering orders, planet/atmosphere radius. Earth defaults, RGB coefficients for ~680/550/440 nm (Hillaire's values) rather than the full spectral integration of the reference: the engine renders in RGB.

### Cache and editor
- The tables are written to `<exe dir>/cache/atmosphere.lut` (RendererDesc::cacheDirectory): header (magic, version, LUT sizes, the `AtmosphereDesc` used, byte counts) + the three tables as RGBA16F. At start-up the renderer loads it (8 MiB read + three `writeTexture`, no compute) or, if missing/stale, generates and writes it once. Bump `kCacheVersion` in `atmosphere.cpp` when the shaders change what the tables contain.
- Saving is asynchronous: a pack compute pass dumps the tables into raw buffers, an event query tells when the GPU is done, a Low-priority job writes `atmosphere.lut.tmp` and renames it (a reader never sees half a file). Nothing blocks the frame.
- Editor: View > Atmosphere. Edit the parameters, press "Generate and save to cache". The new tables switch over in queue order without a stall (the textures are created once and overwritten in place); frames already submitted keep the old data.
- The shaders hot reload like all others, so after editing `atmosphere_precompute.slang` press Generate again (and bump the cache version if the cache should not be reused by other builds).

## Consequences
- Adds 8.1 MiB VRAM, three fetches (sky) / about nine (lit pixel) per pixel of sky/geometry per frame. Nothing is computed per frame beyond that.
- Light comes from the sun only: the night has no moon or stars (a 0.002 ambient floor keeps it from being pitch black) and exposure is fixed at 1.0, so twilight below about -3 degrees renders black until auto-exposure exists.
- The ground below the horizon is a lit planet sphere (albedo `groundAlbedo`) until terrain and water render there.
- Not done: volumetric clouds, light shafts (the reference's shadow_length), sky-light occlusion by the scene (ambient is not shadowed), a moon, stars, per-view time-of-day control in the editor (rotate the sun entity for now), a sky-view LUT / half-resolution sky (not needed at the measured cost).

## Addendum (2026-10-09): render settings
`Renderer::setRenderSettings(RenderSettings)` holds the switches the editor's Render Graph panel edits: shadows on/off, cascade resolution and distance, atmosphere lookups, aerial perspective, exposure, output dither. They apply to every viewport from the next frame (`ViewConstants::params.x`, `render`, `atmosphere1.y`) and are stored in the editor prefs. Per-viewport pass switches (sky, grid) go through `ViewportFlags`. The graph itself is still declared in code (`viewport_graph.cpp`); a data-driven graph editor would need its own ADR.
