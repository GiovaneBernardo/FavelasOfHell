# 0001: ECS as a Bevy-style layer on flecs

- Status: **Accepted** (project lead, 2026-10-08)
- Supersedes: README Decision Log entry "Custom Bevy-style ECS: Decided"
- Related: README "ECS", AGENTS.md rules 2 and 3

## Context

The README planned a fully custom, Bevy-inspired ECS. The hard part of that is not the API, it is the storage and query engine (archetype tables, cached queries, relationships, observers, reflection, serialization): months of work and subtle bugs, competing with the renderer, streaming and asset work that actually make the game. The API we want (plugins, resources, typed `Query`/`Res`/`Commands` parameters, automatic parallelism) is a thin layer over such an engine.

Requirements that drove the design: game code hot reloads in 1-3 s with world state preserved; the engine DLL boundary stays narrow; the scheduler is multithreading friendly and deterministic; the minimum spec is a 6-core CPU, so parallelism must be real.

## Decision

Use **flecs** (v4.1.6, MIT, via vcpkg) as the storage/query backend and build our own layer on top. The backend is an implementation detail of `engine/ecs`:

- flecs is linked statically and privately into `foh_engine.dll`; no flecs header appears in a public engine header, and the game module never sees its ABI.
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
- Component identity is the type name (compile-time; a type may pin it with `static constexpr std::string_view kName`), cached per module and per world, so a reloaded DLL re-binds to the data already in the world. A changed component *layout* is detected by comparing schemas and **migrated by field name** (see Reflection); types without `FOH_REFLECT` are refused loudly if their size changes.
- Systems are removed (and their state destroyed by code that is still loaded) before the DLL unloads (`App::removeSystemsOwnedBy`). Covered by an automated test that loads a real DLL through `GameModuleHost`, reloads it, and checks the world.
- Components must be registered during `build()`; creating a component, query or event channel while systems run is rejected.

## Features built on the layer

**Reflection and migration.** `FOH_REFLECT(Type, fields...)` (or a hand-written `Reflect<T>::describe`) produces a `Schema`: names, field kinds, offsets, counts, nested structs, buffer element layouts. It is plain data (no function pointers, no `shared_ptr`), copied into the world at registration, so it outlives the DLL that built it. When a module reloads and a component's schema differs from the one the world holds, the engine renames the old component out of the way, creates the new one, and converts every instance (and resource) field by field: matched by name or by `formerName` (rename), numbers converted with saturation, new fields zeroed, removed fields dropped, buffer elements re-laid out in place. Types without `FOH_REFLECT` still work but cannot be migrated or saved.

**Buffers.** `BufferHandle<T>` is a 64-bit handle (generation + slot) to a variable-length array in an engine-owned `BufferPool`. The component stays POD; the elements live in the engine, so they survive reloads and are serializable through the schema (a buffer field is a first-class schema kind, elements may contain further buffers). Ownership is by handle: the component's `on_remove` hook (an engine function, never a module pointer) frees buffers when the component is removed or the entity dies. The pool's slot table is chunked and append-only so reads are lock-free while another thread creates a buffer; `create`/`release` take a short lock; element memory is allocated outside it. `Buffers<T>`/`BuffersMut<T>` system parameters declare read/write access per element type, and inside one system different threads may modify different buffers (e.g. in `parEach`). This replaces "one entity per item": a bag with 200 stacks is one entity and one buffer.

**Snapshots.** `saveSnapshot`/`loadSnapshot` write a self-describing binary (magic, version, schema table, entities with components/parent/pairs, resources). Loading matches components by name, converts through the same migration code (buffers are created with the file's layout, then re-laid out), creates fresh entities and remaps every `Entity` field, parent link and pair to the new ids. The input is treated as untrusted: all counts and offsets are validated against the data and against canonical field sizes, a dry-run pass validates the whole file before the world is touched, and a rejected file changes nothing (tested with every truncation and 400 random corruptions).

**Relationships.** Parent/child uses the backend `ChildOf` (exclusive, cascade delete): `setParent`, `parentOf`, `forEachChild`, a `Parent` query term (yields the parent entity per archetype table; `With/Without<Parent>` filter roots). Custom relationships are tag types used as pairs (`addPair<Rel>(e, target)`), with optional `cascadeDelete<Rel>()`. Both are in snapshots.

**Entity reservation.** The backend cannot create entities inside the parallel phase, so each `CommandQueue` keeps a number of ids reserved on the main thread, sized from its recent spawn counts (min 16). `cmd.spawn()` hands out a real `Entity` immediately, usable as a parent or stored in components. A system that outruns its reservation in one frame still works: overflow spawns are addressed by position and resolved when the queue is applied (`hasId()` is false for them; they can still be parents of or children of anything in the same queue).

**Observers.** `onAdd/onSet/onRemove<T>(fn(Trigger<T>, params...))` register backend observers whose callbacks run an ordinary system model on the main thread, with the same parameters except `World&`; `Commands` recorded by the observer are applied right after it returns. They are owner-tagged and removed with the module. A per-frame call limit (`AppConfig::maxObserverCalls`) turns a self-triggering observer into a logged error instead of a hang. They fire for changes made through `World`, `Commands` and exclusive systems, not for in-place writes through query references.

**Scheduling additions.** Explicit ordering also works between *sets* (any empty struct as a label: `inSet<S>`, `afterSet<S>`, `beforeSet<S>`), resolved at plan time over all members in the stage. A system whose parameters have deferred effects (`Commands`, `EventWriter`) puts anything explicitly ordered after it one *level* later: levels run one after another, each ending with those effects applied (Bevy's `apply_deferred`, derived automatically). Conflict ordering (registration order) does not insert sync points.

**Storage kinds.** `static constexpr bool kSparse = true;` stores a component in a backend sparse set that is also non-fragmenting: adding or removing it does not move the entity between archetype tables (verified: toggling a sparse component on 750 of 1000 entities left the archetype count unchanged). Queries read it per row instead of per column. `ChangedTables` is a query modifier using the backend's table-level change detection: a table counts as written when a system iterated it with write access or something was set/added/removed on it. It never misses a change but can over-report, and it is per table and per query, not per entity or component; use it to skip untouched parts of the world, not for exact "this value changed" logic.

## Consequences

- (+) Storage, queries, relationships, observers and a stable C API come from a mature library; our code is the typed API, scheduler and hot-reload ownership.
- (+) Deterministic frames and real inter-system parallelism, which flecs' own pipeline does not provide.
- (-) A third-party dependency at the heart of the engine; the layer must keep the flecs surface small so it stays replaceable.
- (-) POD-only components. Variable-length or owned data goes through `BufferHandle<T>` (engine pool) or an id into an engine service; it costs one indirection compared with an inline container.
- (-) Exclusive systems are serial barriers. Entity ids are reserved per `Commands` queue ahead of time, so a system that spawns far more than usual in one frame hits the (still working) overflow path once while its reservation adapts.

## Not implemented yet

- **Exact change detection.** Bevy-style per-entity `Added<T>`/`Changed<T>` needs a `Mut<T>` write wrapper plus a hidden tick column per tracked component; the table-level `ChangedTables` exists instead. Revisit when a system needs "which entities changed".
- Optional query terms (`Option<&T>`), relationship-ordered iteration (parents before children, "cascade") for transform propagation, relationship data (pairs carrying values), queries on custom pairs.
- `Local<T>` backed by the world (per-system state that survives reload), a JSON/text dump of a world for the editor, and exposing the schemas to the editor inspector.
- Engine allocators through the backend's OS API; replacing the single-queue job system with work stealing if profiling shows contention.
- Unreflected engine components with non-trivial members (none exist yet).

## Alternatives considered

- **Fully custom ECS** (previous plan): maximum control, but months of storage/query work; rejected by the lead in favor of flecs.
- **EnTT**: lighter and popular, but sparse-set only, with no scheduler, relationships, reflection or stable C API; we would still build most of the layer.
- **flecs pipeline for scheduling**: parallelizes inside a system only, with sync points decided by the library; does not give inter-system parallelism or per-system command ordering.
