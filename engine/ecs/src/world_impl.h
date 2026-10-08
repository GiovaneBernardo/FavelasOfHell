#pragma once

// Private to the ecs module: the only place (with the other .cpp files here) that
// sees flecs. Nothing in include/ may include this or <flecs.h>.

#include <flecs.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "foh/ecs/buffer.h"
#include "foh/ecs/events.h"
#include "foh/ecs/schema.h"
#include "foh/ecs/world.h"

namespace foh {

struct ComponentInfo {
    Schema schema;
    bool hasBuffers = false;
    bool sparse = false;
};

struct World::Impl {
    ecs_world_t* world = nullptr;
    // Marks entities created through spawn(): lets entityCount() and despawn()
    // ignore components, resources and other backend bookkeeping entities.
    ecs_entity_t spawnedTag = 0;
    u32 stageCount = 1;
    std::unordered_map<u64, std::unique_ptr<EventChannel>> channels;

    // The layout the world holds for each registered component. Plain data: it
    // stays valid while the module that described it is unloaded, and it is what
    // a reloaded module is compared against.
    std::unordered_map<ecs_entity_t, ComponentInfo> components;
    std::unordered_set<std::string> warnedUnreflected;
    std::unordered_map<ecs_entity_t, u32> denseIndex; // backend component id -> World::m_componentIds index
    BufferPool buffers;

    ~Impl() {
        // Fini runs the remove hooks of every component that owns buffers, which
        // use `buffers` and `components`: both are members, destroyed after this body.
        if (world) ecs_fini(world);
    }

    void registerInfo(ecs_entity_t id, Schema schema, bool sparse);
    // Creates the backend component (entity, size, storage traits) for desc.
    ecs_entity_t createComponent(const std::string& name, const ComponentDesc& desc);
    // Replaces a component whose layout changed: converts every instance (and
    // resource) to the new layout by field name. Returns the new component id.
    ecs_entity_t migrate(ecs_entity_t oldId, const std::string& name, const ComponentDesc& desc, Schema fresh);
};

// Friend of World: the scheduler's and the query code's view into the backend.
struct WorldAccess {
    static ecs_world_t* raw(const World& w) { return w.m_impl->world; }
    static World::Impl& impl(const World& w) { return *w.m_impl; }

    // One stage per job-system thread slot, so each thread has private iterator
    // allocators and deferred-command state.
    static void setStageCount(World& w, u32 count);
    static ecs_world_t* stage(const World& w, u32 slot);

    // Parallel phase: the backend world becomes immutable (its multi-threaded
    // readonly mode asserts on structural changes) until endParallel().
    static void beginParallel(World& w);
    static void endParallel(World& w);
};

} // namespace foh
