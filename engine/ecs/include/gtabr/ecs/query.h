#pragma once

#include <array>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "gtabr/ecs/access.h"
#include "gtabr/ecs/world.h"

namespace gtabr {

class JobSystem;

// Everything a running system needs from the scheduler. Lives on the stack of
// the thread that runs the system.
struct RunContext {
    World* world;
    void* stage;     // backend stage of the executing thread (opaque)
    JobSystem* jobs; // may be null: parallel helpers then run inline
};

// Query filters: match entities that have / lack T without touching its data.
template <class T>
struct With {};
template <class T>
struct Without {};

namespace detail {

struct QueryCore; // engine-private (flecs query + term table)
enum class TermKind : u8 { Read, Write, With, Without };
struct TermDesc {
    ComponentId id;
    TermKind kind;
};

struct QueryCoreDeleter {
    GTABR_API void operator()(QueryCore* core) const;
};
using QueryHandle = std::unique_ptr<QueryCore, QueryCoreDeleter>;
// Not callable during the parallel phase (registers a query in the backend).
GTABR_API QueryHandle createQuery(World& world, const TermDesc* terms, u32 count);

// Large enough for two backend iterators (a slice keeps its parent next to it,
// and neither may move). Checked by a static_assert in query.cpp.
inline constexpr size_t kQueryIterStorage = 1024;

// Walks the matching tables one at a time. Hot-loop data (columns) is read in
// the caller's template code; only one call per table crosses into the engine.
class GTABR_API QueryIter {
public:
    QueryIter(const QueryCore& core, void* stage);
    // Worker slice: returns roughly 1/workerCount of every matched table.
    QueryIter(const QueryCore& core, void* stage, u32 workerIndex, u32 workerCount);
    ~QueryIter();
    QueryIter(const QueryIter&) = delete;
    QueryIter& operator=(const QueryIter&) = delete;

    bool next();
    u32 count() const { return m_count; }
    const Entity* entities() const { return m_entities; }
    void* field(u32 term, u32 elemSize) const;

private:
    alignas(16) unsigned char m_storage[kQueryIterStorage];
    const Entity* m_entities = nullptr;
    u32 m_count = 0;
    bool m_worker = false;
    bool m_active = true;
};

using ParallelQueryFn = void (*)(void* arg, QueryIter& slice);
// Runs `fn` once per job-system thread, each on its own slice of the query, and
// returns when all are done. Inline (one full-query slice) without a job system.
GTABR_API void parallelQuery(const QueryCore& core, const RunContext& ctx, ParallelQueryFn fn, void* arg);

template <class T>
struct TermInfo;
template <class C>
struct TermInfo<C&> {
    static_assert(!std::is_empty_v<std::remove_const_t<C>>, "use With<T> to match a tag component");
    using Component = std::remove_const_t<C>;
    static constexpr bool kIsData = true;
    static constexpr bool kIsEntity = false;
    static constexpr bool kIsFlecsTerm = true;
    static constexpr TermKind kKind = std::is_const_v<C> ? TermKind::Read : TermKind::Write;
};
template <class C>
struct TermInfo<With<C>> {
    using Component = C;
    static constexpr bool kIsData = false;
    static constexpr bool kIsEntity = false;
    static constexpr bool kIsFlecsTerm = true;
    static constexpr TermKind kKind = TermKind::With;
};
template <class C>
struct TermInfo<Without<C>> {
    using Component = C;
    static constexpr bool kIsData = false;
    static constexpr bool kIsEntity = false;
    static constexpr bool kIsFlecsTerm = true;
    static constexpr TermKind kKind = TermKind::Without;
};
template <>
struct TermInfo<Entity> {
    static constexpr bool kIsData = true;
    static constexpr bool kIsEntity = true;
    static constexpr bool kIsFlecsTerm = false;
};

// Appends T's backend term (if it has one) and records the access it implies.
template <class T>
void addTerm(World& world, TermDesc*& out, AccessSet* access) {
    using Info = TermInfo<T>;
    if constexpr (Info::kIsFlecsTerm) {
        const ComponentId id = world.component<typename Info::Component>();
        *out++ = {id, Info::kKind};
        if (access) {
            if (Info::kKind == TermKind::Read) access->read(id);
            if (Info::kKind == TermKind::Write) access->write(id);
        }
    }
}

} // namespace detail

// System parameter and plain handle for iterating entities that have all of the
// listed components. Terms are:
//     const T&     read T             T&          read and write T
//     With<T>      must have T        Without<T>  must not have T
//     Entity       the entity itself (by value)
// each() calls f with one argument per T&/const T&/Entity term, in order.
//
// Thread-safety: a Query handed to a system is only valid inside that system.
// each() is single-threaded; parEach() splits the work across the job system and
// requires f to be thread-safe (it may write only through its component
// arguments, which are disjoint per entity). Structural changes go through
// Commands, never through the query.
template <class... Terms>
class Query {
    static_assert(sizeof...(Terms) > 0, "a query needs at least one term");
    using Tuple = std::tuple<Terms...>;
    template <size_t I>
    using Nth = std::tuple_element_t<I, Tuple>;
    static constexpr size_t kTermCount = sizeof...(Terms);

    // Backend term index of each query term (entity terms have none).
    static constexpr std::array<u32, kTermCount> kFieldIndex = [] {
        std::array<u32, kTermCount> a{};
        constexpr bool isTerm[] = {detail::TermInfo<Terms>::kIsFlecsTerm...};
        u32 next = 0;
        for (size_t i = 0; i < kTermCount; ++i) a[i] = isTerm[i] ? next++ : 0;
        return a;
    }();
    // Indices (into Terms) of the terms that become callback arguments.
    static constexpr auto kArgs = [] {
        std::array<size_t, kTermCount> idx{};
        size_t n = 0;
        constexpr bool isArg[] = {detail::TermInfo<Terms>::kIsData...};
        for (size_t i = 0; i < kTermCount; ++i)
            if (isArg[i]) idx[n++] = i;
        return std::pair{idx, n};
    }();

public:
    Query(const detail::QueryCore& core, const RunContext& ctx) : m_core(&core), m_ctx(&ctx) {}

    template <class F>
    void each(F&& f) const {
        detail::QueryIter it(*m_core, m_ctx->stage);
        run(it, f);
    }

    template <class F>
    void parEach(F&& f) const {
        struct Job {
            const Query* self;
            std::remove_reference_t<F>* fn;
        } job{this, &f};
        detail::parallelQuery(
            *m_core, *m_ctx,
            [](void* arg, detail::QueryIter& slice) {
                auto* j = static_cast<Job*>(arg);
                j->self->run(slice, *j->fn);
            },
            &job);
    }

private:
    template <size_t I>
    static decltype(auto) arg(const std::array<void*, kTermCount>& cols, const Entity* entities, u32 row) {
        if constexpr (detail::TermInfo<Nth<I>>::kIsEntity) {
            return entities[row];
        } else {
            using C = std::remove_reference_t<Nth<I>>;
            return static_cast<C*>(cols[I])[row];
        }
    }

    template <class F>
    void run(detail::QueryIter& it, F& f) const {
        while (it.next()) {
            const u32 n = it.count();
            const Entity* entities = it.entities();
            std::array<void*, kTermCount> cols{};
            [&]<size_t... I>(std::index_sequence<I...>) {
                ((cols[I] = column<I>(it)), ...);
            }(std::make_index_sequence<kTermCount>{});
            [&]<size_t... K>(std::index_sequence<K...>) {
                for (u32 row = 0; row < n; ++row) f(arg<kArgs.first[K]>(cols, entities, row)...);
            }(std::make_index_sequence<kArgs.second>{});
        }
    }

    template <size_t I>
    static void* column(detail::QueryIter& it) {
        using Info = detail::TermInfo<Nth<I>>;
        if constexpr (Info::kIsData && !Info::kIsEntity) {
            return it.field(kFieldIndex[I], sizeof(typename Info::Component));
        } else {
            return nullptr;
        }
    }

    const detail::QueryCore* m_core;
    const RunContext* m_ctx;
};

// A query owned by plain code (tools, tests, the editor): not a system parameter.
// Must be created and destroyed outside the parallel phase.
template <class... Terms>
class OwnedQuery {
public:
    explicit OwnedQuery(World& world) : m_world(&world), m_ctx{&world, nullptr, nullptr} {
        std::array<detail::TermDesc, sizeof...(Terms)> terms{};
        detail::TermDesc* out = terms.data();
        (detail::addTerm<Terms>(world, out, nullptr), ...);
        m_core = detail::createQuery(world, terms.data(), static_cast<u32>(out - terms.data()));
        // m_ctx.stage stays null: the engine then iterates on the world itself.
    }
    template <class F>
    void each(F&& f) const { Query<Terms...>(*m_core, m_ctx).each(std::forward<F>(f)); }
    template <class F>
    void parEach(F&& f) const { Query<Terms...>(*m_core, m_ctx).parEach(std::forward<F>(f)); }

private:
    World* m_world;
    detail::QueryHandle m_core;
    RunContext m_ctx;
};

} // namespace gtabr
