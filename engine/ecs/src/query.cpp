#include "gtabr/ecs/query.h"

#include <new>

#include "gtabr/assert.h"
#include "gtabr/jobs.h"
#include "world_impl.h"

namespace gtabr::detail {

struct QueryCore {
    ecs_world_t* world;
    ecs_query_t* query;
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
    GTABR_VERIFY(!world.inParallelPhase(), "queries cannot be created while systems run");
    GTABR_VERIFY(count > 0 && count <= FLECS_TERM_COUNT_MAX, "invalid query term count");

    ecs_query_desc_t desc{};
    for (u32 i = 0; i < count; ++i) {
        ecs_term_t& term = desc.terms[i];
        term.id = terms[i].id;
        switch (terms[i].kind) {
            case TermKind::Read: term.inout = EcsIn; break;
            case TermKind::Write: term.inout = EcsInOut; break;
            case TermKind::With: term.inout = EcsInOutNone; break;
            case TermKind::Without: term.oper = EcsNot; break;
        }
    }
    // Cached: the set of matching tables is maintained when tables are created
    // (a structural change, so on the main thread). Iterating from worker threads
    // then only reads it, which an uncached query could not guarantee.
    desc.cache_kind = EcsQueryCacheAuto;

    ecs_world_t* raw = WorldAccess::raw(world);
    ecs_query_t* query = ecs_query_init(raw, &desc);
    GTABR_VERIFY(query != nullptr, "ecs_query_init failed");
    if (!query) return nullptr;
    return QueryHandle(new QueryCore{raw, query});
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

} // namespace gtabr::detail
