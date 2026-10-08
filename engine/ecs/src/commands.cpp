#include "gtabr/ecs/commands.h"

#include <cstring>

#include "gtabr/assert.h"

namespace gtabr {

namespace {

enum class Op : u32 { Spawn, Despawn, Insert, Remove, InsertResource, RemoveResource };

// Fixed-size record header, followed by `size` payload bytes padded to 8.
struct Header {
    Op op;
    u32 size;
    u64 target;
    u32 spawnSlot;
    u32 reserved;
    u64 component;
};
static_assert(sizeof(Header) == 32);

constexpr size_t padded(u32 size) { return (static_cast<size_t>(size) + 7) & ~size_t{7}; }

} // namespace

static void record(std::vector<std::byte>& bytes, const Header& h, const void* payload) {
    const size_t at = bytes.size();
    bytes.resize(at + sizeof(Header) + padded(h.size)); // zero-fills the padding
    std::memcpy(bytes.data() + at, &h, sizeof(Header));
    if (h.size) std::memcpy(bytes.data() + at + sizeof(Header), payload, h.size);
}

CommandQueue::Target CommandQueue::spawn() {
    Target t;
    t.spawnSlot = m_spawnCount++;
    record(m_bytes, {Op::Spawn, 0, 0, t.spawnSlot, 0, 0}, nullptr);
    return t;
}

void CommandQueue::despawn(Target t) { record(m_bytes, {Op::Despawn, 0, t.id, t.spawnSlot, 0, 0}, nullptr); }

void CommandQueue::insert(Target t, ComponentId component, const void* data, u32 size) {
    record(m_bytes, {Op::Insert, size, t.id, t.spawnSlot, 0, component}, data);
}

void CommandQueue::remove(Target t, ComponentId component) {
    record(m_bytes, {Op::Remove, 0, t.id, t.spawnSlot, 0, component}, nullptr);
}

void CommandQueue::insertResource(ComponentId component, const void* data, u32 size) {
    record(m_bytes, {Op::InsertResource, size, 0, Target::kNoSlot, 0, component}, data);
}

void CommandQueue::removeResource(ComponentId component) {
    record(m_bytes, {Op::RemoveResource, 0, 0, Target::kNoSlot, 0, component}, nullptr);
}

void CommandQueue::clear() {
    m_bytes.clear(); // keeps capacity: no per-frame allocation once warmed up
    m_spawnCount = 0;
}

void CommandQueue::apply() {
    World& world = *m_world;
    GTABR_VERIFY(!world.inParallelPhase(), "CommandQueue::apply during the parallel phase");
    m_spawned.assign(m_spawnCount, kNullEntity);

    size_t at = 0;
    while (at < m_bytes.size()) {
        Header h;
        std::memcpy(&h, m_bytes.data() + at, sizeof(Header));
        const std::byte* payload = m_bytes.data() + at + sizeof(Header);
        at += sizeof(Header) + padded(h.size);

        const Entity target =
            h.spawnSlot != Target::kNoSlot ? m_spawned[h.spawnSlot] : Entity{h.target};
        switch (h.op) {
            case Op::Spawn: m_spawned[h.spawnSlot] = world.spawn(); break;
            case Op::Despawn: world.despawn(target); break;
            case Op::Insert: world.setComponent(target, h.component, h.size ? payload : nullptr, h.size); break;
            case Op::Remove: world.removeComponent(target, h.component); break;
            case Op::InsertResource: world.setResource(h.component, payload, h.size); break;
            case Op::RemoveResource: world.removeResource(h.component); break;
        }
    }
    clear();
}

} // namespace gtabr
