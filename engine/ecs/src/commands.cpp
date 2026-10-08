#include "foh/ecs/commands.h"

#include <algorithm>
#include <cstring>

#include "foh/assert.h"

namespace foh {

namespace {

enum class Op : u32 {
    Spawn,
    Despawn,
    Insert,
    Remove,
    SetParent,
    ClearParent,
    AddPair,
    RemovePair,
    InsertResource,
    RemoveResource,
};

// Fixed-size record header, followed by `size` payload bytes padded to 8.
struct Header {
    Op op;
    u32 size;
    u64 target;      // entity id (when targetSlot is none)
    u32 targetSlot;  // overflow spawn index, or kNoSlot
    u32 otherSlot;
    u64 other;       // second entity (parent / pair target)
    u64 component;   // component or relation id
};
static_assert(sizeof(Header) == 40);

constexpr u32 kNoSlot = CommandQueue::Target::kNoSlot;
constexpr size_t padded(u32 size) { return (static_cast<size_t>(size) + 7) & ~size_t{7}; }

constexpr u32 kMinReserve = 16;
constexpr u32 kMaxReserve = 1u << 16;

void record(std::vector<std::byte>& bytes, const Header& h, const void* payload) {
    const size_t at = bytes.size();
    bytes.resize(at + sizeof(Header) + padded(h.size)); // zero-fills the padding
    std::memcpy(bytes.data() + at, &h, sizeof(Header));
    if (h.size) std::memcpy(bytes.data() + at + sizeof(Header), payload, h.size);
}

Header header(Op op, CommandQueue::Target t, ComponentId component = 0, CommandQueue::Target other = {},
              u32 size = 0) {
    return {op, size, t.id, t.spawnSlot, other.spawnSlot, other.id, component};
}

} // namespace

CommandQueue::~CommandQueue() {
    // Ids that were reserved but never used. (Moved-from queues have none.)
    for (size_t i = m_reservedNext; i < m_reserved.size(); ++i) m_world->discardReserved(m_reserved[i]);
}

void CommandQueue::prepare() {
    // Drop what the last frame consumed, remember how much that was, and top up.
    m_reserved.erase(m_reserved.begin(), m_reserved.begin() + static_cast<std::ptrdiff_t>(m_reservedNext));
    m_reservedNext = 0;
    m_spawnedLastFrame = m_spawnedThisFrame;
    m_spawnedThisFrame = 0;
    const u32 want = std::clamp(m_spawnedLastFrame * 2, kMinReserve, kMaxReserve);
    while (m_reserved.size() < want) m_reserved.push_back(m_world->reserveEntity());
}

CommandQueue::Target CommandQueue::spawn() {
    ++m_spawnedThisFrame;
    Target t;
    if (m_reservedNext < m_reserved.size()) {
        t.id = m_reserved[m_reservedNext++].id;
    } else {
        t.spawnSlot = m_spawnCount++;
    }
    record(m_bytes, header(Op::Spawn, t), nullptr);
    return t;
}

void CommandQueue::despawn(Target t) { record(m_bytes, header(Op::Despawn, t), nullptr); }

void CommandQueue::insert(Target t, ComponentId component, const void* data, u32 size) {
    record(m_bytes, header(Op::Insert, t, component, {}, size), data);
}

void CommandQueue::remove(Target t, ComponentId component) {
    record(m_bytes, header(Op::Remove, t, component), nullptr);
}

void CommandQueue::setParent(Target child, Target parent) {
    record(m_bytes, header(Op::SetParent, child, 0, parent), nullptr);
}

void CommandQueue::clearParent(Target child) { record(m_bytes, header(Op::ClearParent, child), nullptr); }

void CommandQueue::addPair(Target e, ComponentId relation, Target target) {
    record(m_bytes, header(Op::AddPair, e, relation, target), nullptr);
}

void CommandQueue::removePair(Target e, ComponentId relation, Target target) {
    record(m_bytes, header(Op::RemovePair, e, relation, target), nullptr);
}

void CommandQueue::insertResource(ComponentId component, const void* data, u32 size) {
    record(m_bytes, header(Op::InsertResource, {0, kNoSlot}, component, {}, size), data);
}

void CommandQueue::removeResource(ComponentId component) {
    record(m_bytes, header(Op::RemoveResource, {0, kNoSlot}, component), nullptr);
}

void CommandQueue::clear() {
    m_bytes.clear(); // keeps capacity: no per-frame allocation once warmed up
    m_spawnCount = 0;
}

void CommandQueue::apply() {
    World& world = *m_world;
    FOH_VERIFY(!world.inParallelPhase(), "CommandQueue::apply during the parallel phase");
    m_spawned.assign(m_spawnCount, kNullEntity);

    auto resolve = [&](u64 id, u32 slot) { return slot != kNoSlot ? m_spawned[slot] : Entity{id}; };

    size_t at = 0;
    while (at < m_bytes.size()) {
        Header h;
        std::memcpy(&h, m_bytes.data() + at, sizeof(Header));
        const std::byte* payload = m_bytes.data() + at + sizeof(Header);
        at += sizeof(Header) + padded(h.size);

        const Entity target = resolve(h.target, h.targetSlot);
        switch (h.op) {
            case Op::Spawn:
                if (h.targetSlot != kNoSlot) m_spawned[h.targetSlot] = world.spawn();
                else world.adoptEntity(Entity{h.target});
                break;
            case Op::Despawn: world.despawn(target); break;
            case Op::Insert: world.setComponent(target, h.component, h.size ? payload : nullptr, h.size); break;
            case Op::Remove: world.removeComponent(target, h.component); break;
            case Op::SetParent: world.setParent(target, resolve(h.other, h.otherSlot)); break;
            case Op::ClearParent: world.clearParent(target); break;
            case Op::AddPair: world.addPair(target, h.component, resolve(h.other, h.otherSlot)); break;
            case Op::RemovePair: world.removePair(target, h.component, resolve(h.other, h.otherSlot)); break;
            case Op::InsertResource: world.setResource(h.component, payload, h.size); break;
            case Op::RemoveResource: world.removeResource(h.component); break;
        }
    }
    clear();
}

} // namespace foh
