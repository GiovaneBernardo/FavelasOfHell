# ADR 0013: Indirect light, ambient occlusion and small-scale sun shadows

Status: Decided, implemented (2026-10-10)

## Context

Under the forest canopy the image was flat: the only ambient term was the atmosphere's sky irradiance scaled by the
normal, so shade had no occlusion, no tint from the leaves above, no reflection and no contact detail. Ray tracing is not
an option on the minimum spec (RTX 2060 / RX 6600), so the answer has to be raster and screen-space techniques plus a
cheap, time-sliced measurement of the surroundings.

## Decision

Everything lives in the viewport graph; switches in `RenderSettings` (editor: Render Graph panel, prefs `render.*`).
Shared shading code is `shaders/common/lighting.slangh`, used by `deferred.slang` and `probe_light.slang`.

- **GTAO** (`gtao.slang`, passes `GtaoMain` + `GtaoBlur`): 3 slices x 6 steps per side, analytic cosine-weighted arc
  (Jimenez 2016 / XeGTAO), interleaved-gradient noise, 5x5 depth-aware blur, R8. Multiplies the diffuse ambient (with the
  Jimenez multi-bounce correction) and, through Lagarde's specular occlusion, the sky reflection.
- **Contact shadows** (same thing as "screen space shadows": one feature): a 12-step depth-buffer ray march toward the sun
  over `contactShadowLength` metres inside the deferred shader, thickness-limited, faded out beyond 30-60 m. Multiplies the
  cascade shadow term.
- **Leaf transmission**: pass `FoliageTransmit` redraws the double-sided alpha-masked casters of the shadow cascades into
  an RGBA8 atlas of the same layout, depth-tested against the shadow atlas (so only a foliage leaf that IS the first thing the
  sun hits writes; the pixel holds the light colour through that leaf). Where the shadow map says shadowed, the lighting adds
  `sun * leafColour * canopyTransmission * (1 - lit)`: green dappled shade, never behind trunks or walls. Backlit leaves
  glow with the same saturated colour. Limitation: only the first leaf layer counts (no thickness accumulation).
- **IBL**: `SkyEnv` renders the whole sky (atmosphere tables + the cloud march, a lit grey floor below the horizon) into a
  128^2 octahedral image every frame per viewport; `EnvDown` x4 blurs it into 64..8 px levels (3x3 binomial), level i standing
  for roughness i/4. The deferred pass reflects it with Karis' analytic split-sum BRDF, scaled by the specular occlusion
  and by how much of the sky the probes say the point sees. It reuses the water probe's shader file and cloud constants.
- **Light probes** (`probes.{h,cpp}`, `probe_light.slang`, `probe_sh.slang`; viewports with `ViewportFlags::Probes`): a window of
  24 x 24 cells (default 4 m) x 2 layers (1.5 m and 6 m above the ground) that follows the camera, toroidal so a probe keeps
  its texel while the window slides. The CPU refreshes `probesPerFrame` (default 2) probes per frame, new cells nearest first,
  then the oldest (no probe sooner than 120 frames). A refresh draws the scene from the probe into 6 faces of 32^2 (the
  camera's G-buffer shaders, depth written by the capture itself, culled to 800 m), lights them with the shared shading
  code and the viewer's shadow cascades, projects them on L1 spherical harmonics (4 RGBA16F texture arrays, alpha = capture
  present) and eases toward the old value. Probes that see (almost) nothing below the horizon (inside terrain, over water)
  store nothing and fall back to the atmosphere. The deferred pass samples the grid (bilinear wrap + 2-layer blend by height
  above ground), fades it at the window border and feeds the capture back into itself, so bounces accumulate over time. Probe
  heights need the terrain height field, which is the water's sea floor (`Renderer::setWaterBed`); scenes without one keep
  the sky model.
- Also fixed: `AddTube` (tree/palm trunks and branches) wound its triangles clockwise, so back-face culling showed the inside.

## Consequences

- Cost (estimate, not measured on a 2060): GTAO ~0.5-0.9 ms at 1080p, env ~0.1 ms (+ clouds march at 128^2), probes ~0.2-0.4
  ms at 2 per frame, contact shadows ~0.1-0.2 ms. All individually switchable. Full refresh of the grid takes
  1152 / 2 frames (~10 s at 60 fps).
- New persistent state: 4 small RGBA16F arrays per probe viewport; an extra 1x shadow-sized RGBA8 atlas when transmission is on.
- Not done: probe-to-probe occlusion weights (light leaks through thin walls are possible), specular from probes (reflections
  are the sky only, darkened by occlusion), GTAO bent normals, multi-layer leaf thickness, temporal filtering of GTAO,
  half-resolution GTAO for the minimum spec, a unit test of the probe scheduler (needs the renderer's private headers).

## Revision 2026-10-10 (evening): cost, canopy shade, foliage

Measured with the new per-pass GPU timers (`RenderSettings::gpuTimers`, one viewport, Render Graph panel and `--profile`):
the first version cost ~9 ms GPU on an RTX 5070 Ti in the forest, ProbeCapture alone 3.6 ms. Causes and fixes:

- **No culling of entity batches.** Terrain tiles are entities, so every pass (and each of the 12 probe faces) drew all of them.
  Meshes now carry a local bounding sphere (`MeshRange::bounds`) and every batch a world sphere from its instances
  (`prepareScene`), culled like the static sets: probe capture 27 M -> 4 M triangles, CPU recording 4.3 -> 1.4 ms.
- Grass (`StaticInstanceSet::castsShadow/inProbes = false`) no longer draws into the shadow cascades or probe captures; the
  transmission pass runs for cascades 0-1 only; probes capture 70 m around, one probe every 2nd frame
  (`probeInterval`); default shadow distance 400 -> 250 m; tree/grass tiles are 128 m / 32 m (was 64 / 16), which divides the
  draw calls of every pass (this changes where trees stand: placement hashes per tile).
- Result in Release: 9.0 -> 3.8 ms GPU, ProbeCapture 0.45 ms, ShadowCascades 1.7 ms (now the biggest).

Look:
- Canopy shade: the cascade filter is a 4x4 grid of bilinear compares in the two near cascades; foliage casters drop 28% of
  their texels in a fixed pattern (`foliageGap`, same in the shadow and the transmission pass) so a crown lets sun flecks
  through instead of casting a solid cut-out; the transmitted light is the leaf colour pulled toward a neutral warm grey;
  the probes' irradiance never goes below 35% of the open-sky light.
- Leaf and grass textures are painted at 2x and box-filtered (soft alpha edges, so the alpha test follows the filtered edge):
  leaf atlas 1024^2 (was 512^2), grass tuft 512^2 (was 256^2) with 340 thin blades, dark roots, dry blades. Leaves are
  painted from the crown's centre outward (dark inside, bright outside).
Not verified: the lead's road-with-trees scene (only the Vidigal slope forest was reviewed), min-spec GPU timings.
