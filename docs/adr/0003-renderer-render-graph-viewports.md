# 0003: Renderer architecture: RHI tier, render graph, scenes and viewports

- Status: **Accepted** (project lead, 2026-10-08). Implements the README decisions "Graphics API: NVRHI", "Render graph" and "Shader language: Slang".
- Related: AGENTS.md rules 2, 4, 5, 8; [ADR 0001](0001-ecs-on-flecs.md); [ADR 0002](0002-windowing-and-input.md)

## Context

The first renderer/editor increment has to be robust, fast and multithread friendly, and it must be structured for several simultaneous views in the editor: an editor-camera scene view, a game-camera view, and a view of a different world (a scene for debugging animations). The renderer must not depend on ECS internals, and game modules (hot reloadable) must not see NVRHI.

## Decisions

### 1. Two header tiers in `engine/renderer`

| Tier | Directory | Includes NVRHI | Who includes it |
|---|---|---|---|
| API | `foh/renderer/` | no (forward declarations only) | anyone, including game modules |
| RHI (the NVRHI adapter) | `foh/rhi/` | yes, on purpose | renderer internals, the editor, the runtime, tests |

AGENTS.md rule "public engine headers must not leak third-party headers unless the module is explicitly the adapter" is satisfied by the split. Consumers of the RHI tier link `nvrhi` themselves (for its headers); the engine DLL keeps its own private copy. Nothing in `foh/rhi/` is D3D12 specific: all D3D12 code is inside `src/rhi/graphics_device.cpp` and `swapchain.cpp` behind `FOH_PLATFORM_WINDOWS`.

### 2. Device and swapchain

`GraphicsDevice` creates the D3D12 device and wraps it with NVRHI (plus the NVRHI validation layer and the D3D12 debug layer when `validation` is on, default in debug builds). Debug layer messages are routed into the engine log and an error counter (`errorCount()`), so automated runs fail on them; the debugger breaks on them only when attached. `beginFrame/endFrame` implement frame pacing (2 frames in flight) with NVRHI event queries.

`Swapchain` is one flip-model DXGI swapchain per OS window (RGBA8, tearing support when vsync is off). It signals its own fence after every `Present` and waits on it before resizing or releasing the buffers, because NVRHI's `waitForIdle` does not cover presentation work (found by the validation layer). The Vulkan backend is **not implemented** (the API and file layout leave room for it; `GraphicsBackend::Vulkan` returns an error).

### 3. The render graph (`foh/rhi/render_graph.h`)

One `RenderGraph` instance per render target chain: one per viewport and one per OS window. Per frame: `reset`, declare (`importTexture`, `addPass`), `compile`, `record`, `submit`.

- **Declaration order is execution order.** The graph never reorders passes; it adds culling, resource management, barriers and parallel recording on top. This keeps behavior predictable and debuggable.
- **Passes** are `addPass<Data>(name, setup, exec)`. `setup(RgPassBuilder&, Data&)` runs immediately and declares every resource the pass touches and how (`RgAccess`: sampled read, UAV, color/depth target, copy, vertex/index/constant/indirect). `exec(const Data&, RgPassContext&)` runs later, possibly on a worker thread. Data and the exec callable live in a per-frame arena (static_assert: trivially destructible), so there is no per-frame heap churn.
- **Culling:** walking backwards, a pass is kept if it is `NeverCull` or writes a resource somebody still needs (imported resources always count as needed). A live pass needs everything it touches. Dead chains disappear together with the transients only they used.
- **Transients** are described by value (`RgTextureDesc`, `RgBufferDesc`) and backed by a pool that persists across frames; a transient takes a pooled resource of equal shape whose previous user is finished, so the second of two lifetime-disjoint resources reuses the first one's memory. A warm graph creates nothing. Unused pool entries retire after 4 frames. (This is *reuse*, not heap *aliasing* of differently shaped resources: see follow-ups.)
- **Barriers** come from the declared accesses. The graph simulates resource states across the whole graph and issues them through NVRHI's explicit state API (`setTextureState`/`setBufferState`), calling `beginTracking...` with the simulated state at the start of each command list. Imported resources are transitioned to their requested final state at the end (the swapchain buffer back to Present, viewport images to ShaderResource).
- **Parallel recording:** the live passes are split into contiguous groups, one NVRHI command list each, recorded as jobs on the job system and submitted in order. A graph with at most `minPassesPerGroup` (default 4) live passes, or without job workers, is one group and pays no job overhead; the shipped viewport graphs are that small, so today's parallelism is *across* viewports (and across scenes during packing), and the split inside a graph is for the bigger graphs to come. `RendererDesc::minPassesPerGroup = 1` (editor flag `--group-passes 1`) forces one command list per pass, which is how the cross-command-list barrier path is exercised. `record()` of different graphs may run concurrently, which is how several viewports render in parallel; `submit()` is main thread only and happens in viewport creation order, so GPU order is deterministic.
- **Caches** (per graph): framebuffers, and binding sets (one cache per recording group, so the pass-body API `ctx.bindingSet(desc, layout)` is lock free). Entries touching imported textures never outlive the frame (the owner may resize/destroy them); everything else is evicted after 3 unused frames.
- **Debugger data:** `passInfos()` and `stats()` feed the editor's Render Graph panel (passes, culled or not, group, physical vs virtual transients, barrier count).

Everything that does not need a GPU is covered by `tests/test_render_graph.cpp` (culling, transitive culling, lifetime reuse, pool persistence and retirement, barrier planning and final states, grouping).

### 4. Scenes, extraction and viewports (`foh/renderer/`)

- **Scene = the renderer's copy of one ECS world's render data** (`RenderScene`: extracted meshes, cameras, sun), double buffered (`write()`/`publish()`/`read()`). A world becomes renderable by adding `RenderExtractPlugin` (components `Transform`, `MeshRenderer`, `Camera`, `DirectionalLight`; resource `RenderSceneLink`; one PostUpdate system). Extraction copies data with a parallel query (`parEach`, one staging vector per job thread, then a concatenation); the renderer never touches ECS storage and the ECS never sees renderer types. The double buffer is also the seam for a future dedicated render thread.
- **Viewport = render target + camera source + scene reference + its own render graph.** The camera is either an explicit override (editor cameras are editor state, not entities) or a scene camera (a specific entity or the first active one). Any number of viewports may exist; several may show the same scene, each may show a different one, and `setViewportScene` can retarget at runtime. Viewports are created, resized (lazily, applied at the next `beginFrame`, old target kept alive one more frame because the UI still holds its pointer), deactivated (hidden panels cost nothing) and destroyed independently.
- The shipped viewport graph: `UploadInstances` (copy) -> `Forward` (sky + instanced meshes, HDR + depth) -> `Grid` (editor, depth-occluded) -> `Tonemap` (to RGBA8 sRGB). Instances are packed once per scene per frame (counting sort by mesh) and shared by every viewport of that scene.

### 5. Conventions

Right-handed, Y up, **reverse-Z with a 32-bit float depth buffer** (near = 1, far = 0, `GreaterOrEqual`), column-major matrices (shaders compile with `-matrix-layout-column-major`), HDR scene color RGBA16F, display targets RGBA8 UNORM holding sRGB-encoded values (ImGui blends in display space). Front faces are counter-clockwise.

### 6. Shaders

Slang sources in `/shaders`, compiled by `slangc` at build time to one DXIL (D3D12) or SPIR-V file per entry point in `<bin>/shaders` (`shaders/CMakeLists.txt`; slangc comes from the Vulkan SDK or PATH). `ShaderLibrary` loads them by `<file>.<entry>` and **hot reloads** them: in Debug/RelWithDebInfo the host's `DevBuilder` also watches `shaders/` and runs the `foh_shaders` target on save (measured 1.5 s save-to-rebuilt), then `ShaderLibrary::pollReload` swaps changed binaries (after the file stops changing) and bumps a per-shader uid that is part of every pipeline cache key, so pipelines rebuild by themselves. A shader that fails to compile or load keeps the last good version. `ViewConstants` is mirrored between `shaders/common/view.slangh` and `src/gpu_structs.h` with static_asserts on the C++ side.

## Consequences

- (+) Several independent views come for free from the structure; the editor's three default panels (Scene, Game, Animation Debug) are exactly three viewports over two worlds.
- (+) Game modules include only `foh/renderer/` headers; hot reload is unaffected (the extraction system lives in the engine and is not unloaded).
- (+) The render graph is testable without a GPU, and the GPU paths were run under the D3D12 validation layer with zero API errors: a scripted stress run (window resizes, viewport recreation, scene switching, panel toggling), once with the default grouping and once with one command list per pass.
- (-) `Transform` currently lives in the renderer module as a flat local-to-world component; it moves to a scene module with the ECS relationship work.
- (-) `RenderSceneLink` holds a pointer, so it is the one engine component that cannot be reflected/saved (the ECS currently warns about it at startup).

## Not done yet (planned follow-ups, roughly in this order)

1. Vulkan backend: device/swapchain creation, SPIR-V shader flags (`-fvk-*-shift` for NVRHI's register layout), validation of the Vulkan initial-layout assumptions in the state simulation.
2. Memory aliasing of transients across *different* shapes via NVRHI heaps (the pool only reuses equal shapes), and one transient pool shared by all viewports.
3. Sub-resource granularity in the graph (mips/array slices); today every access is whole-resource.
4. Async compute passes (the pass flags have room for it; queue sync is not implemented).
5. Frustum culling and GPU-driven instance culling (extraction currently submits every instance to every viewport that shows the scene), a shared per-scene instance buffer instead of one upload per viewport.
6. The deferred/PBR pipeline, shadows, atmosphere; the mesh asset path replacing the built-in primitives.
7. ImGui multi-viewport (OS windows) and a dedicated render thread consuming the published scene snapshots.
