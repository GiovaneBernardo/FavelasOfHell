# 0004: Model and texture import: source assets with sub-assets

- Status: **Accepted** (project lead, 2026-10-08) and implemented; the deviations and open items are in "Implementation status" below. Builds on the README decision "Engine formats" and Assets pipeline.
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

## Implementation status (2026-10-08)

Dependencies chosen: **cgltf** (glTF/GLB, vcpkg), **ufbx** (FBX, vendored in `third_party/ufbx` because it is not in our vcpkg baseline), **stb_image** (PNG/JPG/TGA/BMP decoding, vcpkg), **bc7enc_rdo** (BC7 + rgbcx BC4/BC5, vendored in `third_party/bc7enc`; its `bc7decomp` reference decoder is linked into the tests only).

Built: `engine/assets` (container writer/reader with per-chunk hashes and bounds validation, `AssetId`, asset types and their serializers, the importers, the cook step with BC compression, `AssetDb` with asynchronous imports), `engine/scene` (prefab instantiation, `AssetLoader` streaming to the renderer, reimport sync), the editor drop handler, and in the renderer the material/texture system ([ADR 0006](0006-materials-streaming-transforms.md)). Tests: `test_asset_import`, `test_texture_cook`, `test_async_import`, `test_prefab_instance`.

Where the implementation differs from, or is narrower than, the text above:

- **Metadata is hand-serialized, not reflected.** The README says metadata chunks go through the engine reflection system. `engine/assets` depends only on core (reflection lives in the ECS), so each asset type has explicit, versioned `Serialize*/Deserialize*` functions (`checkHeader` is where a per-version migration branch goes). Moving to reflection means extracting the schema layer from `engine/ecs` first; propose it as its own change.
- **Prefabs are stored as a node table, not as reflected components.** Same reason. Instantiation spawns `Name` + `Transform` + `MeshRenderer` entities.
- **Textures are block compressed:** color -> BC7 sRGB (perceptual weights), normal maps -> BC5 (xy, z rebuilt in the shader, mips renormalized), data (metal/roughness/occlusion) -> BC7 linear, with a full mip chain and one chunk per mip (streamable). A texture whose sides are not multiples of 4 (or whose size cap lands on one) is stored as RGBA8 with a warning; `compressTextures = false` in the import settings keeps RGBA8. Quality is checked in the tests by decoding the result (BC7 > 38 dB PSNR on a gradient + noise image, BC5 within 4/255). Speed: a 2048x2048 color texture with mips cooks in about 175 ms on 32 threads (Debug build).
- **Materials sample textures** (glTF metallic-roughness, normal maps, occlusion, alpha mask, double sided); see ADR 0006. **Not supported yet:** emissive (the G-buffer has no emissive target), blended transparency (blend materials are alpha-tested, there is no transparent pass), KHR_texture_transform, sampler state (everything is wrap + anisotropic 8x).
- **Transforms are hierarchical.** Prefab nodes keep their local transforms and parent links; `GlobalTransform` is computed by the propagation system (ADR 0006), so moving an instance root moves everything below it. `InstantiatePrefab` needs no database or renderer: prefab nodes carry each mesh's submesh -> material slot table (prefab format v2).
- **Sub-asset GUIDs are derived deterministically** from (source GUID, kind, key) when a key is new, in addition to being recorded in the `.meta` table, so a lost `.meta` table still reproduces the same ids as long as the source GUID survives. Renames are handled by `AssetDb::remapSubAsset`.
- **Imports are asynchronous.** `AssetDb::beginImport` returns at once; one import runs at a time on the job system at low priority (parse, parallel decode/mips/BC, cache writes) against a snapshot of the source record, and the main thread commits the result in `pollImports()`. `importFile`/`reimportChanged` are the blocking forms for tools and tests. Streaming assets from the cache to the GPU is `AssetLoader` (async, a few ms of main-thread work per frame at most). Still missing from README item 4: handles with generation counters and reference counting (the loader tracks state per asset id and releases on removal, but nothing frees assets that are merely unused).
- **Skinning/animation, KHR_texture_transform, glTF sampler state, Draco/meshopt/basisu, EXR/HDR images** are not imported (static geometry only, as decided). Required unsupported glTF extensions fail the import with a clear error; other cases produce warnings.
- **FBX**: textures are looked up next to the model (relative name, bare file name); FBX materials map onto the PBR fields ufbx derives. Less tested than glTF (one ASCII FBX in the tests).
- **Dropped files are copied** to `assets/imported/<name>/` (git-ignored for now; move curated assets elsewhere under `assets/`). Cooked output goes to `cache/imported/` (git-ignored).

## Open points

- Container chunk layout details for `Mesh` and `Texture` beyond what is implemented (meshlets, LODs, BLAS-friendly data) arrive with the features that need them.
- Prefab variant authoring UI (variants can be created and saved through `AssetDb::saveVariant`; there is no editor for them yet).
- Where curated source assets live (`assets/` layout, Git LFS for large sources) is still to be decided.

## Consequences

- More small files than a one-file-per-model design, in exchange for per-part reimport, sharing, instancing and streaming.
- The asset DB must implement source GUIDs, the sub-asset key table and the dependency graph before the first importer can ship.
