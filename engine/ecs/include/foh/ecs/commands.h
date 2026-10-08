#pragma once

#include <vector>

#include "foh/ecs/world.h"

namespace foh {

// Deferred structural changes (spawn, despawn, insert/remove component, parent and
// relationship changes, resource changes) recorded by ONE system and applied by
// the scheduler on the main thread after the systems that ran concurrently with it
// have finished.
//
// Why per-system queues: it makes the result independent of thread timing.
// Queues are applied in system registration order, so two runs of the same frame
// produce the same entities and the same component values, however the jobs were
// scheduled.
//
// Spawning: each queue keeps a few entity ids reserved on the main thread (sized
// from its recent use), so spawn() hands out a real, usable Entity immediately and
// it can be stored in components or used as a parent in the same frame. If a
// system spawns more than its reservation in one frame, the extra entities are
// addressed by position in the queue instead: everything on the returned
// EntityCommands still works, but id() is null for them (check hasId()).
//
// Thread-safety: a queue belongs to one system run; recording is single-threaded.
// apply() must run on the main thread outside the parallel phase.
class FOH_API CommandQueue {
public:
    // Refers to an existing/reserved entity, or to the N-th overflow spawn of this
    // queue (resolved when the queue is applied).
    struct Target {
        u64 id = 0;
        u32 spawnSlot = kNoSlot;
        static constexpr u32 kNoSlot = 0xFFFFFFFFu;
        bool isPending() const { return spawnSlot != kNoSlot; }
    };

    explicit CommandQueue(World& world) : m_world(&world) {}
    ~CommandQueue();
    CommandQueue(CommandQueue&&) noexcept = default;
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    World& world() const { return *m_world; }
    bool empty() const { return m_bytes.empty(); }

    Target spawn();
    void despawn(Target target);
    void insert(Target target, ComponentId component, const void* data, u32 size);
    void remove(Target target, ComponentId component);
    void setParent(Target child, Target parent);
    void clearParent(Target child);
    void addPair(Target e, ComponentId relation, Target target);
    void removePair(Target e, ComponentId relation, Target target);
    void insertResource(ComponentId component, const void* data, u32 size);
    void removeResource(ComponentId component);

    // Main thread, before the queue is used for a frame: tops up the reserved ids.
    void prepare();
    // Executes everything in recording order, then clears the queue.
    void apply();
    void clear();

private:
    World* m_world;
    std::vector<std::byte> m_bytes;
    std::vector<Entity> m_spawned;  // overflow spawns, filled in by apply()
    std::vector<Entity> m_reserved; // ids reserved ahead of time (alive, not yet spawned)
    size_t m_reservedNext = 0;      // next unused entry of m_reserved
    u32 m_spawnCount = 0;           // overflow spawns recorded
    u32 m_spawnedThisFrame = 0;
    u32 m_spawnedLastFrame = 0;
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

    EntityCommands& setParent(Entity parent) {
        m_queue->setParent(m_target, {parent.id, CommandQueue::Target::kNoSlot});
        return *this;
    }
    EntityCommands& setParent(const EntityCommands& parent) {
        m_queue->setParent(m_target, parent.m_target);
        return *this;
    }
    EntityCommands& clearParent() {
        m_queue->clearParent(m_target);
        return *this;
    }
    template <class Rel>
    EntityCommands& addPair(Entity target) {
        m_queue->addPair(m_target, relation<Rel>(), {target.id, CommandQueue::Target::kNoSlot});
        return *this;
    }
    template <class Rel>
    EntityCommands& addPair(const EntityCommands& target) {
        m_queue->addPair(m_target, relation<Rel>(), target.m_target);
        return *this;
    }
    template <class Rel>
    EntityCommands& removePair(Entity target) {
        m_queue->removePair(m_target, relation<Rel>(), {target.id, CommandQueue::Target::kNoSlot});
        return *this;
    }
    void despawn() { m_queue->despawn(m_target); }

    // The entity's id. Null only for an overflow spawn (see CommandQueue).
    Entity id() const { return Entity{m_target.isPending() ? 0 : m_target.id}; }
    bool hasId() const { return !m_target.isPending(); }

private:
    template <class Rel>
    ComponentId relation() const {
        static_assert(std::is_empty_v<Rel>, "a relationship type must be an empty tag struct");
        return m_queue->world().component<Rel>();
    }
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

} // namespace foh
