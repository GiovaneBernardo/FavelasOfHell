# Favelas of Hell

> In the source the game is abbreviated **FOH** (namespace `foh`, `FOH_*` macros, `foh_*` targets): see [Naming](#naming).

## How this project is made

This game is **mostly written by AI** (Claude Code), directed, reviewed and decided by a human who has real experience in game engine and graphics development. The human sets the architecture, makes the technical decisions (see the Decision Log), reviews and tests the results, and steers the AI; the AI writes most of the code, documentation and tooling. Consequences for contributors and agents: keep changes small and reviewable, explain non-obvious decisions, write tests, never silently change decisions the human has made, and say clearly what was and was not verified.

## Naming

The game's name is **Favelas of Hell**, abbreviated **FOH** in code: namespace `foh`, macros `FOH_*`, CMake targets `foh_*`, include path `foh/`. The name may still change; if it does, the identifier rename is a mechanical scripted change (it was done once already, from the earlier code name `gtabr`).

Namespace convention: engine code lives in `foh`, game code in `foh::game`, editor code in `foh::editor`, so the layering (game and editor depend on the engine, never the reverse) is visible in the code itself.

A serious attempt at an open-world, Brazil-set crime game, built on a custom C++ engine with its own renderer, ECS, asset pipeline and editor.

> Status: **pre-production / architecture phase.** Items marked **[Proposed]** are recommendations awaiting a final decision. Items marked **[Decided]** are settled. See [Decision Log](#decision-log).

## Target Hardware

| Tier | GPU | CPU / RAM | Target |
|---|---|---|---|
| Minimum | RTX 2060 / RX 6600 (6-8 GB VRAM) | 6-core class, 16 GB | 1080p, 60 fps, medium-high, upscaling allowed |
| Dev machine / "near max" | RTX 5070 Ti | i9-14900K, 64 GB | 1440p-4K, max settings, optional ray tracing |

Implications: D3D12 Feature Level 12_1 / Vulkan 1.3 as the baseline. Ray tracing is **optional** (both minimum GPUs support DXR/RT, but it is a quality toggle, never a requirement). Mesh shaders are optional too. VRAM budget (6 GB) drives texture streaming and the asset format. We profile on the minimum tier continuously, not just at the end.

## Goals

1. A playable open world that feels like Brazil (geography, cities, vegetation, light, roads, culture), as close to real as is practical.
2. A custom engine with a fast iteration loop: **game code, and ideally engine and editor code, hot reload without restarting.**
3. A modern renderer: deferred tiled shading, physically based atmosphere, volumetric clouds, probe-based GI, ray-tracing-ready foundations.
4. A best-in-class content workflow: live Blender round-tripping and in-editor browsing/downloading of online assets.

Non-goals (for now): console builds, multiplayer, dual-contouring/voxel terrain.

## Tech Stack

| Area | Choice | Notes |
|---|---|---|
| Language | C++20 | MSVC / clang-cl primary on Windows; Clang/GCC on Linux |
| Build | CMake (+ presets), vcpkg for third-party deps | Windows first, always keep Linux/macOS buildable |
| Graphics API | **[Decided] NVRHI** over D3D12 + Vulkan | See [Graphics API](#graphics-api) |
| Shaders | **[Decided] Slang** (HLSL-compatible), compiled to DXIL/SPIR-V | Hot-reloadable |
| Physics | **[Decided] Jolt Physics** | See [Physics](#physics) |
| ECS | **[Decided] Bevy-style layer on flecs** (storage/queries from flecs, API + parallel scheduler ours) | See [ECS](#ecs), [ADR 0001](docs/adr/0001-ecs-on-flecs.md) |
| Editor UI | Dear ImGui (docking), custom platform/renderer glue | See [ADR 0002](docs/adr/0002-windowing-and-input.md) |
| Windowing/input | **[Decided] SDL3**, private to `engine/platform` | [ADR 0002](docs/adr/0002-windowing-and-input.md) |
| Render orchestration | **[Decided] Render graph** with per-viewport instances | [ADR 0003](docs/adr/0003-renderer-render-graph-viewports.md) |
| Assets | Custom binary formats + importer/serializer/loader | |

## Repository Layout

```
/engine          all modules fold into ONE shared library, foh_engine.dll
  /core          types, math (glm wrappers), logging, asserts, platform macros   [exists]
  /platform      window + input events (SDL3), dynamic libraries, files, PNG     [exists]
  /jobs          job system: worker pool, fork-join, parallelFor, normal/low priority   [exists]
  /ecs           Bevy-style layer on flecs: World, App, plugins, Query/Res/Commands, parallel scheduler   [exists]
  /host          game-module ABI + hot-reload host                               [exists]
  /assets        chunked container, asset types, asset database, glTF/FBX/image importers (ADR 0004)   [exists]
  /scene         prefab instantiation, asynchronous asset streaming to the renderer, reimport sync   [exists]
  /renderer      render graph, D3D12 device/swapchain via NVRHI, scenes + viewports,
                 shader library, ECS render extraction (ADR 0003)                 [exists]
  /physics       Jolt integration (placeholder)                                  [exists]
  /world         terrain, streaming, procedural generation, roads                [planned]
/runtime         standalone player executable (hosts the game module)            [exists]
/editor          ImGui editor: dock layout, 3 render viewports, render graph panel [exists]
/game            gameplay code, built as a hot-reloadable DLL                    [exists]
/tests           doctest unit tests                                              [exists]
/cmake           toolchain (vcpkg bootstrap) and CMake helpers
/shaders         Slang sources -> DXIL/SPIR-V via slangc, hot reloadable         [exists]
/tools         asset cooker CLI, Blender addon/scripts, data pipeline scripts
/third_party   vendored deps not handled by vcpkg
/docs          design docs, ADRs
```

## Architecture

### Module separation and hot reload (top priority)

Strict layering; dependencies only point downward:

```
game  ──►  engine API  ◄──  editor
              ▲
         engine core
```

- **Engine** is a set of libraries exposing a narrow, stable interface. It never includes editor or game headers.
- **Editor** is a separate executable/module that hosts the engine. Nothing in the engine may depend on the editor.
- **Game** is a dynamic library (`.dll`/`.so`) loaded by the host through a small C-style entry-point table (`game_init`, `game_register_plugins`, `game_shutdown`). The host watches the file, unloads it, and reloads the new build.
- **Target experience: edit code, save, see the change in the running game in ~1-3 seconds, with world state preserved.** This is a hard requirement, not a nice-to-have. How we get there:
  - A **build watcher** (editor/CLI) rebuilds only the changed game module on save: Ninja + clang-cl/lld (or MSVC with `/INCREMENTAL`), PCH, no unity builds in game code, and the game split into several small DLLs (per feature plugin) so links stay short.
  - Reloaded DLLs are copied to uniquely named files (and PDBs) so the debugger and the original build output are never locked.
  - On reload: pause → serialize world via reflection → unload → load → re-register plugins/systems → restore world → resume. Layout changes to components go through reflection-based migration; failures fall back to defaults for the changed fields and log loudly, never crash.
  - **Optional accelerator:** [Live++](https://liveplusplus.tech/) patches function bodies in place for sub-second edits. It is **commercial, not free**: per-seat subscription (search results suggest roughly US$12-20/month; confirm on their site, and check for a free trial before buying). It is Windows-only and does not replace the DLL path. The DLL path is the baseline because it is free, portable, and works with the ECS model; Live++ is a convenience we can add if the trial convinces us.
  - Hot reload is part of CI-style testing: an automated test must reload a game module mid-run and verify world state survives.
- Hot reload rules that make this work:
  - **All game state lives in the ECS world and resources**, not in globals/statics inside the game module.
  - Components, systems and resources register with **stable IDs/names via reflection**, so the world can be serialized before unload and restored after reload (struct layout changes handled via reflection-driven migration).
  - No raw pointers/vtables from the game module may be stored in engine-owned memory across a reload. Systems are re-registered on reload.
  - Allocation across the boundary goes through engine allocators.
- **Engine and editor hot reload** is a stretch goal: achieve it by splitting the engine into DLL modules behind interface tables (renderer, assets, physics, etc.), starting with the renderer (shader hot reload comes first) and the editor panels.
- Shaders, materials, textures, meshes and Blender files hot reload independently via the asset system.

### ECS

Bevy-style API on top of **[flecs](https://github.com/SanderMertens/flecs)** (MIT) for storage and queries; see [ADR 0001](docs/adr/0001-ecs-on-flecs.md) for the reasoning and the threading model. flecs is private to `engine/ecs`: no flecs header is included by any public engine header.

Implemented:

- **Entities** are generational ids. **Components** live in flecs archetype tables (or, opt-in, sparse sets); they are plain POD types identified by type name (see the rules below).
- **Systems** are plain functions with typed parameters: `Query<const T&, U&, With<V>, Without<W>, Entity, Parent, ChangedTables>`, `Res<T>`, `ResMut<T>`, `Commands`, `EventReader<T>`, `EventWriter<T>`, `Buffers<T>`/`BuffersMut<T>`, or a single `World&` for exclusive systems. Parameters declare the access set.
- **Scheduler** (ours, not the flecs pipeline): stages `Startup`, `PreUpdate`, `Update`, `PostUpdate`. Systems whose access sets do not conflict run concurrently on the job system; conflicting ones run in registration order. Explicit ordering with `after()/before()` (by function) and `inSet<S>()/afterSet<S>()/beforeSet<S>()` (by label type). A system that uses `Commands`/`EventWriter` has its effects applied before anything explicitly ordered after it (automatic sync points). Exclusive systems are barriers.
- **Deterministic frames:** `Commands` and events are buffered per system and applied on the main thread in registration order, so results do not depend on thread timing. `cmd.spawn()` returns a real, immediately usable `Entity` (ids are reserved per queue).
- **Data parallelism:** `Query::parEach(f)` splits a query over all job threads.
- **Reflection:** `FOH_REFLECT(Type, a, b, c)` gives a data-only schema (names, kinds, offsets). Everything built on it is reload-safe: changing a component's layout (add, remove, widen, reorder, rename via a former name) **migrates the live data when the game module reloads**; worlds can be saved/loaded with `saveSnapshot`/`loadSnapshot` (self-describing binary, loads across layout changes, bounds-checked, never half-loads).
- **Buffers:** a component can hold `BufferHandle<T>`, a variable-length array owned by an engine pool (thread-safe, freed automatically with the component/entity, survives reloads, saved in snapshots). This is the answer to "an inventory with many items" without one entity per item.
- **Relationships:** parent/child with cascade delete (`setParent`, `Parent` query term), and custom pair relationships (`addPair<Rel>`, optional `cascadeDelete<Rel>`).
- **Observers:** `app.onAdd/onSet/onRemove<T>(fn(Trigger<T>, ...params))`, owner-tagged like systems, with a per-frame call limit against runaway loops.
- **Sparse components:** `static constexpr bool kSparse = true;` on a type stores it outside the archetype, so toggling it never moves the entity (markers and state flags).
- **Plugins:** `Plugin::build(App&)` registers components, resources, events, systems and observers; everything is owner-tagged and removed before a DLL unloads.

Not implemented: exact per-entity change detection (`ChangedTables` is table-level and conservative; per-entity needs a `Mut<T>` wrapper), optional query terms, relationship-ordered iteration (cascade), world-backed `Local<T>`, field-level snapshot of engine-private state. See the ADR.

Rules for components (enforced by `static_assert`): trivially copyable and trivially destructible, no owning members (`std::vector`, `std::string`, pointers into a game module). This is what keeps function pointers out of the world and makes game hot reload safe. Variable-length data goes in a `BufferHandle<T>`; text in a `char[N]`.

### Graphics API

Options considered:

| Option | Pros | Cons |
|---|---|---|
| Raw Vulkan | Max control, runs on Windows/Linux/Android | Huge boilerplate, no D3D12 path |
| Raw D3D12 | Best Windows tooling (PIX), Xbox path | Windows/Xbox only |
| **NVRHI** | One API over D3D12 + Vulkan (+D3D11), thin, resource state tracking, bindless, ray tracing support, battle-tested (NVIDIA Donut/RTXGI) | Not a render graph (we build that), console backends require our own work under NDA |

**[Decided] NVRHI.** It removes most of the barrier/descriptor boilerplate while staying thin, keeps D3D12 (PIX) and Vulkan (Linux, RenderDoc) both available, and exposes ray tracing for later. Consoles need platform-specific backends regardless of choice, so raw D3D12 buys no real advantage there. We write the render graph on top of it.

### Renderer

Phase 1 pipeline:

- Tiled deferred shading (G-buffer + compute light culling), forward pass for transparents/particles
- HDR + tonemapping, Bloom
- GTAO, Screen-space reflections
- Cascaded shadow maps
- Atmosphere: precomputed (Bruneton) scattering for day/night cycle, volumetric clouds
- Probe-based GI (irradiance probes + reflection probes)

Foundations for later: ray tracing (acceleration structure management, BLAS/TLAS from the start in the mesh/asset layer, bindless resources, a visibility/GPU-driven path), temporal upsampling, GPU-driven culling.

**Render graph:** **[Decided, implemented]** declarative passes with culling, pooled transient resources, automatic barriers via NVRHI, parallel command list recording, and **per-viewport graph instances** (editor scene view, game view, a debug scene's view; later material preview, asset thumbnails). Design, conventions (reverse-Z, sRGB) and the open follow-ups (Vulkan backend, heap aliasing, async compute, GPU-driven culling) are in [ADR 0003](docs/adr/0003-renderer-render-graph-viewports.md).

### Physics

**[Decided] Jolt Physics** over PhysX. Reasons: MIT license and trivial CMake integration, excellent multi-core performance, deterministic option, solid character controller and **wheeled/tracked vehicle** support (key for a GTA-style game), used in shipped AAA titles. PhysX 5 is viable but heavier to build, its vehicle SDK is awkward, and GPU features are NVIDIA-only. The physics layer sits behind an engine interface so it remains swappable.

### World

**[Decided] Scope for the first game: a slice of Rio de Janeiro built around a well-known favela, near 1:1 and highly relatable to the real place, with sensible compromises** (compressed distances between far-apart points, simplified interiors, fewer total buildings than reality, fictional names). Full Brazil is a long-term vision, not a v1 target.

**[Decided] Starting area: Vidigal.** A hillside favela on the slope below Morro Dois Irmãos, facing the sea, naturally isolated from other districts. Its only road connection is the Avenida Niemeyer coastal highway toward Leblon, which is a natural later expansion (second map chunk / streaming test) and a great set piece.

- **Initial map bounds (top-down view): about 1500 m wide (east-west, along the coast) by about 500 m deep (north-south, from the hillside toward the sea).** That is roughly 0.75 km². The box covers the **entire Vidigal favela** (from the Dois Irmãos / Hostel Ninho da Águia end in the west to Praia do Vidigal and the Sheraton area in the east), a stretch of Avenida Niemeyer, the rocky coast, and **some open water**, plus hillside forest above. The 500 m is a ground-plan dimension on the map, not elevation. Terrain height still rises steeply toward the Dois Irmãos ridge; the exact height range comes from the elevation data. Distant mountains and sea beyond the playable edge are non-playable backdrop. The exact crop and orientation are tuned later against the elevation data.
- **Art direction: realistic.** Realistic lighting and 3D models; not a perfect 1:1, but it must give the *feel* of the place: tight streets and stairways, many power poles with heavy tangles of wires, businesses (bars, shops, workshops) on the street edges, laundry, water tanks, murals, motorbike traffic, vegetation growing between buildings.

Approach: a **hybrid of real data and procedural generation**:

- **Real data as the seed** (used offline in `/tools` to generate terrain, road networks and block layouts):
  - **Elevation:** first choice is Rio's municipal open data (Instituto Pereira Passos via [Data.Rio](https://datario-pcrj.hub.arcgis.com/)), which has cartographic bases and, reportedly, LiDAR-derived terrain models (~1 m) for the Tijuca massif and Rio's hills. Availability of a downloadable DTM for the Vidigal/Dois Irmãos area still has to be confirmed. Fallback: Copernicus GLO-30 (30 m), which is too coarse for steep favela slopes on its own, so the terrain gets hand-sculpted detail and rock/cliff meshes on top.
  - **Roads and buildings:** OpenStreetMap coverage in favelas is sparse (even mainstream maps show only a few roads in Vidigal), so footprints are expected to be built from orthophotos/aerial imagery, reference photos and hand layout plus the procedural generator, using real data only as a guide. Roads and stairs are largely authored.
  - **Land cover/vegetation:** MapBiomas, IBGE.
  - Verify each dataset's license before use, and do not ship imagery with unclear licensing.
- **Procedural fill, with hand-authored hero areas:** buildings, vegetation, props, traffic and pedestrians are generated from rules plus the real-data masks. Landmarks and key mission locations are hand-authored.
- **Favela-specific systems:** dense, self-built, multi-story construction with irregular footprints, narrow alleys (becos), stairways, rooftops (lajes), water tanks, cable tangles, and organic growth up the slope. This means modular building kits + a generator that stacks/extrudes them, and gameplay/AI/navigation built for verticality and tight spaces rather than road-based navmesh only.
- Heightmap terrain with streamed tiles + clipmap/LOD; caves and cliffs are placed meshes (no dual contouring).
- **Density drives renderer decisions:** expect extreme instance counts, so plan GPU-driven culling, aggressive LODs/impostors and (later) meshlet-based rendering early. GI probe placement needs to handle narrow alleys and light leaking.
- Large-world handling (floating origin or double-precision coordinates) and a streaming system designed in from day one.
- **Respect and representation:** favelas are real communities. Depict them with care and variety (daily life, culture, music, commerce, not only crime), use fictional names for factions, people and brands, and avoid real events or identifiable individuals.

### Assets

Pipeline: **Importer → Serializer → Loader**.

1. **Importers** convert external formats (png, jpg, hdr, exr, fbx, gltf/glb, **.blend**, wav/ogg) into engine formats. Run by the editor, the CLI cooker, and the file watcher.
2. **[Decided] Engine formats:** one shared **chunked container** (RIFF/glTF-style) used by every asset type, with no third-party serialization dependency.
   - **Header:** magic, container version, asset type ID + type version, GUID, flags, chunk table (offset, size, codec, hash). Chunks are 16/64-byte aligned so they can be memory-mapped and uploaded to the GPU directly.
   - **Metadata chunk:** small structured data (material parameters, import settings, dependencies, license/attribution, source `.blend`/object info) written through the **engine reflection system**. Fields are identified by stable names/IDs, so fields can be added, removed or renamed (with alias) and old files load via explicit per-type migration functions.
   - **Bulk-data chunks:** described by a **descriptor** (named, typed, ordered streams with format, stride, offset and semantic, exactly like a vertex layout). Meshes (vertex streams, index buffers, meshlets, BLAS-friendly data, LODs), textures (GPU-ready BC1-BC7 mip chains, tail mips packed for streaming), animation, audio, terrain tiles. Readers go through the descriptor, so new or reordered attributes do not break old files; unknown streams are skipped.
   - **Compression:** per-chunk codec (none / LZ4 for fast loads / Zstd for storage); GPU-ready texture data stays block-compressed.
   - **Streaming-aware:** mip and LOD chunks are independently addressable so textures and meshes stream by priority under the 6 GB VRAM budget.
   - **Safety:** hashes per chunk, strict bounds validation on load (files are untrusted once mods or online downloads exist), fuzz-tested loaders.
   - Two forms of each asset: **cooked** (runtime, per-platform) and **editor/intermediate** (lossless, inspectable). Rule: the container never changes silently; any layout change bumps a version and ships a migrator.
3. **Asset database:** stable GUIDs, source-to-artifact mapping, dependency graph, content-hash-based cache of cooked artifacts, per-platform variants.
4. **Loader/manager:** async loading on the job system, handles with generation counters, ref counting, streaming priorities, GPU upload queue, **hot reload that swaps assets in place** and notifies dependents.
5. **Blender integration:** the importer shells out to headless Blender to export glTF-equivalent data from `.blend` (we do not parse `.blend` directly). A file watcher triggers re-import on save, so edits show up live. Imported objects store source `.blend` path + object name. In-editor picking + hotkey runs `blender file.blend --python <select_object.py>` to open the file with that object selected.
6. **Online asset browser (editor window):** search + one-click download + auto-import from Sketchfab (OAuth, glTF download API) and CC0 material sources (Poly Haven, ambientCG). License and attribution metadata are stored with every imported asset and surfaced in the editor.

### Editor

Dear ImGui (docking + multi-viewport), custom theme and font stack. Panels: scene view(s), hierarchy, inspector (reflection-driven), asset browser, online asset browser, profiler, render graph debugger, console. The editor drives the same engine the game uses; play-in-editor reloads the game module.

## Roadmap (rough)

1. **Foundation:** CMake/presets, core lib, logging, job system, reflection, windowing, input.
2. **ECS + hot reload spike:** world, queries, schedules, plugins; game DLL reload with world round-trip.
3. **Renderer bootstrap:** NVRHI, render graph, triangle → mesh → PBR forward, shader hot reload, multi-viewport.
4. **Editor skeleton:** ImGui, viewports, hierarchy/inspector.
5. **Asset pipeline v1:** formats, importer (glTF, textures), async loader, hot reload.
6. **Deferred renderer:** tiled deferred, CSM, HDR/bloom, GTAO, SSR.
7. **Atmosphere + clouds, probe GI.**
8. **Blender + Sketchfab integration.**
9. **Terrain + world streaming, real-data pipeline.**
10. **Physics, vehicles, characters, then gameplay vertical slice.**

## Building

### Visual Studio (recommended on Windows)

1. `File > Open > Folder...` and pick the repo root (do not generate a .sln).
2. Pick the **Windows x64 Debug** configuration. Visual Studio reads `CMakePresets.json` and configures automatically; **saving any `CMakeLists.txt` re-runs CMake** and you just build/run.
3. Choose `foh_editor.exe` or `foh_runtime.exe` as the startup item and press F5. Building either also builds the game module.

The first configure bootstraps vcpkg and builds all dependencies (about 10 minutes, once; later configures are fast). Requirements: Visual Studio 2022+ with the *Desktop development with C++* workload, Git, and the **Vulkan SDK** (provides `slangc`, which compiles the shaders; without it CMake warns and the renderer cannot start). A GPU with D3D12 and shader model 6.2 is required (the minimum spec RTX 2060 / RX 6600 qualifies).

### Command line

```
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

(Run from a "x64 Native Tools" / Developer prompt so `cl` and Ninja are on PATH.) Output goes to `out/build/<preset>/bin`.

### Hot reload loop

Run `foh_runtime.exe` or `foh_editor.exe` (F5 or Ctrl+F5 in Visual Studio), edit and **save** any file in `game/`, and that is it: the host's dev builder (`engine/host/dev_builder.*`, Debug/RelWithDebInfo only) notices the save, rebuilds just the `foh_game` target in a child process, and the host reloads the new DLL, about 2-4 seconds in total. Build errors are printed to the game log (full log in `%TEMP%/foh_game_build.log`). The host loads a shadow copy from `.hotreload/`, so the build output is never locked.

**With the debugger attached:** it works the same way. Visual Studio disables its own Build command (Ctrl+B) during a debug session, which is exactly why the build is triggered by the host and not by the IDE. The PDB is copied next to each DLL copy so breakpoints re-bind after every reload. The game module is built into `bin/modules/` (not next to the executables) so the debugger cannot pick up and lock the build's own PDB, which would make every rebuild fail with LNK1201. Limits: only game code reloads; changing engine code still requires stopping the session (the engine DLL is locked while running, rebuild it with Ctrl+B after stopping).

We deliberately compile with `/Zi`, not `/ZI`: Edit and Continue keeps the compiler databases locked while debugging, and Visual Studio's own Hot Reload button does not work with shadow-copied DLLs (it reports NEN1004). Ignore or disable that button (*Tools > Options > Debugging > .NET/C++ Hot Reload*); our reload replaces it.

### Editor, runtime and shader hot reload

The editor (`foh_editor`) opens with three **viewports**: *Scene* (the main world through a free-fly editor camera: hold RMB over the view, WASD, Q/E down/up, mouse wheel = speed, Shift = fast), *Game* (the same world through the camera entity the game defines) and *Animation Debug* (a second, independent world with its own camera). Each viewport's overlay can retarget it to any scene and toggle the editor camera and grid; the *Render Graph* and *Stats* panels show passes (culled/recorded, command list group), transient reuse, barriers and frame time. The layout is saved to `editor_layout.ini` next to the executable (*Settings > Reset layout* restores the default).

Saving a `.slang` file under `shaders/` rebuilds the shader binaries (`foh_shaders`, about 1.5 s) and the running editor/runtime picks them up without a restart, exactly like the game code loop above. A shader that fails to compile keeps the previous version running; the compiler output is in `%TEMP%/foh_foh_shaders_build.log`.

Smoke/QA flags (both executables): `--frames N` renders N frames and exits with code 2 if the graphics validation layer reported anything; `--screenshot out.png` saves the last frame. The editor also takes `--stress` (scripted resizes, scene switches, panel toggles), `--group-passes N`, `--no-vsync` and `--no-validation`.

### Dependencies (vcpkg manifest, `vcpkg.json`)

flecs (MIT; ECS storage and queries, private to `engine/ecs`), glm, Jolt Physics, NVRHI (D3D12 + Vulkan), SDL3, zstd, lz4, cgltf (MIT, glTF parser) and stb (public domain / MIT, `stb_image` decoding), both header-only and private to `engine/assets`, Dear ImGui (docking, editor only), libcurl (HTTPS, schannel on Windows) and miniz (MIT, zip extraction) for the editor's assets workshop (ADR 0006), doctest (tests). **Vendored in `third_party/`** (not in our vcpkg baseline): ufbx (MIT / public domain, FBX loader; see `third_party/ufbx/README.md`) and bc7enc_rdo (MIT / public domain, BC7 + BC1-BC5 block encoders; `bc7decomp` is linked into the tests only; see `third_party/bc7enc/README.md`). Shaders are compiled with `slangc` from the Vulkan SDK (the vcpkg `shaders` feature, which builds Slang itself, stays available as an alternative). vcpkg is pinned to a fixed commit (`builtin-baseline`) for reproducible builds; `cmake/toolchain.cmake` uses `$VCPKG_ROOT` if set (Visual Studio sets it) or bootstraps `.vcpkg/`. Triplet: `x64-windows-static-md` (third-party static, dynamic CRT).

## Decision Log

| Decision | Status |
|---|---|
| C++20, CMake, Windows-first | Decided |
| Bevy-style ECS layer on flecs, own parallel scheduler ([ADR 0001](docs/adr/0001-ecs-on-flecs.md)); supersedes "fully custom ECS" (2026-10-08) | Decided |
| ImGui editor | Decided |
| Game code hot-reloadable via DLL; engine/editor stretch goal | Decided |
| Deferred tiled renderer + listed feature set | Decided |
| Graphics API: NVRHI | Decided |
| Shader language: Slang | Decided |
| Physics: Jolt | Decided |
| Render graph: declaration-ordered passes, culling, pooled transients, NVRHI barriers, parallel recording, one graph instance per viewport ([ADR 0003](docs/adr/0003-renderer-render-graph-viewports.md)) (2026-10-08) | Decided, implemented |
| Windowing/input: SDL3 behind `foh::Window`, engine-owned event stream, own ImGui platform glue ([ADR 0002](docs/adr/0002-windowing-and-input.md)) (2026-10-08) | Decided |
| Editor multi-viewport model: viewport = target + camera source + scene + own graph; scene = renderer snapshot of one ECS world filled by an extraction plugin (2026-10-08) | Decided |
| Rendering conventions: reverse-Z + D32F, RH Y-up, HDR RGBA16F, sRGB RGBA8 display targets | Decided |
| Header tiers in `engine/renderer`: `foh/renderer/` NVRHI-free API for game code, `foh/rhi/` the NVRHI adapter | Decided |
| Renderer backends: D3D12 done, Vulkan backend not started | Open (planned) |
| First map: Vidigal, ~1500 m x ~500 m ground plan (entire favela + some water), Av. Niemeyer to Leblon as later expansion | Decided |
| Art direction: realistic lighting and models, dense wires/poles, tight streets, street businesses | Decided |
| Asset format: chunked container + reflection metadata + descriptor-based bulk data (no FlatBuffers) | Decided |
| Model import: a source file (gltf/fbx) is an asset with GUID-addressed sub-assets (mesh, material, texture, base prefab), generated base prefab + prefab variants for edits, static geometry first ([ADR 0004](docs/adr/0004-model-and-texture-import.md)) (2026-10-08) | Decided, implemented (see ADR 0004 "Implementation status" for what is still open) |
| Editor UI: DPI-derived UI scale + system font, content browser with async thumbnails (atlas, CPU-rasterized model previews), assets workshop (ambientCG, Poly Haven, Sketchfab; curl + miniz; FreePBR excluded: no API) ([ADR 0005](docs/adr/0005-editor-ui-content-browser-workshop.md)) (2026-10-08) | Decided, implemented |
| Asset import dependencies: cgltf (glTF), ufbx (FBX, vendored), stb_image (decoding), bc7enc_rdo (BC7/BC5, vendored) (2026-10-08) | Decided |
| Transforms: `Transform` local + computed `GlobalTransform`, change-driven propagation ([ADR 0006](docs/adr/0006-materials-streaming-transforms.md)) (2026-10-08) | Decided, implemented |
| Materials: material buffer + bindless texture table, draw classes for alpha mask / double sided, textures BC7/BC5, async import + streaming under byte/time budgets, low-priority job class ([ADR 0006](docs/adr/0006-materials-streaming-transforms.md)) (2026-10-08) | Decided, implemented |
| Atmosphere: Bruneton precomputed scattering (transmittance/scattering/irradiance LUTs from compute shaders, 8.1 MiB), sun/ambient/aerial perspective from the LUTs, tables cached on disk and rebuilt from an editor button ([ADR 0007](docs/adr/0007-precomputed-atmosphere.md)) (2026-10-09) | Decided, implemented |
| Volumetric clouds: one ray-marched slab, half resolution + temporal accumulation, early-out on geometry, lit through the Bruneton tables; ~0.1-0.2 ms on a 5070 Ti ([ADR 0008](docs/adr/0008-volumetric-clouds.md)) (2026-10-09) | Decided, implemented |
| Post-processing: Call of Duty AW bloom (13-tap down chain + additive tent upsample), luminance-histogram auto exposure per viewport, selectable tonemap operators ([ADR 0010](docs/adr/0010-post-processing.md)) (2026-10-09) | Decided, implemented |
| Hybrid real-data + procedural world | Decided |
| Hardware: min RTX 2060 / RX 6600, dev RTX 5070 Ti; RT optional | Decided |
| Hot reload goal: save code, see it live in ~1-3 s with state preserved (DLL reload baseline, Live++ optional) | Decided |
| Live++ purchase | Open (paid; trial first) |
| Exact crop of the 1500 m x 500 m box, elevation data source, procedural generation scope | Open (deferred) |

## Open Questions

- Exact placement and orientation of the 1500 m x 500 m box over Vidigal, and how much sea/ridge/backdrop sits outside it. The elevation data search and the procedural-generation approach are deferred; the project owner will decide later.
- Confirm a downloadable high-resolution DTM for the area on Data.Rio (or fall back to Copernicus + hand sculpting) and the license terms of any orthophotos used as layout reference.
- Live++: try the trial, then decide.
- Legal: use fictional brands, factions and people; avoid real trademarks and identifiable real individuals. Track online-asset licenses (CC-BY attribution, no-commercial restrictions).
- Asset production plan: realistic style at this density is a lot of content, so decide the mix of modular kits, procedural generation and downloaded assets early.

## License

TBD.
