#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "foh/assert.h"
#include "foh/ecs/world.h"
#include "foh/log.h"
#include "schema_ops.h"
#include "world_impl.h"

namespace foh {

namespace {
u32 nextWorldUid() {
    static std::atomic<u32> counter{0};
    return ++counter; // starts at 1: a zeroed cache slot never matches a world
}

// Remove hook for components that own buffers: frees them when the component is
// removed or the entity dies. Engine code only; never a pointer into a game module.
void onRemoveReleaseBuffers(ecs_iter_t* it) {
    auto* impl = static_cast<World::Impl*>(it->ctx);
    const auto found = impl->components.find(it->event_id);
    if (found == impl->components.end()) return;
    const size_t size = static_cast<size_t>(it->sizes[0]);
    const bool sparse = (it->row_fields & 1u) != 0; // sparse storage is addressed per row
    auto* column = sparse ? nullptr : static_cast<std::byte*>(ecs_field_w_size(it, size, 0));
    for (i32 i = 0; i < it->count; ++i) {
        std::byte* value = nullptr;
        if (sparse) value = static_cast<std::byte*>(ecs_field_at_w_size(it, size, 0, i));
        else if (column) value = column + static_cast<size_t>(i) * size;
        if (value) schema::releaseBuffers(impl->buffers, found->second.schema.fields, value);
    }
}
} // namespace

World::World() : m_impl(std::make_unique<Impl>()), m_uid(nextWorldUid()) {
    // ecs_mini: no addon modules are imported (no pipeline, no timers...). The
    // scheduler is ours; addons are imported explicitly when a feature needs one.
    m_impl->world = ecs_mini();
    ecs_set_ctx(m_impl->world, m_impl.get(), nullptr);
    ecs_entity_desc_t tag{};
    tag.name = "foh_Spawned";
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
    FOH_VERIFY(slot < w.m_impl->stageCount, "thread slot has no ECS stage: App stage count must cover the job system");
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

void World::Impl::registerInfo(ecs_entity_t id, Schema schema, bool sparse) {
    ComponentInfo info;
    info.hasBuffers = schema::hasBuffers(schema.fields);
    info.sparse = sparse;
    info.schema = std::move(schema);
    if (info.hasBuffers && info.schema.size > 0) {
        ecs_type_hooks_t hooks{};
        hooks.on_remove = onRemoveReleaseBuffers;
        hooks.ctx = this;
        ecs_set_hooks_id(world, id, &hooks);
    }
    components[id] = std::move(info);
}

ecs_entity_t World::Impl::createComponent(const std::string& name, const ComponentDesc& desc) {
    ecs_entity_desc_t entity{};
    entity.name = name.c_str();
    entity.symbol = name.c_str();
    entity.sep = "";          // type names contain '::' and templates: no path splitting
    entity.use_low_id = true; // components get low, stable ids like any flecs component
    ecs_entity_t id = ecs_entity_init(world, &entity);
    if (desc.size > 0) {
        ecs_component_desc_t component{};
        component.entity = id;
        component.type.size = static_cast<ecs_size_t>(desc.size);
        component.type.alignment = static_cast<ecs_size_t>(desc.alignment);
        id = ecs_component_init(world, &component);
    }
    if (desc.sparse) {
        // Own storage, and not part of the entity's archetype: toggling it is cheap.
        ecs_add_id(world, id, EcsSparse);
        ecs_add_id(world, id, EcsDontFragment);
    }
    return id;
}

ecs_entity_t World::Impl::migrate(ecs_entity_t oldId, const std::string& name, const ComponentDesc& desc,
                                  Schema fresh) {
    ComponentInfo old = std::move(components[oldId]);
    components.erase(oldId);
    FOH_INFO("Migrating component '{}' ({} -> {} bytes{})", name, old.schema.size, fresh.size,
             old.sparse != desc.sparse ? ", storage kind changed" : "");

    // Move the old definition out of the way so the new one can take the name.
    const std::string legacy = name + "#legacy";
    ecs_set_name(world, oldId, legacy.c_str());
    ecs_set_symbol(world, oldId, legacy.c_str());

    const ecs_entity_t newId = createComponent(name, desc);
    const Schema freshCopy = fresh; // registerInfo takes ownership below
    registerInfo(newId, std::move(fresh), desc.sparse);

    // Find every holder first, then rewrite: changing entities while iterating the
    // query would invalidate it.
    std::vector<ecs_entity_t> holders;
    {
        ecs_query_desc_t qd{};
        qd.terms[0].id = oldId;
        qd.cache_kind = EcsQueryCacheNone;
        ecs_query_t* q = ecs_query_init(world, &qd);
        ecs_iter_t it = ecs_query_iter(world, q);
        while (ecs_query_next(&it)) holders.insert(holders.end(), it.entities, it.entities + it.count);
        ecs_query_fini(q);
    }
    std::vector<std::byte> source(old.schema.size);
    std::vector<std::byte> converted(freshCopy.size);
    for (const ecs_entity_t holder : holders) {
        // A resource lives on its own component entity: it moves to the new one.
        const ecs_entity_t target = holder == oldId ? newId : holder;
        if (desc.size == 0) {
            ecs_add_id(world, target, newId);
        } else if (old.schema.size == 0) {
            ecs_ensure_id(world, target, newId, desc.size); // was a tag, now has data: zeroed
        } else {
            const void* current = ecs_get_id(world, holder, oldId);
            std::memcpy(source.data(), current, source.size());
            std::memset(converted.data(), 0, converted.size());
            schema::convertRecord(buffers, old.schema.fields, source.data(), freshCopy.fields, converted.data());
            ecs_set_id(world, target, newId, freshCopy.size, converted.data());
            // Buffer ownership moved into the new value; the old copy must not free it.
            if (old.hasBuffers) {
                if (auto* live = static_cast<std::byte*>(ecs_get_mut_id(world, holder, oldId))) {
                    schema::clearBufferHandles(old.schema.fields, live);
                }
            }
        }
        ecs_remove_id(world, holder, oldId);
    }
    ecs_delete(world, oldId);
    return newId;
}

ComponentId World::resolveComponent(const ComponentDesc& desc, std::atomic<u64>& slot) const {
    Impl& impl = *m_impl;
    ecs_world_t* w = impl.world;
    const std::string name(desc.name); // flecs needs a NUL-terminated string
    Schema fresh;
    desc.describe(fresh);
    if (!fresh.reflected && impl.warnedUnreflected.insert(name).second) {
        FOH_WARN("Component '{}' has no FOH_REFLECT: it works, but cannot be saved or migrated", name);
    }

    ecs_entity_t id = ecs_lookup_symbol(w, name.c_str(), false, false);
    const auto known = id ? impl.components.find(id) : impl.components.end();

    if (id && known != impl.components.end()) {
        const Schema& old = known->second.schema;
        const bool layoutChanged = !sameLayout(old, fresh);
        const bool storageChanged = known->second.sparse != desc.sparse;
        if (layoutChanged || storageChanged) {
            if (old.reflected && fresh.reflected && !m_parallel) {
                id = impl.migrate(id, name, desc, std::move(fresh));
            } else if (old.size != fresh.size || old.alignment != fresh.alignment || storageChanged) {
                // Without field descriptions there is no way to carry the old data
                // over, and binding new code to the old layout would corrupt memory.
                FOH_ERROR("Component '{}' changed ({}b/{}a -> {}b/{}a{}) and cannot be migrated "
                          "(it needs FOH_REFLECT, and must not change while systems run); restart required",
                          name, old.size, old.alignment, fresh.size, fresh.alignment,
                          storageChanged ? ", storage kind" : "");
                FOH_VERIFY(false, "component layout changed while the world holds data of the old layout");
                return 0;
            }
            // else: same size, no field info: assume compatible.
        }
    } else if (!id) {
        if (m_parallel) {
            FOH_ERROR("Component '{}' is not registered; register it in plugin build() (App::registerComponent), "
                      "components cannot be created while systems run",
                      name);
            FOH_VERIFY(false, "component registered during the parallel phase");
            return 0;
        }
        id = impl.createComponent(name, desc);
        impl.registerInfo(id, std::move(fresh), desc.sparse);
    } else {
        // Known to the backend but not to us (created by other engine code): adopt it.
        impl.registerInfo(id, std::move(fresh), desc.sparse);
    }
    FOH_VERIFY(id != 0, "component registration failed");
    // The cache slot holds a dense index: a backend id may use all 64 bits.
    const auto [position, inserted] = impl.denseIndex.try_emplace(id, static_cast<u32>(m_componentIds.size()));
    if (inserted) m_componentIds.push_back(id);
    slot.store((static_cast<u64>(m_uid) << 32) | position->second, std::memory_order_relaxed);
    return id;
}

size_t World::archetypeCount() const {
    return static_cast<size_t>(ecs_get_world_info(m_impl->world)->table_count);
}

const Schema* World::schema(ComponentId component) const {
    const auto it = m_impl->components.find(component);
    return it == m_impl->components.end() ? nullptr : &it->second.schema;
}

BufferPool& World::buffers() { return m_impl->buffers; }
const BufferPool& World::buffers() const { return m_impl->buffers; }

// ---- entities -----------------------------------------------------------------

Entity World::spawn() {
    FOH_VERIFY(!m_parallel, "spawn() during the parallel phase: use Commands");
    ecs_entity_desc_t desc{};
    const ecs_id_t add[] = {m_impl->spawnedTag, 0};
    desc.add = add;
    return Entity{ecs_entity_init(m_impl->world, &desc)};
}

bool World::isAlive(Entity entity) const {
    return entity.id != 0 && ecs_is_alive(m_impl->world, entity.id);
}

bool World::despawn(Entity entity) {
    FOH_VERIFY(!m_parallel, "despawn() during the parallel phase: use Commands");
    // Only spawned entities: never let a stale handle delete a component entity.
    if (!isAlive(entity) || !ecs_has_id(m_impl->world, entity.id, m_impl->spawnedTag)) return false;
    ecs_delete(m_impl->world, entity.id);
    return true;
}

size_t World::entityCount() const {
    return static_cast<size_t>(ecs_count_id(m_impl->world, m_impl->spawnedTag));
}

void World::setComponent(Entity e, ComponentId component, const void* data, u32 size) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
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

void World::markModified(Entity e, ComponentId component) {
    FOH_VERIFY(!m_parallel, "markModified during the parallel phase");
    if (!isAlive(e) || component == 0 || !ecs_has_id(m_impl->world, e.id, component)) return;
    ecs_modified_id(m_impl->world, e.id, component);
}

void World::removeComponent(Entity e, ComponentId component) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
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

// ---- relationships ----------------------------------------------------------------

void World::setParent(Entity child, Entity parent) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
    if (!isAlive(child) || !isAlive(parent) || child == parent) return;
    ecs_add_id(m_impl->world, child.id, ecs_pair(EcsChildOf, parent.id)); // ChildOf is exclusive: replaces
}

void World::clearParent(Entity child) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
    if (!isAlive(child)) return;
    ecs_remove_id(m_impl->world, child.id, ecs_pair(EcsChildOf, EcsWildcard));
}

Entity World::parentOf(Entity child) const {
    return isAlive(child) ? Entity{ecs_get_parent(m_impl->world, child.id)} : kNullEntity;
}

size_t World::childCount(Entity parent) const {
    if (!isAlive(parent)) return 0;
    return static_cast<size_t>(ecs_count_id(m_impl->world, ecs_pair(EcsChildOf, parent.id)));
}

void World::eachChild(Entity parent, ChildFn fn, void* ctx) const {
    if (!isAlive(parent)) return;
    ecs_iter_t it = ecs_children(m_impl->world, parent.id);
    while (ecs_children_next(&it)) {
        for (i32 i = 0; i < it.count; ++i) fn(ctx, Entity{it.entities[i]});
    }
}

void World::addPair(Entity e, ComponentId relation, Entity target) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
    if (!isAlive(e) || !isAlive(target) || relation == 0) return;
    ecs_add_id(m_impl->world, e.id, ecs_pair(relation, target.id));
}

void World::removePair(Entity e, ComponentId relation, Entity target) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase: use Commands");
    if (!isAlive(e) || relation == 0) return;
    ecs_remove_id(m_impl->world, e.id, ecs_pair(relation, target.id));
}

bool World::hasPair(Entity e, ComponentId relation, Entity target) const {
    return isAlive(e) && relation != 0 && ecs_has_id(m_impl->world, e.id, ecs_pair(relation, target.id));
}

Entity World::pairTarget(Entity e, ComponentId relation, u32 index) const {
    if (!isAlive(e) || relation == 0) return kNullEntity;
    return Entity{ecs_get_target(m_impl->world, e.id, relation, static_cast<i32>(index))};
}

void World::cascadeDelete(ComponentId relation) {
    FOH_VERIFY(!m_parallel, "structural change during the parallel phase");
    if (relation) ecs_add_id(m_impl->world, relation, ecs_pair(EcsOnDeleteTarget, EcsDelete));
}

// ---- entity id reservation ----------------------------------------------------------

Entity World::reserveEntity() {
    FOH_VERIFY(!m_parallel, "reserveEntity() during the parallel phase");
    return Entity{ecs_new(m_impl->world)};
}

void World::adoptEntity(Entity reserved) {
    FOH_VERIFY(!m_parallel, "adoptEntity() during the parallel phase");
    if (isAlive(reserved)) ecs_add_id(m_impl->world, reserved.id, m_impl->spawnedTag);
}

void World::discardReserved(Entity reserved) {
    FOH_VERIFY(!m_parallel, "discardReserved() during the parallel phase");
    if (isAlive(reserved)) ecs_delete(m_impl->world, reserved.id);
}

// ---- resources: a component stored on its own component entity -----------------

void World::setResource(ComponentId component, const void* data, u32 size) {
    FOH_VERIFY(!m_parallel, "resource change during the parallel phase: use Commands");
    FOH_VERIFY(component != 0 && size > 0, "invalid resource");
    ecs_set_id(m_impl->world, component, component, size, data);
}

void* World::resourcePtr(ComponentId component) const {
    return component ? ecs_get_mut_id(m_impl->world, component, component) : nullptr;
}

bool World::removeResource(ComponentId component) {
    FOH_VERIFY(!m_parallel, "resource change during the parallel phase: use Commands");
    if (!component || !ecs_has_id(m_impl->world, component, component)) return false;
    ecs_remove_id(m_impl->world, component, component);
    return true;
}

// ---- events --------------------------------------------------------------------

EventChannel& World::eventChannel(u64 key, u32 elemSize) {
    auto it = m_impl->channels.find(key);
    if (it == m_impl->channels.end()) {
        FOH_VERIFY(!m_parallel, "event channels cannot be created while systems run");
        it = m_impl->channels.emplace(key, std::make_unique<EventChannel>(elemSize)).first;
    }
    FOH_VERIFY(it->second->elemSize() == elemSize, "event type changed size: restart required");
    return *it->second;
}

void World::advanceEvents() {
    for (auto& [key, channel] : m_impl->channels) channel->advance();
}

} // namespace foh
