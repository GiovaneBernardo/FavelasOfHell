# 0006: Materials, texture streaming and transform propagation

- Status: **Accepted** (project lead, 2026-10-08: "implement all the remaining stuff ... be careful with performance"). Completes the follow-ups listed in [ADR 0004](0004-model-and-texture-import.md).
- Related: AGENTS.md rules 2, 4, 5, 8; [ADR 0001](0001-ecs-on-flecs.md); [ADR 0003](0003-renderer-render-graph-viewports.md)

## Context

Imported models arrived as flat-colored meshes: textures were cooked but not sampled, were uncompressed, imports blocked the editor, and prefab children did not follow their root. The target machine class is an RTX 2060 / RX 6600 with 6 GB of VRAM, a 1.5 km world of dense props, and frames that must not hitch while content streams in.

## Decisions

### 1. Transforms: `Transform` is local, `GlobalTransform` is computed

`Transform` is relative to the parent (the world transform for an entity without a parent). `GlobalTransform` (3x4 rows, the GPU instance layout, plus a frame stamp) is added automatically to entities with a `Transform` and kept current by two PostUpdate systems (`RegisterTransformPropagation`, called by `RegisterRenderExtract`). The renderer extraction, picking and the editor gizmo read it; gameplay code writes `Transform`.

Cost model: nothing for what did not change. Two `ChangedTables` queries (roots, children with the parent taken from the table) find archetype tables somebody wrote to; a changed entity's world matrix is computed once and pushed down its subtree; visiting order does not matter because every recompute is followed by a recompute of everything below it, and a frame stamp skips only provably finished work. 100,000 static entities cost 0.36 ms per frame in a Debug build (`test_transform_propagation`). In-place writes through `World::get` must call the new `World::markModified<T>(entity)` (the inspector and gizmo do; the inspector compares bytes before and after so it only announces real edits). New ECS pieces: `Local<T>` system parameter (per-system state, any type), `World::markModified`.

Known limit: normals use the 3x3 part of the world matrix as is, so a non-uniform scale on a rotated parent skews lighting slightly, and the TRS composition drops shear.

### 2. Materials and bindless textures

- One structured buffer of `GpuMaterial` (80 B; index 0 = default material) and one **bindless texture table** (8192 descriptors, register space 1, unbounded array indexed with `NonUniformResourceIndex`). Instances carry a material index, so batching stays one instanced draw per mesh and class, whatever materials the instances use. Needs descriptor indexing (D3D12 resource binding tier 3: RTX 2060 / RX 6600 qualify); renderer creation fails with a clear message otherwise.
- Pixel shader: base color texture x factor x instance tint; normal map (BC5, tangent frame from the packed vertex tangent); roughness from G and metallic from B of the metal/roughness texture; occlusion from the same texture when the glTF uses one ORM image (one sample), else its own.
- Vertex: 36 bytes (position, normal, uv, tangent packed as 4 snorm bytes).
- **Draw classes** (opaque, masked, opaque double sided, masked double sided): instances are counting-sorted per (class, mesh). Opaque single-sided geometry keeps the cheapest path (depth passes without a pixel shader); masked classes add a pixel shader that tests the texture alpha in depth, shadow, pick and G-buffer passes. All 16 mesh pipeline variants are created at start-up against dummy framebuffers, because a first-use pipeline compile stalled a frame by 160 ms in Debug.
- `MeshRenderer` references mesh and material by asset id (stable across sessions); `MeshLookup`/`MaterialLookup` map them to renderer ids in the extraction system. An asset that is not loaded yet degrades gracefully: the mesh is not drawn, the material is the default, textures are white / flat-normal stand-ins.

### 3. Texture residency and streaming

- Textures upload from a queue under a per-frame byte budget (default 32 MB), fully resident before use. Descriptor slots are written once; a replaced texture (reimport) gets a new slot and the old one retires four frames later, so frames in flight never sample a changed descriptor. Materials hold texture asset ids and re-resolve their slots whenever a texture arrives or changes.
- `AssetLoader` (engine/scene): workers read, decompress, validate and convert (mesh streams by semantic, texture mips, material parameters) at low priority; the main thread hands results to the renderer within a few milliseconds per frame. Oversized textures (default over 4096) are loaded without their largest mips. At most 6 loads in flight.

### 4. Background work must not hurt the frame

- `JobPriority::Low`: low jobs run only on idle pool workers, never on a thread that is waiting for normal work (the main thread cannot get stuck in a multi-second import), and jobs submitted from inside a low job inherit the priority. At most a quarter of the workers (at least one) run low jobs at once (`JobSystem::setLowPriorityLimit`), and the OS thread priority drops to below-normal while they run (`SetCurrentThreadBackground`, platform module). `JobSystem::parallelFor` / `ParallelFor` split ranges across the pool with the caller taking part.
- Imports cook textures at most two at a time (a decoded 4K image is 64 MB) and keep stb's decode buffer instead of copying it; block compressed mips are stored without LZ4 (no gain, a full extra copy). Measured with a model of 12 4096x4096 textures (BC7/BC5, 256 MB resident) in a Debug build with the D3D12 validation layer on: the editor averages 2.6 ms per frame while it streams (same as idle) with a worst frame of about 60 ms; the import takes about 13 s in the background. With half of the workers on low jobs and no OS priority drop, frames of 100-190 ms appeared. Numbers will be better in release builds.

## Consequences

- A new engine module dependency `jobs -> platform` (thread priority) and the vendored `bc7enc` (MIT / public domain).
- Prefab format v2 (submesh slot table); old cached prefabs are rebuilt because the importer version was bumped to 2.
- Not done yet, by choice: emissive and blended transparency (needs a forward pass), mip-by-mip texture streaming (a texture is visible when its whole chain is resident), GPU-side residency feedback, a Vulkan path for the bindless table (the layout is expressed through NVRHI so it should map to descriptor indexing, but nothing was run on Vulkan), reference counting that frees unused assets.
