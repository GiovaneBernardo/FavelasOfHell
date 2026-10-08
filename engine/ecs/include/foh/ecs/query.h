#pragma once

#include <array>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "foh/ecs/access.h"
#include "foh/ecs/world.h"

namespace foh {

class JobSystem;

// Everything a running system needs from the scheduler. Lives on the stack of
// the thread that runs the system.
struct RunContext {
    World* world = nullptr;
    void* stage = nullptr;     // backend stage of the executing thread (opaque)
    JobSystem* jobs = nullptr; // may be null: parallel helpers then run inline
    // Set only while an observer runs: the entity and component value that fired it.
    Entity triggerEntity{};
    const void* triggerValue = nullptr;
};

// Query filters: match entities that have / lack T without touching its data.
template <class T>
struct With {};
template <class T>
struct Without {};

// Relationship term. As a data term it yields the entity's parent:
//     Query<Position&, Parent>   ->   f(Position&, Entity parent)
// With<Parent> / Without<Parent> keep entities that have / lack a parent (roots).
struct Parent {};

// Query modifier: only visit tables that something wrote to since this query last
// visited them. A table counts as written when a system iterated it with write
// access or a component was set/added/removed on it, so this can say "changed"
// when nothing really did (never the opposite). It works per archetype table, not
// per entity or per component, and each query has its own memory of what it saw.
// Typical use: skip the huge static part of the world when recomputing derived data.
// Not allowed with parEach().
struct ChangedTables {};

namespace detail {

struct QueryCore; // engine-private (flecs query + term table)
enum class TermKind : u8 { Read, Write, With, Without, ParentData, ParentWith, ParentWithout, ChangedTables };
struct TermDesc {
    ComponentId id;
    TermKind kind;
};

struct QueryCoreDeleter {
    FOH_API void operator()(QueryCore* core) const;
};
using QueryHandle = std::unique_ptr<QueryCore, QueryCoreDeleter>;
// Not callable during the parallel phase (registers a query in the backend).
FOH_API QueryHandle createQuery(World& world, const TermDesc* terms, u32 count);

// Large enough for two backend iterators (a slice keeps its parent next to it,
// and neither may move). Checked by a static_assert in query.cpp.
inline constexpr size_t kQueryIterStorage = 1024;

// Walks the matching tables one at a time. Hot-loop data (columns) is read in
// the caller's template code; only one call per table crosses into the engine.
class FOH_API QueryIter {
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
    // Sparse components have no column: they are looked up per row.
    void* fieldAt(u32 term, u32 elemSize, u32 row) const;
    // The target of the relationship matched by `term` for the current table.
    Entity pairTarget(u32 term) const;
    // For queries built with ChangedTables: was the current table written since
    // this query last visited it? skip() must be called for tables that are skipped.
    bool changed() const;
    void skip();

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
FOH_API void parallelQuery(const QueryCore& core, const RunContext& ctx, ParallelQueryFn fn, void* arg);

// How each kind of query term maps to a backend term and to a callback argument.
//   kIsArg      becomes an argument of the each() callback
//   kIsSelf     the entity itself (no backend term)
//   kIsParent   the parent entity (a per-table value)
//   kIsTerm     has a backend term (and therefore a field index)
//   kIsColumn   its data is a table column read by pointer
//   kIsSparse   its data is looked up per row (sparse component)
//   kIsChanged  the ChangedTables modifier
struct TermDefaults {
    static constexpr bool kIsArg = false, kIsSelf = false, kIsParent = false, kIsTerm = false, kIsColumn = false,
                          kIsSparse = false, kIsChanged = false;
};
template <class T>
struct TermInfo;
template <class C>
struct TermInfo<C&> : TermDefaults {
    static_assert(!std::is_empty_v<std::remove_const_t<C>>, "use With<T> to match a tag component");
    using Component = std::remove_const_t<C>;
    static constexpr bool kIsArg = true, kIsTerm = true, kIsSparse = isSparse<Component>(),
                          kIsColumn = !isSparse<Component>();
    static TermDesc term(World& w) {
        return {w.component<Component>(), std::is_const_v<C> ? TermKind::Read : TermKind::Write};
    }
};
template <class C>
struct TermInfo<With<C>> : TermDefaults {
    static constexpr bool kIsTerm = true;
    static TermDesc term(World& w) { return {w.component<C>(), TermKind::With}; }
};
template <class C>
struct TermInfo<Without<C>> : TermDefaults {
    static constexpr bool kIsTerm = true;
    static TermDesc term(World& w) { return {w.component<C>(), TermKind::Without}; }
};
template <>
struct TermInfo<With<Parent>> : TermDefaults {
    static constexpr bool kIsTerm = true;
    static TermDesc term(World&) { return {0, TermKind::ParentWith}; }
};
template <>
struct TermInfo<Without<Parent>> : TermDefaults {
    static constexpr bool kIsTerm = true;
    static TermDesc term(World&) { return {0, TermKind::ParentWithout}; }
};
template <>
struct TermInfo<Parent> : TermDefaults {
    static constexpr bool kIsArg = true, kIsParent = true, kIsTerm = true;
    static TermDesc term(World&) { return {0, TermKind::ParentData}; }
};
template <>
struct TermInfo<Entity> : TermDefaults {
    static constexpr bool kIsArg = true, kIsSelf = true;
};
template <>
struct TermInfo<ChangedTables> : TermDefaults {
    static constexpr bool kIsChanged = true;
    static TermDesc term(World&) { return {0, TermKind::ChangedTables}; }
};

// Appends T's backend term (if it has one) and records the access it implies.
template <class T>
void addTerm(World& world, TermDesc*& out, AccessSet* access) {
    using Info = TermInfo<T>;
    if constexpr (Info::kIsTerm || Info::kIsChanged) {
        const TermDesc desc = Info::term(world);
        *out++ = desc;
        if (access) {
            if (desc.kind == TermKind::Read) access->read(desc.id);
            if (desc.kind == TermKind::Write) access->write(desc.id);
        }
    }
}

} // namespace detail

// System parameter and plain handle for iterating entities that have all of the
// listed components. Terms are:
//     const T&     read T             T&          read and write T
//     With<T>      must have T        Without<T>  must not have T
//     Entity       the entity itself (by value)
//     Parent       its parent entity (by value); With<Parent>/Without<Parent> filter
//     ChangedTables  only tables written since this query last saw them
// each() calls f with one argument per T&/const T&/Entity/Parent term, in order.
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
    template <size_t I>
    using Info = detail::TermInfo<Nth<I>>;
    static constexpr size_t kTermCount = sizeof...(Terms);
    static constexpr bool kDetectChanges = (detail::TermInfo<Terms>::kIsChanged || ...);

    // Backend term index of each query term (entity terms have none).
    static constexpr std::array<u32, kTermCount> kFieldIndex = [] {
        std::array<u32, kTermCount> a{};
        constexpr bool isTerm[] = {detail::TermInfo<Terms>::kIsTerm...};
        u32 next = 0;
        for (size_t i = 0; i < kTermCount; ++i) a[i] = isTerm[i] ? next++ : 0;
        return a;
    }();
    // Indices (into Terms) of the terms that become callback arguments.
    static constexpr auto kArgs = [] {
        std::array<size_t, kTermCount> idx{};
        size_t n = 0;
        constexpr bool isArg[] = {detail::TermInfo<Terms>::kIsArg...};
        for (size_t i = 0; i < kTermCount; ++i)
            if (isArg[i]) idx[n++] = i;
        return std::pair{idx, n};
    }();

    // Per-table values that are not component columns.
    struct TableData {
        std::array<void*, kTermCount> cols{};
        std::array<Entity, kTermCount> parents{};
        const Entity* entities = nullptr;
        const detail::QueryIter* it = nullptr; // for sparse components
    };

public:
    Query(const detail::QueryCore& core, const RunContext& ctx) : m_core(&core), m_ctx(&ctx) {}

    template <class F>
    void each(F&& f) const {
        detail::QueryIter it(*m_core, m_ctx->stage);
        run(it, f);
    }

    template <class F>
    void parEach(F&& f) const {
        static_assert(!kDetectChanges, "ChangedTables keeps per-query state: it cannot be used with parEach");
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
    static decltype(auto) arg(const TableData& t, u32 row) {
        if constexpr (Info<I>::kIsSelf) {
            return t.entities[row];
        } else if constexpr (Info<I>::kIsParent) {
            return t.parents[I];
        } else if constexpr (Info<I>::kIsSparse) {
            using C = std::remove_reference_t<Nth<I>>;
            return *static_cast<C*>(t.it->fieldAt(kFieldIndex[I], sizeof(typename Info<I>::Component), row));
        } else {
            using C = std::remove_reference_t<Nth<I>>;
            return static_cast<C*>(t.cols[I])[row];
        }
    }

    template <size_t I>
    static void fill(detail::QueryIter& it, TableData& t) {
        if constexpr (Info<I>::kIsColumn) {
            t.cols[I] = it.field(kFieldIndex[I], sizeof(typename Info<I>::Component));
        } else if constexpr (Info<I>::kIsParent) {
            t.parents[I] = it.pairTarget(kFieldIndex[I]);
        }
    }

    template <class F>
    void run(detail::QueryIter& it, F& f) const {
        while (it.next()) {
            if constexpr (kDetectChanges) {
                if (!it.changed()) {
                    it.skip();
                    continue;
                }
            }
            const u32 n = it.count();
            TableData table;
            table.entities = it.entities();
            table.it = &it;
            [&]<size_t... I>(std::index_sequence<I...>) {
                (fill<I>(it, table), ...);
            }(std::make_index_sequence<kTermCount>{});
            [&]<size_t... K>(std::index_sequence<K...>) {
                for (u32 row = 0; row < n; ++row) f(arg<kArgs.first[K]>(table, row)...);
            }(std::make_index_sequence<kArgs.second>{});
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

} // namespace foh
