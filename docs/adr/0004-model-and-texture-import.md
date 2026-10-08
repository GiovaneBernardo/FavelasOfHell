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

Dependencies chosen: **cgltf** (glTF/GLB, vcpkg), **ufbx** (FBX, vendored in `third_party/ufbx` because it is not in our vcpkg baseline), **stb_image** (PNG/JPG/TGA/BMP decoding, vcpkg). No BC compressor yet (see below).

Built: `engine/assets` (container writer/reader with per-chunk hashes and bounds validation, `AssetId`, asset types and their serializers, the importers, the cook step, `AssetDb`), `engine/scene` (prefab instantiation, mesh upload, reimport sync), the editor drop handler and Assets panel, and `Renderer::uploadMeshAsset`. Tests: `tests/test_asset_import.cpp`, `tests/test_prefab_instance.cpp`.

Where the implementation differs from, or is narrower than, the text above:

- **Metadata is hand-serialized, not reflected.** The README says metadata chunks go through the engine reflection system. `engine/assets` depends only on core (reflection lives in the ECS), so each asset type has explicit, versioned `Serialize*/Deserialize*` functions (`checkHeader` is where a per-version migration branch goes). Moving to reflection means extracting the schema layer from `engine/ecs` first; propose it as its own change.
- **Prefabs are stored as a node table, not as reflected components.** Same reason. Instantiation spawns `Name` + `Transform` + `MeshRenderer` entities.
- **Textures are cooked as RGBA8** (sRGB for color, UNORM for normal/data) with a full mip chain, one chunk per mip (streamable). The BC7/BC5/BC4 formats are declared in `TexelFormat` and the descriptor but not produced yet; `stb_dxt.h` is already installed with `stb` and covers BC1-BC5, BC7 needs another encoder (bc7enc or similar).
- **The renderer does not sample textures yet.** Materials are cooked with their texture references and the editor tints instances with the material's base color, but the G-buffer pass only has a per-instance color. Texture sampling needs a material system (bindless or per-material binding sets) and is the next renderer step.
- **Transforms are baked at instantiation.** `Transform` is a flat world transform (see `renderer/components.h`), so `InstantiatePrefab` composes the node hierarchy and sets parent links only for the hierarchy view. Moving an instance's root does not move its children until transform propagation exists.
- **Sub-asset GUIDs are derived deterministically** from (source GUID, kind, key) when a key is new, in addition to being recorded in the `.meta` table, so a lost `.meta` table still reproduces the same ids as long as the source GUID survives. Renames are handled by `AssetDb::remapSubAsset`.
- **Import runs synchronously on the main thread.** A big model with large textures blocks the editor for the duration. The loader/manager (async loads, handles, ref counting, README item 4) is not built; `AssetDb::load` is the synchronous primitive it will wrap.
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
