#include <atomic>
#include <string>

#include "gtabr/assert.h"
#include "gtabr/ecs/world.h"
#include "gtabr/log.h"
#include "world_impl.h"

namespace gtabr {

namespace {
u32 nextWorldUid() {
    static std::atomic<u32> counter{0};
    return ++counter; // starts at 1: a zeroed cache slot never matches a world
}
} // namespace

World::World() : m_impl(std::make_unique<Impl>()), m_uid(nextWorldUid()) {
    // ecs_mini: no addon modules are imported (no pipeline, no timers...). The
    // scheduler is ours; addons are imported explicitly when a feature needs one.
    m_impl->world = ecs_mini();
    ecs_entity_desc_t tag{};
    tag.name = "gtabr_Spawned";
    m_impl->spawnedTag = ecs_entity_init(m_impl->world, &tag);
}

World::~World() = default;

// ---- WorldAccess --------------------------------------------------------------

void WorldAccess::setStageCount(World& w, u32 count) {
    if (count < 1) count = 1;
    ecs_set_stage_count(w.m_impl->world, static_cast<i32>(count));
    w.m_impl->stageCount = count;
}

ecs_world_t* WorldAccess::stage(const World& w, u32 slot) {
    GTABR_VERIFY(slot < w.m_impl->stageCount, "thread slot has no ECS stage: App stage count must cover the job system");
    return ecs_get_stage(w.m_impl->world, static_cast<i32>(slot));
}

void WorldAccess::beginParallel(World& w) {
    w.m_parallel = true;
    ecs_readonly_begin(w.m_impl->world, true);
}

void WorldAccess::endParallel(World& w) {
    ecs_readonly_end(w.m_impl->world);
    w.m_parallel = false;
}

// ---- components ---------------------------------------------------------------

ComponentId World::resolveComponent(const ComponentDesc& desc, std::atomic<u64>& slot) const {
    ecs_world_t* w = m_impl->world;
    const std::string name(desc.name); // flecs needs a NUL-terminated string
    ecs_entity_t id = ecs_lookup_symbol(w, name.c_str(), false, false);

    if (id) {
        const ecs_type_info_t* info = ecs_get_type_info(w, id);
        const u32 size = info ? static_cast<u32>(info->size) : 0u;
        const u32 alignment = info ? static_cast<u32>(info->alignment) : 0u;
        if (size != desc.size || alignment != desc.alignment) {
            // The struct changed since this component was registered (typically a
            // hot reload after editing a component). Existing data has the old
            // layout, so binding the new code to it would corrupt memory.
            // TODO(reflection): migrate by field name instead of refusing.
            GTABR_ERROR("Component '{}' changed layout (was {}b/{}a, now {}b/{}a); restart required", desc.name,
                        size, alignment, desc.size, desc.alignment);
            GTABR_VERIFY(false, "component layout changed while the world holds data of the old layout");
            return 0;
        }
    } else {
        if (m_parallel) {
            GTABR_ERROR("Component '{}' is not registered; register it in plugin build() (App::registerComponent), "
                        "components cannot be created while systems run",
                        desc.name);
            GTABR_VERIFY(false, "component registered during the parallel phase");
            return 0;
        }
        ecs_entity_desc_t entity{};
        entity.name = name.c_str();
        entity.symbol = name.c_str();
        entity.sep = ""; // type names contain '::' and templates: no path splitting
        id = ecs_entity_init(w, &entity);
        if (desc.size > 0) {
            ecs_component_desc_t component{};
            component.entity = id;
            component.type.size = static_cast<ecs_size_t>(desc.size);
            component.type.alignment = static_cast<ecs_size_t>(desc.alignment);
            id = ecs_component_init(w, &component);
        }
    }
    GTABR_VERIFY(id != 0 && id <= 0xFFFFFFFFull, "component id does not fit the cache slot");
    slot.store((static_cast<u64>(m_uid) << 32) | id, std::memory_order_relaxed);
    return id;
}

// ---- entities -----------------------------------------------------------------

Entity World::spawn() {
    GTABR_VERIFY(!m_parallel, "spawn() during the parallel phase: use Commands");
    ecs_entity_desc_t desc{};
    const ecs_id_t add[] = {m_impl->spawnedTag, 0};
    desc.add = add;
    return Entity{ecs_entity_init(m_impl->world, &desc)};
}

bool World::isAlive(Entity entity) const {
    return entity.id != 0 && ecs_is_alive(m_impl->world, entity.id);
}

bool World::despawn(Entity entity) {
    GTABR_VERIFY(!m_parallel, "despawn() during the parallel phase: use Commands");
    // Only spawned entities: never let a stale handle delete a component entity.
    if (!isAlive(entity) || !ecs_has_id(m_impl->world, entity.id, m_impl->spawnedTag)) return false;
    ecs_delete(m_impl->world, entity.id);
    return true;
}

size_t World::entityCount() const {
    return static_cast<size_t>(ecs_count_id(m_impl->world, m_impl->spawnedTag));
}

void World::setComponent(Entity e, ComponentId component, const void* data, u32 size) {
    GTABR_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
    if (!isAlive(e) || component == 0) return;
    ecs_world_t* w = m_impl->world;
    if (size == 0) {
        ecs_add_id(w, e.id, component);
    } else if (data) {
        ecs_set_id(w, e.id, component, size, data);
    } else {
        ecs_ensure_id(w, e.id, component, size);
        ecs_modified_id(w, e.id, component);
    }
}

void World::removeComponent(Entity e, ComponentId component) {
    GTABR_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
    if (!isAlive(e) || component == 0) return;
    ecs_remove_id(m_impl->world, e.id, component);
}

bool World::hasComponent(Entity e, ComponentId component) const {
    return isAlive(e) && component != 0 && ecs_has_id(m_impl->world, e.id, component);
}

void* World::componentPtr(Entity e, ComponentId component) const {
    if (!isAlive(e) || component == 0) return nullptr;
    return ecs_get_mut_id(m_impl->world, e.id, component);
}

// ---- resources: a component stored on its own component entity -----------------

void World::setResource(ComponentId component, const void* data, u32 size) {
    GTABR_VERIFY(!m_parallel, "resource change during the parallel phase: use Commands");
    GTABR_VERIFY(component != 0 && size > 0, "invalid resource");
    ecs_set_id(m_impl->world, component, component, size, data);
}

void* World::resourcePtr(ComponentId component) const {
    return component ? ecs_get_mut_id(m_impl->world, component, component) : nullptr;
}

bool World::removeResource(ComponentId component) {
    GTABR_VERIFY(!m_parallel, "resource change during the parallel phase: use Commands");
    if (!component || !ecs_has_id(m_impl->world, component, component)) return false;
    ecs_remove_id(m_impl->world, component, component);
    return true;
}

// ---- events --------------------------------------------------------------------

EventChannel& World::eventChannel(u64 key, u32 elemSize) {
    auto it = m_impl->channels.find(key);
    if (it == m_impl->channels.end()) {
        GTABR_VERIFY(!m_parallel, "event channels cannot be created while systems run");
        it = m_impl->channels.emplace(key, std::make_unique<EventChannel>(elemSize)).first;
    }
    GTABR_VERIFY(it->second->elemSize() == elemSize, "event type changed size: restart required");
    return *it->second;
}

void World::advanceEvents() {
    for (auto& [key, channel] : m_impl->channels) channel->advance();
}

} // namespace gtabr
