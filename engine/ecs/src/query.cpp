#include "foh/ecs/query.h"

#include <new>

#include "foh/assert.h"
#include "foh/jobs.h"
#include "world_impl.h"

namespace foh::detail {

struct QueryCore {
    ecs_world_t* world;
    ecs_query_t* query;
    bool detectsChanges;
};

namespace {

// A worker slice stores its parent iterator next to it: the backend keeps a
// pointer to the parent, so neither may ever move.
constexpr size_t kIterSize = (sizeof(ecs_iter_t) + 15) & ~size_t{15};
static_assert(2 * kIterSize <= kQueryIterStorage, "raise kQueryIterStorage in query.h");
static_assert(alignof(ecs_iter_t) <= 16);

} // namespace

void QueryCoreDeleter::operator()(QueryCore* core) const {
    if (!core) return;
    ecs_query_fini(core->query);
    delete core;
}

QueryHandle createQuery(World& world, const TermDesc* terms, u32 count) {
    FOH_VERIFY(!world.inParallelPhase(), "queries cannot be created while systems run");
    FOH_VERIFY(count > 0 && count <= FLECS_TERM_COUNT_MAX + 1, "invalid query term count");

    ecs_query_desc_t desc{};
    bool detectsChanges = false;
    u32 next = 0; // index of the backend term being filled (modifiers have none)
    for (u32 i = 0; i < count; ++i) {
        if (terms[i].kind == TermKind::ChangedTables) {
            detectsChanges = true;
            continue;
        }
        FOH_VERIFY(next < FLECS_TERM_COUNT_MAX, "too many query terms");
        ecs_term_t& term = desc.terms[next++];
        term.id = terms[i].id;
        switch (terms[i].kind) {
            case TermKind::ChangedTables: break; // handled above
            case TermKind::Read: term.inout = EcsIn; break;
            case TermKind::Write: term.inout = EcsInOut; break;
            case TermKind::With: term.inout = EcsInOutNone; break;
            case TermKind::Without: term.oper = EcsNot; break;
            case TermKind::ParentData:
                term.id = ecs_pair(EcsChildOf, EcsWildcard);
                term.inout = EcsInOutNone;
                break;
            case TermKind::ParentWith:
                term.id = ecs_pair(EcsChildOf, EcsWildcard);
                term.inout = EcsInOutNone;
                break;
            case TermKind::ParentWithout:
                term.id = ecs_pair(EcsChildOf, EcsWildcard);
                term.oper = EcsNot;
                break;
        }
    }
    // Cached: the set of matching tables is maintained when tables are created
    // (a structural change, so on the main thread). Iterating from worker threads
    // then only reads it, which an uncached query could not guarantee.
    desc.cache_kind = EcsQueryCacheAuto;
    if (detectsChanges) desc.flags |= EcsQueryDetectChanges;
    FOH_VERIFY(next > 0, "a query needs at least one component term");

    ecs_world_t* raw = WorldAccess::raw(world);
    ecs_query_t* query = ecs_query_init(raw, &desc);
    FOH_VERIFY(query != nullptr, "ecs_query_init failed");
    if (!query) return nullptr;
    return QueryHandle(new QueryCore{raw, query, detectsChanges});
}

QueryIter::QueryIter(const QueryCore& core, void* stage) {
    ecs_world_t* w = stage ? static_cast<ecs_world_t*>(stage) : core.world;
    new (m_storage) ecs_iter_t(ecs_query_iter(w, core.query));
}

QueryIter::QueryIter(const QueryCore& core, void* stage, u32 workerIndex, u32 workerCount) : m_worker(true) {
    ecs_world_t* w = stage ? static_cast<ecs_world_t*>(stage) : core.world;
    auto* parent = new (m_storage) ecs_iter_t(ecs_query_iter(w, core.query));
    new (m_storage + kIterSize)
        ecs_iter_t(ecs_worker_iter(parent, static_cast<i32>(workerIndex), static_cast<i32>(workerCount)));
}

QueryIter::~QueryIter() {
    // Only an iteration abandoned half-way needs cleanup; a finished one already
    // released its resources when next() returned false.
    if (!m_active) return;
    auto* it = reinterpret_cast<ecs_iter_t*>(m_storage + (m_worker ? kIterSize : 0));
    ecs_iter_fini(it);
}

bool QueryIter::next() {
    auto* it = reinterpret_cast<ecs_iter_t*>(m_storage + (m_worker ? kIterSize : 0));
    if (!m_active || !(m_worker ? ecs_worker_next(it) : ecs_query_next(it))) {
        m_active = false;
        m_count = 0;
        m_entities = nullptr;
        return false;
    }
    m_count = static_cast<u32>(it->count);
    m_entities = reinterpret_cast<const Entity*>(it->entities);
    return true;
}

void* QueryIter::field(u32 term, u32 elemSize) const {
    auto* it = reinterpret_cast<const ecs_iter_t*>(m_storage + (m_worker ? kIterSize : 0));
    return ecs_field_w_size(it, elemSize, static_cast<i8>(term));
}

void* QueryIter::fieldAt(u32 term, u32 elemSize, u32 row) const {
    auto* it = reinterpret_cast<const ecs_iter_t*>(m_storage + (m_worker ? kIterSize : 0));
    return ecs_field_at_w_size(it, elemSize, static_cast<i8>(term), static_cast<i32>(row));
}

Entity QueryIter::pairTarget(u32 term) const {
    auto* it = reinterpret_cast<const ecs_iter_t*>(m_storage + (m_worker ? kIterSize : 0));
    const ecs_id_t id = ecs_field_id(it, static_cast<i8>(term));
    return Entity{ecs_pair_second(it->real_world, id)};
}

bool QueryIter::changed() const {
    auto* it = reinterpret_cast<ecs_iter_t*>(const_cast<unsigned char*>(m_storage) + (m_worker ? kIterSize : 0));
    return ecs_iter_changed(it);
}

void QueryIter::skip() {
    auto* it = reinterpret_cast<ecs_iter_t*>(m_storage + (m_worker ? kIterSize : 0));
    ecs_iter_skip(it);
}

namespace {

struct Slice {
    const QueryCore* core;
    const RunContext* ctx;
    ParallelQueryFn fn;
    void* arg;
    u32 index;
    u32 count;
};

void runSlice(void* p) {
    auto* s = static_cast<Slice*>(p);
    // The job may run on any pool thread: use THAT thread's stage, because
    // iterator scratch memory is per stage.
    void* stage = WorldAccess::stage(*s->ctx->world, JobSystem::currentThreadSlot());
    QueryIter it(*s->core, stage, s->index, s->count);
    s->fn(s->arg, it);
}

} // namespace

void parallelQuery(const QueryCore& core, const RunContext& ctx, ParallelQueryFn fn, void* arg) {
    const u32 count = ctx.jobs ? ctx.jobs->threadCount() : 1;
    if (count <= 1) {
        QueryIter it(core, ctx.stage);
        fn(arg, it);
        return;
    }
    Slice slices[kMaxJobThreads];
    JobCounter counter;
    for (u32 i = 0; i < count; ++i) {
        slices[i] = {&core, &ctx, fn, arg, i, count};
        ctx.jobs->submit(&runSlice, &slices[i], counter);
    }
    ctx.jobs->wait(counter);
}

} // namespace foh::detail
