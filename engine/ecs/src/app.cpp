#include "gtabr/ecs/app.h"

#include <algorithm>
#include <atomic>
#include <string>

#include "gtabr/assert.h"
#include "gtabr/jobs.h"
#include "gtabr/log.h"
#include "world_impl.h"

namespace gtabr {

namespace {

constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
constexpr const char* kStageNames[kStageCount] = {"Startup", "PreUpdate", "Update", "PostUpdate"};

struct SystemEntry {
    u64 id = 0;
    OwnerId owner = kEngineOwner;
    std::string name;
    std::uintptr_t key = 0; // the system function's address, for after()/before()
    void* state = nullptr;
    SystemVTable vtable{};
    AccessSet access;
    std::vector<std::uintptr_t> after;  // keys of systems that must run before this one
    std::vector<std::uintptr_t> before; // keys of systems that must run after this one
    bool runnable = true;               // per segment run: false if a dependency is missing
    bool missingReported = false;
};

// A run of systems with no exclusive system in between, executed as a graph.
struct Segment {
    App::Impl* impl = nullptr;
    std::vector<SystemEntry>* list = nullptr;
    bool exclusive = false;
    std::vector<u32> systems;                 // indices into the stage's system list, registration order
    std::vector<u32> depCount;                // per local node: unfinished predecessors at start
    std::vector<std::vector<u32>> dependents; // per local node: successors
    std::vector<u32> roots;
    std::unique_ptr<std::atomic<u32>[]> remaining; // per run
    JobCounter counter;

    struct NodeArg {
        Segment* segment;
        u32 local;
    };
    std::vector<NodeArg> nodeArgs;
};

struct StagePlan {
    bool dirty = true;
    std::vector<std::unique_ptr<Segment>> segments;
};

} // namespace

struct App::Impl {
    World* world = nullptr;
    JobSystem* jobs = nullptr;
    u64 nextId = 1;
    std::vector<SystemEntry> systems[kStageCount];
    StagePlan plans[kStageCount];

    ~Impl() {
        for (auto& list : systems) {
            for (SystemEntry& s : list) s.vtable.destroy(s.state);
        }
    }
};

namespace {

// ---- planning -----------------------------------------------------------------

bool reaches(const std::vector<std::vector<u32>>& edges, u32 from, u32 to) {
    std::vector<u32> stack{from};
    std::vector<char> seen(edges.size(), 0);
    while (!stack.empty()) {
        const u32 n = stack.back();
        stack.pop_back();
        if (n == to) return true;
        if (seen[n]) continue;
        seen[n] = 1;
        for (u32 next : edges[n]) stack.push_back(next);
    }
    return false;
}

bool hasCycle(const std::vector<std::vector<u32>>& edges) {
    std::vector<u32> indegree(edges.size(), 0);
    for (const auto& out : edges)
        for (u32 to : out) ++indegree[to];
    std::vector<u32> ready;
    for (u32 i = 0; i < edges.size(); ++i)
        if (indegree[i] == 0) ready.push_back(i);
    size_t processed = 0;
    while (!ready.empty()) {
        const u32 n = ready.back();
        ready.pop_back();
        ++processed;
        for (u32 to : edges[n])
            if (--indegree[to] == 0) ready.push_back(to);
    }
    return processed != edges.size();
}

void addEdge(std::vector<std::vector<u32>>& edges, u32 from, u32 to) {
    if (from == to) return;
    auto& out = edges[from];
    if (std::find(out.begin(), out.end(), to) == out.end()) out.push_back(to);
}

void finishSegment(Segment& seg, std::vector<std::vector<u32>> edges) {
    const size_t n = seg.systems.size();
    seg.depCount.assign(n, 0);
    for (const auto& out : edges)
        for (u32 to : out) ++seg.depCount[to];
    seg.roots.clear();
    for (u32 i = 0; i < n; ++i)
        if (seg.depCount[i] == 0) seg.roots.push_back(i);
    seg.dependents = std::move(edges);
    seg.remaining = std::make_unique<std::atomic<u32>[]>(n);
    seg.nodeArgs.resize(n);
    for (u32 i = 0; i < n; ++i) seg.nodeArgs[i] = {&seg, i};
}

void buildPlan(App::Impl& impl, size_t stageIndex) {
    std::vector<SystemEntry>& list = impl.systems[stageIndex];
    StagePlan& plan = impl.plans[stageIndex];
    plan.segments.clear();

    // Which segment each system lands in (to judge cross-segment constraints).
    std::vector<u32> segmentOf(list.size(), 0);
    std::vector<std::vector<u32>> groups;
    {
        std::vector<u32> run;
        auto flush = [&] {
            if (!run.empty()) groups.push_back(std::move(run));
            run.clear();
        };
        for (u32 i = 0; i < list.size(); ++i) {
            if (list[i].access.exclusive) {
                flush();
                groups.push_back({i});
            } else {
                run.push_back(i);
            }
        }
        flush();
        for (u32 g = 0; g < groups.size(); ++g)
            for (u32 i : groups[g]) segmentOf[i] = g;
    }

    for (u32 g = 0; g < groups.size(); ++g) {
        auto seg = std::make_unique<Segment>();
        seg->impl = &impl;
        seg->list = &list;
        seg->systems = groups[g];
        seg->exclusive = list[groups[g].front()].access.exclusive;
        const size_t n = seg->systems.size();
        std::vector<std::vector<u32>> edges(n);

        if (!seg->exclusive) {
            auto localOf = [&](u32 global) -> int {
                for (u32 i = 0; i < n; ++i)
                    if (seg->systems[i] == global) return static_cast<int>(i);
                return -1;
            };
            // Explicit constraints first: they define direction where it matters.
            auto resolve = [&](u32 local, std::uintptr_t key, bool otherRunsFirst) {
                bool found = false;
                for (u32 other = 0; other < list.size(); ++other) {
                    if (list[other].key != key || other == seg->systems[local]) continue;
                    found = true;
                    const int otherLocal = localOf(other);
                    if (otherLocal >= 0) {
                        if (otherRunsFirst) addEdge(edges, static_cast<u32>(otherLocal), local);
                        else addEdge(edges, local, static_cast<u32>(otherLocal));
                    } else if ((segmentOf[other] < g) != otherRunsFirst) {
                        // The other system sits across an exclusive barrier in the
                        // wrong direction; barriers keep registration order.
                        GTABR_WARN("{}: ordering between '{}' and '{}' contradicts the exclusive system between them; ignored",
                                   kStageNames[stageIndex], list[seg->systems[local]].name, list[other].name);
                    }
                }
                if (!found) {
                    GTABR_WARN("{}: '{}' is ordered against a system that is not registered; ignored",
                               kStageNames[stageIndex], list[seg->systems[local]].name);
                }
            };
            for (u32 i = 0; i < n; ++i) {
                for (std::uintptr_t key : list[seg->systems[i]].after) resolve(i, key, true);
                for (std::uintptr_t key : list[seg->systems[i]].before) resolve(i, key, false);
            }
            if (hasCycle(edges)) {
                GTABR_ERROR("{}: explicit system ordering contains a cycle; ignoring all explicit ordering in this segment",
                            kStageNames[stageIndex]);
                edges.assign(n, {});
            }
            // Conflicting pairs without an explicit relation run in registration
            // order. A pair already ordered (directly or transitively) is skipped,
            // which also keeps the graph acyclic.
            for (u32 i = 0; i < n; ++i) {
                for (u32 j = i + 1; j < n; ++j) {
                    if (!list[seg->systems[i]].access.conflictsWith(list[seg->systems[j]].access)) continue;
                    if (reaches(edges, i, j) || reaches(edges, j, i)) continue;
                    addEdge(edges, i, j);
                }
            }
        }
        finishSegment(*seg, std::move(edges));
        plan.segments.push_back(std::move(seg));
    }
    plan.dirty = false;
}

// ---- execution ----------------------------------------------------------------

void reportMissing(SystemEntry& sys, std::string_view missing, Stage stage) {
    sys.runnable = missing.empty();
    if (sys.runnable) {
        sys.missingReported = false;
    } else if (!sys.missingReported) {
        sys.missingReported = true; // once, not every frame
        GTABR_ERROR("{}: system '{}' skipped: resource '{}' is not in the world", kStageNames[static_cast<size_t>(stage)],
                    sys.name, missing);
    }
}

void runNode(void* p) {
    auto* arg = static_cast<Segment::NodeArg*>(p);
    Segment& seg = *arg->segment;
    App::Impl& impl = *seg.impl;
    SystemEntry& sys = (*seg.list)[seg.systems[arg->local]];
    if (sys.runnable) {
        // Own stage per thread: the backend keeps iterator scratch memory there.
        const RunContext ctx{impl.world, WorldAccess::stage(*impl.world, JobSystem::currentThreadSlot()), impl.jobs};
        sys.vtable.run(sys.state, ctx);
    }
    // Releasing successors BEFORE this job's own completion is counted keeps the
    // segment counter above zero until the whole graph has run.
    for (u32 next : seg.dependents[arg->local]) {
        if (seg.remaining[next].fetch_sub(1, std::memory_order_acq_rel) == 1) {
            impl.jobs->submit(&runNode, &seg.nodeArgs[next], seg.counter);
        }
    }
}

void runExclusive(App::Impl& impl, Segment& seg, Stage stage) {
    SystemEntry& sys = (*seg.list)[seg.systems.front()];
    reportMissing(sys, sys.vtable.prepare(sys.state, *impl.world), stage);
    if (!sys.runnable) return;
    // Outside the parallel phase: the system may change the world freely. A null
    // stage makes queries iterate on the world itself.
    const RunContext ctx{impl.world, nullptr, impl.jobs};
    sys.vtable.run(sys.state, ctx);
    sys.vtable.finish(sys.state, *impl.world);
}

void runParallel(App::Impl& impl, Segment& seg, Stage stage) {
    std::vector<SystemEntry>& list = *seg.list;
    const size_t n = seg.systems.size();
    for (u32 i : seg.systems) reportMissing(list[i], list[i].vtable.prepare(list[i].state, *impl.world), stage);

    WorldAccess::beginParallel(*impl.world);
    if (n == 1) {
        // Nothing to overlap with: skip the job round trip.
        SystemEntry& sys = list[seg.systems[0]];
        if (sys.runnable) {
            const RunContext ctx{impl.world, WorldAccess::stage(*impl.world, 0), impl.jobs};
            sys.vtable.run(sys.state, ctx);
        }
    } else {
        for (size_t i = 0; i < n; ++i) seg.remaining[i].store(seg.depCount[i], std::memory_order_relaxed);
        for (u32 root : seg.roots) impl.jobs->submit(&runNode, &seg.nodeArgs[root], seg.counter);
        impl.jobs->wait(seg.counter);
    }
    WorldAccess::endParallel(*impl.world);

    // Side effects (commands, events) in registration order, whatever order the
    // jobs actually finished in.
    for (u32 i : seg.systems) {
        if (list[i].runnable) list[i].vtable.finish(list[i].state, *impl.world);
    }
}

void runStage(App::Impl& impl, Stage stage) {
    const size_t index = static_cast<size_t>(stage);
    if (impl.plans[index].dirty) buildPlan(impl, index);
    for (auto& seg : impl.plans[index].segments) {
        if (seg->exclusive) runExclusive(impl, *seg, stage);
        else runParallel(impl, *seg, stage);
    }
}

} // namespace

// ---- App ------------------------------------------------------------------------

App::App(AppConfig config) {
    if (config.jobs) {
        m_jobs = config.jobs;
    } else {
        const u32 workers =
            config.workerThreads == AppConfig::kAutoWorkers ? JobSystem::defaultWorkerCount() : config.workerThreads;
        m_ownedJobs = std::make_unique<JobSystem>(workers);
        m_jobs = m_ownedJobs.get();
    }
    WorldAccess::setStageCount(m_world, m_jobs->threadCount());
    m_world.insertResource(m_time);

    m_impl = std::make_unique<Impl>();
    m_impl->world = &m_world;
    m_impl->jobs = m_jobs;
}

App::~App() = default;

JobSystem& App::jobs() { return *m_jobs; }

SystemRef App::registerSystem(Stage stage, void* state, const SystemVTable& vtable, AccessSet&& access,
                              std::uintptr_t key) {
    GTABR_VERIFY(static_cast<size_t>(stage) < kStageCount, "invalid stage");
    GTABR_VERIFY(!m_world.inParallelPhase(), "systems cannot be added while a frame runs");
    SystemEntry entry;
    entry.id = m_impl->nextId++;
    entry.owner = m_activeOwner;
    entry.name = "system#" + std::to_string(entry.id);
    entry.key = key;
    entry.state = state;
    entry.vtable = vtable;
    entry.access = std::move(access);
    const u64 id = entry.id;
    const size_t index = static_cast<size_t>(stage);
    m_impl->systems[index].push_back(std::move(entry));
    m_impl->plans[index].dirty = true;
    return SystemRef(*this, id);
}

void App::removeSystemsOwnedBy(OwnerId owner) {
    GTABR_VERIFY(!m_world.inParallelPhase(), "systems cannot be removed while a frame runs");
    for (size_t s = 0; s < kStageCount; ++s) {
        auto& list = m_impl->systems[s];
        // Destroy first: this runs code that lives in the module about to unload.
        for (SystemEntry& e : list)
            if (e.owner == owner) e.vtable.destroy(e.state);
        const size_t before = list.size();
        std::erase_if(list, [owner](const SystemEntry& e) { return e.owner == owner; });
        if (list.size() != before) m_impl->plans[s].dirty = true;
    }
}

void App::setSystemName(u64 id, const char* name) {
    for (auto& list : m_impl->systems)
        for (SystemEntry& e : list)
            if (e.id == id) e.name = name;
}

void App::addOrdering(u64 id, std::uintptr_t otherKey, bool otherRunsFirst) {
    for (size_t s = 0; s < kStageCount; ++s) {
        for (SystemEntry& e : m_impl->systems[s]) {
            if (e.id != id) continue;
            (otherRunsFirst ? e.after : e.before).push_back(otherKey);
            m_impl->plans[s].dirty = true;
            return;
        }
    }
}

size_t App::systemCount() const {
    size_t n = 0;
    for (const auto& list : m_impl->systems) n += list.size();
    return n;
}

void App::update(f32 deltaSeconds) {
    GTABR_VERIFY(!m_world.inParallelPhase(), "App::update is not re-entrant");
    m_time.delta = deltaSeconds;
    m_time.elapsed += deltaSeconds;
    ++m_time.frame;
    m_world.insertResource(m_time);

    // Startup systems run once, then go away.
    auto& startup = m_impl->systems[static_cast<size_t>(Stage::Startup)];
    if (!startup.empty()) {
        runStage(*m_impl, Stage::Startup);
        for (SystemEntry& e : startup) e.vtable.destroy(e.state);
        startup.clear();
        m_impl->plans[static_cast<size_t>(Stage::Startup)].dirty = true;
    }
    runStage(*m_impl, Stage::PreUpdate);
    runStage(*m_impl, Stage::Update);
    runStage(*m_impl, Stage::PostUpdate);
    m_world.advanceEvents();
}

SystemRef& SystemRef::name(const char* name) {
    m_app->setSystemName(m_id, name);
    return *this;
}

SystemRef& SystemRef::order(std::uintptr_t otherKey, bool otherRunsFirst) {
    m_app->addOrdering(m_id, otherKey, otherRunsFirst);
    return *this;
}

} // namespace gtabr
