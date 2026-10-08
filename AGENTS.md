# AGENTS.md

Instructions for AI coding agents working in this repo. Read `README.md` first for the vision, stack, architecture and decision log. This file holds the working rules.

## Who works on this

The game is called **Favelas of Hell** (abbreviated FOH: namespace `foh`, macros `FOH_*`, targets `foh_*`; see README "Naming"). It is mostly written by AI agents under the direction of an experienced human lead, who makes the architecture and technical decisions and reviews the results. So: follow the Decision Log, ask before changing a decided design, keep diffs reviewable, explain non-obvious choices briefly, and report honestly what you built, tested and could not verify. Do not add dependencies the lead did not ask for without saying so (e.g. the ECS is our Bevy-style layer on flecs, see ADR 0001: build features on that layer, do not add another ECS library and do not use flecs directly outside `engine/ecs`).

## Project in one paragraph

A serious open-world Brazilian crime game on a custom C++20 engine (own renderer, Bevy-style ECS, asset pipeline, ImGui editor). Windows is the primary dev platform; keep Linux/macOS portability in mind. Iteration speed through hot reloading is a core requirement.

## Non-negotiable rules

1. **Layering:** (namespaces: engine = `foh`, game = `foh::game`, editor = `foh::editor`) `game → engine ← editor`. The engine must never include editor or game headers. The editor must not be required to run the game. Violating this breaks hot reload.
2. **Game code must be hot reloadable, with a save-and-see workflow (~1-3 s).** Keep game modules small, headers light and link times short; do not add anything that makes incremental rebuilds slow (heavy templates in widely included headers, unity builds in game code). No mutable global/static state in game modules. All state lives in the ECS world/resources and must be reflected (serializable) so it survives a reload. Never store pointers into the game module (function pointers, vtables, `std::function` from the module) in engine-owned memory without re-registering after reload.
3. **Module boundaries use narrow, stable interfaces** (C-style tables or pure interfaces). No STL types or exceptions across DLL boundaries unless allocators are shared; use engine allocators.
4. **Everything platform/graphics-API specific stays behind an abstraction** (`platform/`, the RHI wrapper). No direct D3D12/Vulkan calls outside the RHI layer; use NVRHI (once confirmed).
5. **Assets are never loaded from source formats at runtime.** Go through importer → engine format → loader. Loaders are async and hot-reload aware.
6. **Do not silently change decisions** marked Decided/Proposed in the README. If a change is warranted, propose it and update the Decision Log.
7. **Fictional content only** for brands, factions and people. Record license and attribution metadata for every third-party asset. The setting is Vidigal (Rio de Janeiro), initially ~1500 m x ~500 m (ground plan, whole favela plus some water), steep terrain, realistic art direction: depict it with care and variety. Design systems for density (power lines, poles, alleys, stairs, street businesses) and verticality.
8. **Performance budget:** minimum spec is RTX 2060 / RX 6600 (6 GB VRAM, 1080p60). Ray tracing and mesh shaders are optional features, never requirements. Design data layouts and streaming for the minimum spec; dev machine is far stronger, so do not judge performance only on it.

## Architecture cheat sheet

- ECS: Bevy-style layer on flecs (private to `engine/ecs`, ADR 0001); systems are functions with typed params (`Query`, `Res`, `ResMut`, `Commands`, events, `Buffers`); our scheduler gets parallelism from access sets and applies commands/events per system in registration order (deterministic); components are POD (trivially copyable), identified by type name, and reflected with `FOH_REFLECT` (data-only schema: it is what lets layout changes migrate on hot reload and worlds be saved); variable-length data goes in `BufferHandle<T>` (engine-owned pool), never a `std::vector`; features ship as **plugins** (components, systems, observers); register components in `build()`, never while systems run.
- Renderer (ADR 0003): render graph with per-viewport instances (viewport = target + camera + scene + own graph; scenes are extracted snapshots of ECS worlds); tiled deferred + forward transparents; keep acceleration-structure/bindless-friendly data layouts for future ray tracing.
- Assets: chunked binary container + descriptor (named, typed, strided fields) + payload blocks; GUID-based asset DB; content-hash cache; versions handled via descriptor migration.
- Blender: headless Blender → glTF-style export on `.blend` change; imported objects remember source `.blend` + object name.

## Code conventions

- C++20. No exceptions in engine code paths; no RTTI-dependent logic (use the engine reflection). Prefer data-oriented design; avoid deep inheritance.
- Naming: `PascalCase` types, `camelCase` functions/variables, `m_` prefix for private members, `kPascalCase` constants, `snake_case` files and directories. (Adjust here if the codebase establishes otherwise; match existing code.)
- Headers: `#pragma once`; minimal includes; forward declare where possible. Public engine headers must not leak third-party headers (NVRHI, Jolt, ImGui) unless the module is explicitly the adapter for it. The renderer has two tiers: `foh/renderer/` (NVRHI-free, safe for game modules) and `foh/rhi/` (the NVRHI adapter: engine internals, editor, runtime and tests only; game modules must never include it).
- Memory: use engine allocators and arenas; no hidden allocations in hot paths; no per-frame heap churn.
- Threading: use the job system, not raw threads. Document thread-safety on every public type.
- Logging/asserts: use engine logging macros; assert on invariants; no `printf`/`std::cout`.
- Comments explain *why*, not *what*. Match the density of surrounding code.
- Shaders live in `/shaders` (Slang; every entry point is compiled by `shaders/CMakeLists.txt` into `<bin>/shaders`, register new ones there), are hot reloaded by `ShaderLibrary`, and share common headers for structs mirrored on the CPU (keep layouts in sync with static asserts, see `ViewConstants`).
- Rendering: scene data reaches the renderer only through ECS extraction plugins into `RenderScene` snapshots. New render features add passes to the viewport graph (declare every resource a pass touches; pass bodies must be safe to run on any recording thread) instead of calling NVRHI from elsewhere. Depth is reverse-Z (ADR 0003).

## Build and test

- Build system: CMake with presets; third-party deps via vcpkg (manifest mode) or `/third_party`.
- Commands (run from a VS Developer prompt, or just open the folder in Visual Studio):
  - Configure: `cmake --preset windows-debug`
  - Build: `cmake --build --preset windows-debug` (game only: `--target foh_game`)
  - Tests: `ctest --preset windows-debug`
- Output: `out/build/<preset>/bin`. Dependencies: edit `vcpkg.json` (pinned baseline); the first configure takes ~10 min.
- Engine modules are added with `foh_add_engine_module(...)` (OBJECT libs folded into `foh_engine.dll`); the game is a MODULE library that must link `foh::engine` and nothing statically engine-like. Public engine headers use `FOH_API` for exports.
- Every new module gets its own CMake target with explicit public/private dependencies. No global include directories; no glob-based source lists for engine targets.
- Add unit tests for ECS, containers, asset serialization and migration, and anything non-GPU. Verify hot reload flows manually and, where possible, with an automated reload test.
- Before declaring work done: it builds on the Windows preset, tests pass, and you did not add platform-specific code outside `platform/` (and the D3D12 parts of `renderer/src/rhi/`). For rendering/editor changes also run `foh_editor --frames 300 --stress --no-vsync` (and once with `--group-passes 1`): exit code 0 means the D3D12 validation layer stayed silent; add `--screenshot out.png` to look at the result. Needs the Vulkan SDK (`slangc`).

## Working style

- Prefer small, reviewable changes. Do the work, then summarize what changed and what is unverified.
- For architecture-level choices (new module, format change, new dependency), write a short ADR in `/docs/adr/NNNN-title.md` and update the README Decision Log.
- When requirements are ambiguous and the answer changes the design, ask. Otherwise pick the sensible default and state it.
- Adding a dependency requires: license check (permissive preferred), CMake/vcpkg integration, and a note in the README.
- Never commit large binary assets directly; plan for Git LFS or an external asset store. Cooked artifacts and caches are git-ignored.

## Directory map

See "Repository Layout" in `README.md`. Quick version: `engine/` (core, ecs, renderer, physics, assets, world, platform), `editor/`, `game/`, `shaders/`, `tools/`, `third_party/`, `docs/`.
