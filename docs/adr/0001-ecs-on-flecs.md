# 0001: ECS as a Bevy-style layer on flecs

- Status: **Accepted** (project lead, 2026-10-08)
- Supersedes: README Decision Log entry "Custom Bevy-style ECS: Decided"
- Related: README "ECS", AGENTS.md rules 2 and 3

## Context

The README planned a fully custom, Bevy-inspired ECS. The hard part of that is not the API, it is the storage and query engine (archetype tables, cached queries, relationships, observers, reflection, serialization): months of work and subtle bugs, competing with the renderer, streaming and asset work that actually make the game. The API we want (plugins, resources, typed `Query`/`Res`/`Commands` parameters, automatic parallelism) is a thin layer over such an engine.

Requirements that drove the design: game code hot reloads in 1-3 s with world state preserved; the engine DLL boundary stays narrow; the scheduler is multithreading friendly and deterministic; the minimum spec is a 6-core CPU, so parallelism must be real.

## Decision

Use **flecs** (v4.1.6, MIT, via vcpkg) as the storage/query backend and build our own layer on top. The backend is an implementation detail of `engine/ecs`:

- flecs is linked statically and privately into `gtabr_engine.dll`; no flecs header appears in a public engine header, and the game module never sees its ABI.
- We use flecs for: entity ids and generations, archetype storage, cached queries, and (planned) relationships, observers, reflection (meta) and serialization.
- We do **not** use the flecs pipeline/systems or its threading: scheduling is ours (see below).

The layer (`engine/ecs`, plus a new small `engine/jobs`):

| Concept | Implementation |
|---|---|
| `World` | owns the flecs world; typed `spawn/insert/get/remove`, resources, event channels |
| Components | plain POD types, identified by **type name**, registered via `App::registerComponent<T>()` |
| Resources | singleton component on the component's own entity (`Res<T>`, `ResMut<T>`) |
| Systems | plain function pointers; parameters (`Query<...>`, `Res`, `ResMut`, `Commands`, `EventReader/Writer`, `World&`) are deduced and drive the access set |
| Plugins | `Plugin::build(App&)`; everything registered is tagged with an owner and removed before a DLL unloads |
| Scheduler | per stage, systems are split into segments at exclusive (`World&`) systems; inside a segment they run as a dependency graph on the job system |
| `JobSystem` | worker pool, fork-join counters, helping `wait()` (nested parallelism cannot deadlock), stable per-thread slots |

## Threading model

1. **Access sets decide parallelism.** Two systems may overlap only if neither writes what the other reads or writes. Conflicting systems without an explicit relation run in registration order. `before()/after()` add explicit edges (cycles are reported and ignored).
2. **Parallel phase = flecs multi-threaded readonly mode.** During a segment the world is immutable structurally; systems only read/write component data in place. Misuse (structural change from a system) asserts instead of racing.
3. **One flecs stage per job-system thread slot**, so iterators have private scratch memory.
4. **All structural changes and events are buffered per system** (`CommandQueue`, `EventWriter`) and applied on the main thread, **in system registration order**, when the segment ends. The result of a frame therefore does not depend on thread timing (tested: 25 frames of 8 parallel spawners always produce the same entity order).
5. **Data parallelism inside a system:** `Query::parEach(f)` splits every matched table across the job system threads with flecs worker iterators.
6. Queries are cached and created outside the parallel phase (registration time), because iterating from several threads must only read the match cache.

## Hot reload rules this imposes

- Components/resources/events must be **trivially copyable and destructible** (static_assert). flecs then stores no constructor/destructor/copy hooks, so no function pointer into a game DLL ever lives in the world. Use ids/handles instead of owning members (`std::vector`, `std::string`).
- Component identity is the type name (compile-time), cached per module and per world, so a reloaded DLL re-binds to the data already in the world. A changed component *layout* is detected (size/alignment) and refused loudly; field-level migration is part of the reflection work.
- Systems are removed (and their state destroyed by code that is still loaded) before the DLL unloads (`App::removeSystemsOwnedBy`). Covered by an automated test that loads a real DLL through `GameModuleHost`, reloads it, and checks the world.
- Components must be registered during `build()`; creating a component, query or event channel while systems run is rejected.

## Consequences

- (+) Storage, queries, relationships, observers and a stable C API come from a mature library; our code is the typed API, scheduler and hot-reload ownership.
- (+) Deterministic frames and real inter-system parallelism, which flecs' own pipeline does not provide.
- (-) A third-party dependency at the heart of the engine; the layer must keep the flecs surface small so it stays replaceable.
- (-) POD-only components. Owning types need an engine-side service or an id into an engine pool.
- (-) Exclusive systems are serial barriers; spawn through `Commands` returns no id until applied (`EntityCommands::id()` is null for just-spawned entities).

## Not implemented yet (planned, in this order)

1. Reflection of components (flecs meta) and world serialization/restore across reload, with layout migration.
2. Relationships (parent/child, "inside building") and observers/hooks exposed through plugins.
3. Change detection (`Added`/`Changed` filters), optional terms, system sets, automatic sync points between ordered systems.
4. Sparse-set storage opt-in (flecs `Sparse` trait) for frequently toggled markers.
5. Entity-id reservation in `Commands`, `Local<T>` backed by the world, engine allocators via the flecs OS API, replacing the single-queue job system with work stealing if profiling asks for it.

## Alternatives considered

- **Fully custom ECS** (previous plan): maximum control, but months of storage/query work; rejected by the lead in favor of flecs.
- **EnTT**: lighter and popular, but sparse-set only, with no scheduler, relationships, reflection or stable C API; we would still build most of the layer.
- **flecs pipeline for scheduling**: parallelizes inside a system only, with sync points decided by the library; does not give inter-system parallelism or per-system command ordering.
