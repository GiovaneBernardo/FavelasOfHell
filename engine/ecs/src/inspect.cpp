#include "foh/ecs/inspect.h"

#include <algorithm>

#include "world_impl.h"

namespace foh {

namespace {

void sortByName(const World::Impl& impl, std::vector<ComponentId>& ids) {
    std::sort(ids.begin(), ids.end(), [&](ComponentId a, ComponentId b) {
        return impl.components.at(a).schema.name < impl.components.at(b).schema.name;
    });
}

} // namespace

void listComponents(const World& world, std::vector<ComponentInfoView>& out) {
    const World::Impl& impl = WorldAccess::impl(world);
    out.clear();
    for (const auto& [id, info] : impl.components) out.push_back({id, &info.schema});
    std::sort(out.begin(), out.end(),
              [](const ComponentInfoView& a, const ComponentInfoView& b) { return a.schema->name < b.schema->name; });
}

void findEntities(const World& world, std::span<const ComponentId> with, std::span<const ComponentId> without,
                  std::vector<Entity>& out) {
    World::Impl& impl = WorldAccess::impl(world);
    out.clear();
    ecs_query_desc_t qd{};
    qd.cache_kind = EcsQueryCacheNone;
    u32 n = 0;
    qd.terms[n++].id = impl.spawnedTag;
    constexpr u32 kMaxTerms = FLECS_TERM_COUNT_MAX;
    for (ComponentId c : with) {
        if (n >= kMaxTerms) break;
        qd.terms[n].id = c;
        qd.terms[n].inout = EcsInOutNone; // presence only, never touches the data
        ++n;
    }
    for (ComponentId c : without) {
        if (n >= kMaxTerms) break;
        qd.terms[n].id = c;
        qd.terms[n].oper = EcsNot;
        qd.terms[n].inout = EcsInOutNone;
        ++n;
    }
    ecs_query_t* q = ecs_query_init(impl.world, &qd);
    if (!q) return;
    ecs_iter_t it = ecs_query_iter(impl.world, q);
    while (ecs_query_next(&it)) {
        for (i32 i = 0; i < it.count; ++i) out.push_back(Entity{it.entities[i]});
    }
    ecs_query_fini(q);
}

void componentsOf(const World& world, Entity entity, std::vector<ComponentId>& out) {
    World::Impl& impl = WorldAccess::impl(world);
    out.clear();
    ecs_world_t* w = impl.world;
    if (!ecs_is_alive(w, entity.id)) return;
    const ecs_type_t* type = ecs_get_type(w, entity.id);
    for (i32 i = 0; type && i < type->count; ++i) {
        const ecs_id_t id = type->array[i];
        if (!ECS_IS_PAIR(id) && id != impl.spawnedTag && impl.components.count(id)) out.push_back(id);
    }
    // Sparse components are not part of the entity type: ask for each one.
    for (const auto& [id, info] : impl.components) {
        if (info.sparse && ecs_has_id(w, entity.id, id)) out.push_back(id);
    }
    sortByName(impl, out);
}

} // namespace foh
