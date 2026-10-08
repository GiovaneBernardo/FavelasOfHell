#pragma once

#include <vector>

#include "gtabr/ecs/world.h"

namespace gtabr {

// Deferred structural changes (spawn, despawn, insert/remove component, resource
// changes) recorded by ONE system and applied by the scheduler on the main thread
// after the systems that ran concurrently with it have finished.
//
// Why per-system queues: it makes the result independent of thread timing.
// Queues are applied in system registration order, so two runs of the same frame
// produce the same entities and the same component values, however the jobs were
// scheduled.
//
// Thread-safety: a queue belongs to one system run; recording is single-threaded.
// apply() must run on the main thread outside the parallel phase.
class GTABR_API CommandQueue {
public:
    // Refers either to an existing entity or to the N-th entity spawned by this
    // queue (whose real id only exists once the queue is applied).
    struct Target {
        u64 id = 0;
        u32 spawnSlot = kNoSlot;
        static constexpr u32 kNoSlot = 0xFFFFFFFFu;
        bool isSpawned() const { return spawnSlot != kNoSlot; }
    };

    explicit CommandQueue(World& world) : m_world(&world) {}

    World& world() const { return *m_world; }
    bool empty() const { return m_bytes.empty(); }

    Target spawn();
    void despawn(Target target);
    void insert(Target target, ComponentId component, const void* data, u32 size);
    void remove(Target target, ComponentId component);
    void insertResource(ComponentId component, const void* data, u32 size);
    void removeResource(ComponentId component);

    // Executes everything in recording order, then clears the queue.
    void apply();
    void clear();

private:
    World* m_world;
    std::vector<std::byte> m_bytes;
    std::vector<Entity> m_spawned; // scratch, reused between frames
    u32 m_spawnCount = 0;
};

// Builder returned by Commands::spawn()/entity(). Calls can be chained.
class EntityCommands {
public:
    EntityCommands(CommandQueue& queue, CommandQueue::Target target) : m_queue(&queue), m_target(target) {}

    template <class T>
    EntityCommands& insert(const T& value) {
        m_queue->insert(m_target, m_queue->world().component<T>(), std::is_empty_v<T> ? nullptr : &value,
                        std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T)));
        return *this;
    }
    template <class T>
    EntityCommands& insert() { // tags
        static_assert(std::is_empty_v<T>, "insert<T>() is for tag components; pass a value otherwise");
        m_queue->insert(m_target, m_queue->world().component<T>(), nullptr, 0);
        return *this;
    }
    template <class T>
    EntityCommands& remove() {
        m_queue->remove(m_target, m_queue->world().component<T>());
        return *this;
    }
    void despawn() { m_queue->despawn(m_target); }

    // The real id, only for entities that already existed (Commands::entity).
    // A just-spawned entity has no id until the queue is applied.
    Entity id() const { return Entity{m_target.isSpawned() ? 0 : m_target.id}; }

private:
    CommandQueue* m_queue;
    CommandQueue::Target m_target;
};

// System parameter: the handle a system uses to record deferred changes.
class Commands {
public:
    explicit Commands(CommandQueue& queue) : m_queue(&queue) {}

    EntityCommands spawn() { return EntityCommands(*m_queue, m_queue->spawn()); }
    template <class... Ts>
    EntityCommands spawn(const Ts&... components) {
        EntityCommands e = spawn();
        (e.insert(components), ...);
        return e;
    }
    EntityCommands entity(Entity e) { return EntityCommands(*m_queue, {e.id, CommandQueue::Target::kNoSlot}); }
    void despawn(Entity e) { m_queue->despawn({e.id, CommandQueue::Target::kNoSlot}); }

    template <class T>
    void insertResource(const T& value) {
        static_assert(!std::is_empty_v<T>, "a resource needs data");
        m_queue->insertResource(m_queue->world().component<T>(), &value, static_cast<u32>(sizeof(T)));
    }
    template <class T>
    void removeResource() { m_queue->removeResource(m_queue->world().component<T>()); }

private:
    CommandQueue* m_queue;
};

} // namespace gtabr
