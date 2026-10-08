#pragma once

// Private to the ecs module: the only place (with the other .cpp files here) that
// sees flecs. Nothing in include/ may include this or <flecs.h>.

#include <flecs.h>

#include <memory>
#include <unordered_map>

#include "gtabr/ecs/events.h"
#include "gtabr/ecs/world.h"

namespace gtabr {

struct World::Impl {
    ecs_world_t* world = nullptr;
    // Marks entities created through spawn(): lets entityCount() and despawn()
    // ignore components, resources and other backend bookkeeping entities.
    ecs_entity_t spawnedTag = 0;
    u32 stageCount = 1;
    std::unordered_map<u64, std::unique_ptr<EventChannel>> channels;

    ~Impl() {
        if (world) ecs_fini(world);
    }
};

// Friend of World: the scheduler's and the query code's view into the backend.
struct WorldAccess {
    static ecs_world_t* raw(const World& w) { return w.m_impl->world; }

    // One stage per job-system thread slot, so each thread has private iterator
    // allocators and deferred-command state.
    static void setStageCount(World& w, u32 count);
    static ecs_world_t* stage(const World& w, u32 slot);

    // Parallel phase: the backend world becomes immutable (its multi-threaded
    // readonly mode asserts on structural changes) until endParallel().
    static void beginParallel(World& w);
    static void endParallel(World& w);
};

} // namespace gtabr
