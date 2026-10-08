# 0004: Model and texture import: source assets with sub-assets

- Status: **Proposed** (direction chosen by the project lead, 2026-10-08; format details and dependencies still to confirm). Builds on the README decision "Engine formats" and Assets pipeline.
- Related: AGENTS.md rules 5, 6, 7, 8; [ADR 0001](0001-ecs-on-flecs.md); [ADR 0003](0003-renderer-render-graph-viewports.md)

## Context

We need importers for textures and 3D models (glTF/GLB first, FBX second), writing our own chunked-container formats. A model file can contain many meshes, materials, textures and a node hierarchy. The question was whether it becomes one engine file, one file per part, and whether a prefab is generated.

## Decisions

### 1. A source file is an asset with sub-assets (Unity/Unreal style)

- The editor shows one asset per source file (`house.gltf`), identified by a source GUID stored in `house.gltf.meta`. Expanding it shows its **sub-assets**: meshes, materials, textures and one prefab.
- Every sub-asset is a separate engine container file with its **own GUID**, and everything references sub-assets by that GUID only. The runtime never knows about the source file or the "model" asset; it loads a mesh, a material or a prefab directly. This keeps instancing, LODs, streaming and the content-hash cache per sub-asset, and a reimport only rewrites what changed.
- Sub-asset kinds from one model import: `Mesh` (all primitives of one glTF mesh as submeshes in one vertex/index buffer set, one material slot per submesh), `Material`, `Texture`, `Prefab`.
- Embedded textures (GLB, FBX) are extracted and deduplicated by content hash like external ones.
- Static geometry only for now. The mesh descriptor stays open so skin streams, skeletons and animation clips become additional sub-asset kinds later (`Skeleton`, `AnimationClip`) without a format break.

### 2. Stable sub-asset GUIDs

The `.meta` file holds a table `sub-asset key -> GUID`. The key is the source's stable name (glTF node/mesh/material name), with the index as fallback when names are missing or duplicated. On reimport, keys that still exist keep their GUID; new keys get new GUIDs; keys that vanished are kept as tombstones for a while so a rename can be fixed in the table by hand (or by the editor) instead of breaking references. Artists never see GUIDs.

### 3. Prefab: generated base plus variants

- Import generates a **base prefab** from the source node hierarchy: transforms, mesh references, material references (reflected components, same format as saved worlds). It is owned by the importer and rewritten on every reimport; it is read-only in the editor.
- Customisation is a **prefab variant**: a separate asset that references the base prefab GUID and stores only overrides (material swaps, added/removed components, transform tweaks), addressed by node path. When the base changes, the variant reapplies; overrides whose target vanished are reported, not silently dropped.
- Import setting `generatePrefab = false` imports a mesh-set only (kit pieces: poles, stairs, props), with no hierarchy.

### 4. Import pipeline

`parse (glTF | FBX) -> ImportedScene (nodes, meshes, materials, textures, in engine conventions) -> cook -> sub-assets`. Only the parse step differs per format; units and axes are converted to engine conventions (RH, Y-up, meters) in the `ImportedScene` stage. glTF is the format we test hardest (Blender is the authoring tool); FBX is best-effort.

- Textures: importer picks sRGB vs linear and the BC format from the material slot that uses them (base color: BC7 sRGB, normal: BC5, mask/ORM: BC7 linear, single channel: BC4); overridable in the `.meta`. Mip chains are generated at cook time. A texture dropped in alone gets default settings by name suffix, editable in the `.meta`.
- Materials: reflected parameter block + texture GUIDs. The glTF metallic-roughness (+ occlusion) packing is kept as one texture instead of being split.
- Intermediate vs cooked: import writes the editor/intermediate form; the cooker writes per-platform runtime forms, as in the README.
- Loaders stay async and hot-reload aware: a reimport swaps the affected sub-assets in place and notifies dependents (rule 5).

### 5. Metadata

Every sub-asset records source path, source key, importer version, license and attribution (rule 7), and for `.blend`-sourced data the `.blend` file + object name.

## Open points (need project lead input)

- **Third-party parsers (new dependencies, need approval and license check):** glTF: cgltf (MIT, tiny) or fastgltf (MIT, faster, C++20); FBX: ufbx (public domain / MIT). Image decoding: stb_image (public domain / MIT) and BC compression: bc7enc / ISPC texture compressor or DirectXTex (MIT, Windows-leaning). Proposed default: cgltf, ufbx, stb_image, bc7enc-style compressor.
- Exact container chunk layout for `Mesh` and `Texture` (follow-up section once the first importer works).
- Where the importer cache lives (`cache/imported/<guid>.<ext>` proposed, git-ignored).

## Consequences

- More small files than a one-file-per-model design, in exchange for per-part reimport, sharing, instancing and streaming.
- The asset DB must implement source GUIDs, the sub-asset key table and the dependency graph before the first importer can ship.
