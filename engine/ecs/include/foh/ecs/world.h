#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "foh/ecs/buffer.h"
#include "foh/ecs/reflect.h"
#include "foh/ecs/type_name.h"
#include "foh/ecs/types.h"
#include "foh/platform.h"

namespace foh {

class EventChannel;
struct WorldAccess; // engine-internal accessor (src/world_impl.h)

// What the storage backend needs to know about a component. Identity is the type
// name, so the engine and a reloaded game module resolve the same component.
struct ComponentDesc {
    std::string_view name;
    u32 size;      // 0 for tag components
    u32 alignment; // 0 for tag components
    // Fills the schema from the module's reflection code. Called only during the
    // registration call, never stored (it points into the registering module).
    void (*describe)(Schema& out);
    // Stored in a sparse set instead of archetype tables: adding/removing it never
    // moves the entity between tables. Declare with `static constexpr bool kSparse = true;`.
    bool sparse;
};

namespace detail {

// One cache slot per component type PER MODULE (inline variables are not shared
// across DLLs, which is intended: a reloaded module starts empty and re-resolves
// by name). Packs (world uid << 32 | dense component index) so the fast path is one
// relaxed load and a compare, and several Worlds can coexist.
template <class T>
struct ComponentSlot {
    static inline std::atomic<u64> value{0};
};

template <class T>
constexpr ComponentDesc describe() {
    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>,
                  "component types must be plain object types");
    // Components live in engine-owned memory and are moved with memcpy. No
    // constructors/destructors means no function pointers into a game module are
    // ever stored in the world, which is what makes game hot reload safe. Use
    // handles (Entity, ids, indices) instead of owning members like std::vector.
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                  "components must be trivially copyable and destructible (POD-like)");
    return {componentName<T>(), std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T)),
            std::is_empty_v<T> ? 0u : static_cast<u32>(alignof(T)), &buildSchema<T>, isSparse<T>()};
}

} // namespace detail

// Entities, components and resources, stored by the flecs backend (which never
// leaks through this header).
//
// Thread-safety: a World is NOT thread-safe in general. While systems run in
// parallel (inParallelPhase()), only the read-only fast paths below may be used
// from several threads: component<T>() for an already registered T, isAlive(),
// has(), get() and resource() reads. Everything that changes structure goes
// through Commands inside systems, or happens outside the parallel phase.
class FOH_API World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    u32 uid() const { return m_uid; }
    // True while the scheduler runs systems concurrently. Structural changes are
    // forbidden then (asserted).
    bool inParallelPhase() const { return m_parallel; }

    // ---- components -------------------------------------------------------
    // Registers T on first use. Registering is not allowed during the parallel
    // phase, so register components in plugin build() (App::registerComponent).
    template <class T>
    ComponentId component() const {
        constexpr ComponentDesc desc = detail::describe<T>();
        std::atomic<u64>& slot = detail::ComponentSlot<T>::value;
        const u64 cached = slot.load(std::memory_order_relaxed);
        if (static_cast<u32>(cached >> 32) == m_uid) return m_componentIds[static_cast<u32>(cached)];
        return resolveComponent(desc, slot);
    }

    // ---- entities ---------------------------------------------------------
    Entity spawn();
    template <class... Ts>
    Entity spawn(const Ts&... components) {
        const Entity e = spawn();
        (insert(e, components), ...);
        return e;
    }
    bool despawn(Entity entity);
    bool isAlive(Entity entity) const;
    // Entities created through spawn()/Commands; excludes components, resources
    // and other backend bookkeeping entities.
    size_t entityCount() const;
    // Number of archetype tables the backend holds. Mostly for tests and diagnostics:
    // it grows with distinct component combinations, not with entities.
    size_t archetypeCount() const;

    template <class T>
    void insert(Entity e, const T& value) {
        setComponent(e, component<T>(), std::is_empty_v<T> ? nullptr : &value,
                     std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T)));
    }
    template <class T>
    void insert(Entity e) { // tags
        static_assert(std::is_empty_v<T>, "insert<T>(Entity) is for tag components; pass a value otherwise");
        setComponent(e, component<T>(), nullptr, 0);
    }
    template <class T>
    void remove(Entity e) { removeComponent(e, component<T>()); }
    template <class T>
    bool has(Entity e) const { return hasComponent(e, component<T>()); }
    // nullptr if the entity is dead or lacks T. Pointers are invalidated by any
    // structural change.
    template <class T>
    T* get(Entity e) { return static_cast<T*>(componentPtr(e, component<T>())); }
    template <class T>
    const T* get(Entity e) const { return static_cast<const T*>(componentPtr(e, component<T>())); }

    // ---- relationships -----------------------------------------------------------
    // Parent/child: an entity has at most one parent. Despawning a parent despawns
    // its children (recursively).
    void setParent(Entity child, Entity parent); // replaces a previous parent
    void clearParent(Entity child);
    Entity parentOf(Entity child) const;          // null if none
    size_t childCount(Entity parent) const;
    // Calls fn(Entity child) for each child, in unspecified order. fn must not
    // change the world's structure.
    template <class F>
    void forEachChild(Entity parent, F&& fn) const {
        eachChild(
            parent,
            [](void* ctx, Entity child) { (*static_cast<std::remove_reference_t<F>*>(ctx))(child); }, &fn);
    }

    // Custom relationships: Rel is an empty (tag) component type that names the
    // relationship, e.g. `struct Owns {};`. An entity can have several targets.
    template <class Rel>
    void addPair(Entity e, Entity target) { addPair(e, relation<Rel>(), target); }
    template <class Rel>
    void removePair(Entity e, Entity target) { removePair(e, relation<Rel>(), target); }
    template <class Rel>
    bool hasPair(Entity e, Entity target) const { return hasPair(e, relation<Rel>(), target); }
    template <class Rel>
    Entity pairTarget(Entity e, u32 index = 0) const { return pairTarget(e, relation<Rel>(), index); }
    // Make despawning a target despawn the entities that point at it with Rel
    // (the default is to just drop the pair).
    template <class Rel>
    void cascadeDelete() { cascadeDelete(relation<Rel>()); }

    void addPair(Entity e, ComponentId relation, Entity target);
    void removePair(Entity e, ComponentId relation, Entity target);
    bool hasPair(Entity e, ComponentId relation, Entity target) const;
    Entity pairTarget(Entity e, ComponentId relation, u32 index) const;
    void cascadeDelete(ComponentId relation);

    // ---- entity id reservation (used by Commands; rarely needed directly) ---------
    // A reserved entity is alive but is not counted as spawned until adopted.
    Entity reserveEntity();
    void adoptEntity(Entity reserved);
    void discardReserved(Entity reserved);

    // ---- resources: one value per type, shared by all systems --------------
    template <class T>
    void insertResource(const T& value) {
        static_assert(!std::is_empty_v<T>, "a resource needs data");
        setResource(component<T>(), &value, static_cast<u32>(sizeof(T)));
    }
    template <class T>
    T* resource() { return static_cast<T*>(resourcePtr(component<T>())); }
    template <class T>
    const T* resource() const { return static_cast<const T*>(resourcePtr(component<T>())); }
    template <class T>
    bool removeResource() { return removeResource(component<T>()); }

    // ---- reflection ----------------------------------------------------------
    // Layout the world currently holds for a component; nullptr if unknown.
    const Schema* schema(ComponentId component) const;
    template <class T>
    const Schema* schema() const { return schema(component<T>()); }

    // ---- buffers: variable-length arrays owned by the engine ---------------------
    BufferPool& buffers();
    const BufferPool& buffers() const;

    // ---- events ------------------------------------------------------------
    // Channel for events keyed by type identity; created on first use. The
    // channel lives in the engine, so it survives game module reloads.
    EventChannel& eventChannel(u64 key, u32 elemSize);
    // Called once per frame by App: drops events older than one full frame.
    void advanceEvents();

    // ---- type-erased operations (typed wrappers above and the command queue) --
    ComponentId resolveComponent(const ComponentDesc& desc, std::atomic<u64>& slot) const;
    void setComponent(Entity e, ComponentId component, const void* data, u32 size);
    void removeComponent(Entity e, ComponentId component);
    bool hasComponent(Entity e, ComponentId component) const;
    void* componentPtr(Entity e, ComponentId component) const;
    void setResource(ComponentId component, const void* data, u32 size);
    void* resourcePtr(ComponentId component) const;
    bool removeResource(ComponentId component);

    struct Impl; // opaque: backend state (src/world_impl.h)

private:
    using ChildFn = void (*)(void* ctx, Entity child);
    void eachChild(Entity parent, ChildFn fn, void* ctx) const;
    template <class Rel>
    ComponentId relation() const {
        static_assert(std::is_empty_v<Rel>, "a relationship type must be an empty tag struct");
        return component<Rel>();
    }

    friend struct WorldAccess;
    std::unique_ptr<Impl> m_impl;
    u32 m_uid;
    // Backend id of each component this world has registered, by dense index. The
    // per-module cache slots store the index, because a backend id can use all 64
    // bits (recycled ids carry a generation). Only grows, and only on the main thread.
    mutable std::vector<ComponentId> m_componentIds;
    bool m_parallel = false;
};

} // namespace foh
