#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>

#include "gtabr/ecs/type_name.h"
#include "gtabr/ecs/types.h"
#include "gtabr/platform.h"

namespace gtabr {

class EventChannel;
struct WorldAccess; // engine-internal accessor (src/world_impl.h)

// What the storage backend needs to know about a component. Identity is the type
// name, so the engine and a reloaded game module resolve the same component.
struct ComponentDesc {
    std::string_view name;
    u32 size;      // 0 for tag components
    u32 alignment; // 0 for tag components
};

namespace detail {

// One cache slot per component type PER MODULE (inline variables are not shared
// across DLLs, which is intended: a reloaded module starts empty and re-resolves
// by name). Packs (world uid << 32 | component id) so the fast path is one
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
    return {typeName<T>(), std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T)),
            std::is_empty_v<T> ? 0u : static_cast<u32>(alignof(T))};
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
class GTABR_API World {
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
        if (static_cast<u32>(cached >> 32) == m_uid) return cached & 0xFFFFFFFFull;
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

private:
    friend struct WorldAccess;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    u32 m_uid;
    bool m_parallel = false;
};

} // namespace gtabr
