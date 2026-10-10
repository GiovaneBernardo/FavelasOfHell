# 0012: World authoring (terrain, vegetation, roads, parcels, buildings)

- Status: **Proposed** (project lead decided the points under "Decided"); terrain (real heightmap + sculpt), ground material, vegetation, collision and manual roads implemented; parcels and buildings planned.
- Related: README "World" section, [ADR 0006](0006-materials-streaming-transforms.md) (materials, streaming budgets), [ADR 0001](0001-ecs-on-flecs.md)

## Goal

Make Vidigal's look (irregular sloped streets, buildings that adapt to the road height, no two houses alike, worn pavement, wire tangles, tight spaces, a mountain covered in trees) reachable by hand and by generator, with save-and-see iteration and room to grow (Leblon, Ipanema, Copacabana, Botafogo, Rocinha, Cristo Redentor, Pao de Acucar).

## Decided (project lead, 2026-10-10)

1. Terrain is a **heightmap plus placed rock meshes** (no overhangs, no voxels).
2. Roads **carve the terrain and also get their own mesh** draped on top (both).
3. Generators use **stable seeds**; generator versions are explicit so hand edits survive upgrades.
4. First deliverable covers **terrain + spline roads + building generator**, in that order.
5. The terrain is **infinite and flat at -200 m** except around the generated/real land, finer resolution, **textures, trees and grass**.
6. Vegetation is **not entities** (GPU instanced); terrain and trees have **collision**; roads start as **manual splines**.
7. A real placeholder heightmap of the whole area (Vidigal, Rocinha, Leblon, Ipanema, Copacabana, Botafogo, Cristo, Pao de Acucar) until a higher resolution one arrives.

## Model: layers of intent

The source of truth is a small amount of authored intent; everything dense is generated and is a cache, not source.

| Layer | Authored | Generated |
|---|---|---|
| Terrain | base (real heightmap, placeholder generator as fallback) + sculpt deltas | LOD node meshes, ground material blend |
| Vegetation | seed, density, clearings | trees, palms, grass tufts per tile |
| Roads/paths | splines with width, sidewalk, grade | road + sidewalk meshes, terrain carving, vegetation clearings |
| Parcels | lots along roads, hero lots by hand | footprints snapped to the road edge |
| Buildings | recipe per lot (kit, style, storeys, seed), hand edits | stacked kit buildings in varied states |
| Surface detail | masks and decal polygons | cracks, patches, manholes, stains |
| Utilities | poles, tangle tools | wire splines with sag, drops to buildings |
| Props | scatter volumes and density paint | laundry, tanks, dishes, vehicles |

Key rule (parcels and buildings, not built yet): the lot's ground level samples the road profile, and the generator absorbs the height difference with stairs, ramps, stilts or garages. That is what makes the street read as Vidigal.

## Terrain (implemented, `engine/world`)

- **Base + sculpt.** Final height = base + sculpt. The base is a pure function: the **real heightmap** when one is loaded (`DemHeightmap`, `world/rio_dem.fohdem`), else the procedural placeholder (`TerrainGenParams`). Both are flat at `floorHeight` (-200 m) outside the land, so the world has no edge. The sculpt layer is untouched when the base changes.
- **Rio heightmap (placeholder).** `tools/fetch_rio_dem.py` downloads two Copernicus GLO-30 tiles (30 m, open data on AWS, no login; licence in `world/ATTRIBUTION.md`). `tools/build_rio_heightmap.py` stacks them, removes buildings from the low city (grey opening, 120 m window, only where the opened surface is below 90 m, so mountains and beaches keep their shape), gives the ocean and the Lagoa a sea bed (the model has none: depth falls from the shore to the floor with distance, 420 m scale, smoothed), and resamples with a cubic filter to a uniform **15 m grid** over 17.5 x 12.5 km (x -6..11.5 km, z -8..4.5 km from Vidigal at lat -22.9935, lon -43.2330; all the listed places are inside; Leblon is about 760 m north of Vidigal, i.e. negative z). Stored as int16 in 1/16 m (1.9 MB). At runtime the terrain adds procedural detail between samples (undulation from 70 m down to 8 cm grain, more on mountains, none on water, little on flats), samples bicubically so normals are smooth from a coarse grid, and fades the land to the floor over 1.5 km inside the data border. Limits of a 30 m surface model: tree canopy is in the heights, and the buildings of Vidigal and Rocinha are partly still there (above the 90 m cleaning limit). The higher resolution model replaces the file (`build_rio_heightmap.py` takes any GeoTIFF).
- **Sculpt layer** on a 0.5 m grid, stored **sparsely** in 32 m tiles (65 x 65 floats; border rows shared by neighbours are written to all copies). Brushes (raise, lower, smooth, flatten, noise, erase), `editRect` (used by roads), whole-tile stroke undo/redo (64 steps), clear-all. A reader/writer lock lets height queries and node builds run on any thread while the main thread sculpts.
- **Resolution and LOD.** A quadtree of nodes around the camera (`TerrainRenderer`): every node is 64 x 64 cells; level 0 is 32 m wide with 0.5 m cells, every level doubles both, 8 levels. A node splits while the focus is closer than `size * splitFactor`. Meshes are built on the job system (low priority), uploaded under a 6 per frame budget, swapped without holes or overlap, cracks hidden by skirts. Sculpting and parameter changes rebuild only the nodes they touch (dirty-rect cursors, one per consumer).
- **Mesh store.** Streaming needs reusable vertex/index space: `MeshStore` has a first-fit allocator with coalescing (before, replacing or releasing a mesh leaked its space).
- **Water.** The sea-floor texture is a 1024^2 window (4 m texels) that follows the camera, rebuilt on a job and patched in place where sculpting happens (a synchronous rebuild froze the editor for seconds after every stroke).
- **Collision.** The `WorldSurface` resource (`foh/world/surface.h`) gives game code the terrain height and a trunk query. The player stands on the terrain (the floor under boxes too); terrain steeper than `Player::maxSlope` (about 54 degrees) cannot be walked up (the player slides along the contour); tree and palm trunks are solid circles (`Vegetation::pushOutOfTrunks`); the third-person camera stays above the ground.
- **Persistence.** `world/terrain.fohterrain` (version 2): generator parameters + sparse sculpt tiles; saved on editor exit when changed.

## Ground material (implemented, renderer)

`MaterialParams::terrain` selects a world-space material (no UVs, so LOD levels agree): four layers (grass, soil, rock, sand) blended by slope, height and noise, albedo and normal map per layer, rock projected from three axes, wet sand, a canopy look for distant forest. `GpuMaterial` is 96 bytes. A layer whose textures are missing renders as a plain colour. Textures are ambientCG (CC0): `python tools/fetch_ambientcg.py`.

## Vegetation (implemented, `engine/world`)

- **Not entities.** Each tile of plants is a few retained `StaticInstanceSet`s in the renderer's `RenderScene` (32 bytes per instance, a bounding sphere per set) and a compact list of trunks for collision. The renderer packs the sets next to the entities' batches (same passes, same materials, shadows included) and **culls every set against each viewport's frustum and each shadow cascade's**. The world scene went from ~12k entities to ~750.
- **Content (procedural stand-ins).** Trees: a trunk mesh (flared, 10 sides, branches at LOD 0, UVs in real-world metres, tangents) with the **PBR bark** set (ambientCG Bark006: albedo + normal map, tinted brown) and a leaf mesh of ~46 clumps of crossed cards (LOD 0; 14 at LOD 1) with **crown-shaped normals** and an atlas of 1500 small leaves per twig (two twigs for variety). Palms: a leaning ringed trunk and fronds. Grass: crossed, bent tufts at 0.48 m spacing, thinning with distance, a 256^2 blade texture with 120 blades. Authored models can replace any of it.
- **Foliage lighting.** `MaterialParams::foliage`: double-sided leaves keep the same smooth normal on both sides (the old flipped back-face normal made every card flat light or dark), wrap lighting plus a backlit glow through the G-buffer's spare channel (`translucency`).
- **Placement and streaming.** Deterministic from the seed and the terrain (jittered grid per tile accepted by height, slope and noise masks; `clearings` for places that must stay free, roads add them). Tiles are generated on the job system (nearest first) and planted when ready; trees swap to a cheaper mesh with distance; sculpting replants the tiles it touches.

## Roads (implemented: manual splines)

`engine/world/roads.*` + the editor's `RoadTool` (View > Roads). A road is a uniform Catmull-Rom spline through points on the ground plane (heights come from the terrain) with width, sidewalk width, curb height and maximum grade. **Carve terrain** conforms the ground to a smoothed, grade-limited profile along the road (level across asphalt and sidewalk, blending back over 5 m) as one undoable terrain stroke (decision 2). The meshes (asphalt; curbed sidewalk with a ramp back into the ground) drape on the carved ground with ambientCG asphalt and concrete. Roads keep vegetation away, are saved to `world/roads.fohroads` (text) and are edited in the viewport (click adds a point, drag moves it, Ctrl+click deletes). Not yet: intersections and lane markings, terrain LOD forced near roads (a road can sink into coarse far terrain), road-wear decals.

## Editor

View > Terrain: sculpt with LMB (Shift inverts, Ctrl+Z/Y), generator and heightmap controls, LOD/streaming settings and stats, vegetation, ground texture size, save/reload. View > Roads. QA flags: `--terrain [0..3]` (sea, Vidigal slope, hill, Leblon beach), `--terrain-sculpt` (scripted strokes, then a 600 m flight out and back), `--roads-test`, `--profile` (phase timings, the slowest ECS systems, the worst unclamped frame of each window).

## Measured (2026-10-10, dev GPU RTX 5070 Ti, RelWithDebInfo, 1600 x 900 editor)

About 5 ms/frame in the forest at default settings (GPU-bound: foliage overdraw and shadows), about 1 ms from the air. The Debug build is slower but not frozen: the worst frame after a sculpt stroke is ~220 ms. **Not yet verified on the minimum spec** (RTX 2060 / RX 6600): expect to need foliage LOD/imposters, cheaper cascades and terrain triangle limits.

## Not done yet (known gaps)

- The heightmap at final resolution (in preparation); rock mesh placement; terrain holes for tunnels/caves.
- Distant forest is a shader look, not trees; no imposters; no wind; static sets are packed per frame on the CPU (fine to ~50k instances; a retained GPU buffer comes with the favela's density).
- Parcels, the building generator, wires, road-wear decals, intersections, props (next, in that order).

## Consequences

- The real data arrives late without redoing work: it only replaces the base file.
- Any world size works: nothing allocates per square metre; cost follows what is near the camera and what was sculpted.
- The ground material, the mesh-space allocator and the instance sets are shared infrastructure (props, buildings and roads use them).
