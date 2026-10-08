#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <thread>
#include <vector>

#include "gtabr/ecs.h"
#include "gtabr/jobs.h"

using namespace gtabr;

namespace ecs_test {

struct Position { float x = 0, y = 0; };
struct Velocity { float x = 0, y = 0; };
struct Health { int value = 0; };
struct Frozen {}; // tag
struct Score { int value = 0; }; // resource
struct Hit { int amount = 0; };  // event

// Distinct resource types so tests can build systems that do / do not conflict.
template <int N>
struct Marker { int value = 0; };

AppConfig sequential() { return {.workerThreads = 0}; }
AppConfig threaded() { return {.workerThreads = 3}; }

} // namespace ecs_test

using namespace ecs_test;

// ---- world: entities, components, resources ---------------------------------------

TEST_CASE("World entities have generational ids") {
    World world;
    const Entity a = world.spawn();
    CHECK(world.isAlive(a));
    CHECK(world.entityCount() == 1);

    CHECK(world.despawn(a));
    CHECK_FALSE(world.isAlive(a));
    CHECK_FALSE(world.despawn(a));
    CHECK(world.entityCount() == 0);

    const Entity b = world.spawn(); // reuses the slot with a new generation
    CHECK(b.index() == a.index());
    CHECK(b.generation() != a.generation());
    CHECK_FALSE(world.isAlive(a));
    CHECK(world.isAlive(b));
    CHECK_FALSE(world.isAlive(kNullEntity));
}

TEST_CASE("World components: insert, get, overwrite, remove, tags") {
    World world;
    const Entity e = world.spawn(Position{1, 2}, Health{10});
    REQUIRE(world.get<Position>(e) != nullptr);
    CHECK(world.get<Position>(e)->y == 2);
    CHECK(world.has<Health>(e));
    CHECK_FALSE(world.has<Velocity>(e));

    world.insert(e, Position{5, 6}); // overwrite in place
    CHECK(world.get<Position>(e)->x == 5);

    world.get<Health>(e)->value = 99; // direct mutation
    CHECK(world.get<Health>(e)->value == 99);

    world.insert<Frozen>(e);
    CHECK(world.has<Frozen>(e));
    world.remove<Frozen>(e);
    CHECK_FALSE(world.has<Frozen>(e));

    world.remove<Health>(e);
    CHECK(world.get<Health>(e) == nullptr);

    // Operations on dead entities are harmless no-ops.
    world.despawn(e);
    world.insert(e, Health{1});
    CHECK(world.get<Health>(e) == nullptr);
    CHECK_FALSE(world.has<Position>(e));
}

TEST_CASE("World component ids are per world and resolved by name") {
    World a;
    World b;
    b.component<Health>(); // register in a different order than in `a`
    const ComponentId ida = a.component<Position>();
    const ComponentId idb = b.component<Position>();
    CHECK(ida != 0);
    CHECK(idb != 0);
    // Alternating between worlds must keep resolving correctly (cache slot is shared).
    CHECK(a.component<Position>() == ida);
    CHECK(b.component<Position>() == idb);
    CHECK(a.component<Position>() == ida);

    const Entity ea = a.spawn(Position{1, 1});
    const Entity eb = b.spawn(Position{2, 2});
    CHECK(a.get<Position>(ea)->x == 1);
    CHECK(b.get<Position>(eb)->x == 2);
}

TEST_CASE("World resources") {
    World world;
    CHECK(world.resource<Score>() == nullptr);
    world.insertResource(Score{7});
    REQUIRE(world.resource<Score>() != nullptr);
    CHECK(world.resource<Score>()->value == 7);
    world.resource<Score>()->value = 8;
    CHECK(world.resource<Score>()->value == 8);
    CHECK(world.removeResource<Score>());
    CHECK_FALSE(world.removeResource<Score>());
    CHECK(world.resource<Score>() == nullptr);
    CHECK(world.entityCount() == 0); // resources are not entities
}

// ---- queries ---------------------------------------------------------------------

TEST_CASE("Queries match on components and filters") {
    World world;
    const Entity moving = world.spawn(Position{0, 0}, Velocity{1, 0});
    const Entity frozen = world.spawn(Position{0, 0}, Velocity{1, 0});
    world.insert<Frozen>(frozen);
    const Entity still = world.spawn(Position{9, 9});

    {
        OwnedQuery<Position&, const Velocity&, Without<Frozen>> q(world);
        int n = 0;
        q.each([&](Position& p, const Velocity& v) {
            p.x += v.x;
            ++n;
        });
        CHECK(n == 1);
        CHECK(world.get<Position>(moving)->x == 1);
        CHECK(world.get<Position>(frozen)->x == 0);
    }
    {
        OwnedQuery<Entity, const Position&, With<Frozen>> q(world);
        int n = 0;
        q.each([&](Entity e, const Position&) {
            CHECK(e == frozen);
            ++n;
        });
        CHECK(n == 1);
    }
    {
        OwnedQuery<Entity, const Position&> q(world);
        std::vector<Entity> seen;
        q.each([&](Entity e, const Position&) { seen.push_back(e); });
        CHECK(seen.size() == 3);
        CHECK(std::find(seen.begin(), seen.end(), still) != seen.end());
    }
}

// ---- systems and params ------------------------------------------------------------

namespace {

void Movement(Query<Position&, const Velocity&> q, Res<Time> time) {
    q.each([&](Position& p, const Velocity& v) {
        p.x += v.x * time->delta;
        p.y += v.y * time->delta;
    });
}

void Accumulate(ResMut<Score> score, Query<const Health&> q) {
    q.each([&](const Health& h) { score->value += h.value; });
}

} // namespace

TEST_CASE("Systems receive typed parameters") {
    App app(sequential());
    app.insertResource(Score{0});
    app.registerComponent<Position>().registerComponent<Velocity>().registerComponent<Health>();
    World& world = app.world();
    const Entity a = world.spawn(Position{0, 0}, Velocity{2, 4});
    world.spawn(Health{3});
    world.spawn(Health{4});

    app.addSystem(Stage::Update, &Movement);
    app.addSystem(Stage::Update, &Accumulate);
    app.update(0.5f);

    CHECK(world.get<Position>(a)->x == doctest::Approx(1.0f));
    CHECK(world.get<Position>(a)->y == doctest::Approx(2.0f));
    CHECK(world.resource<Score>()->value == 7);
    CHECK(app.time().frame == 1);
    CHECK(world.resource<Time>()->delta == doctest::Approx(0.5f));
}

namespace {

std::atomic<int> g_missingRuns{0};
std::atomic<int> g_okRuns{0};
void NeedsMissing(Res<Marker<99>>) { ++g_missingRuns; }
void DoesNotNeed() { ++g_okRuns; }

} // namespace

TEST_CASE("A system whose resource is missing is skipped, others still run") {
    g_missingRuns = g_okRuns = 0;
    App app(sequential());
    app.addSystem(Stage::Update, &NeedsMissing);
    app.addSystem(Stage::Update, &DoesNotNeed);
    app.update(0.016f);
    app.update(0.016f);
    CHECK(g_missingRuns == 0);
    CHECK(g_okRuns == 2);
    app.world().insertResource(Marker<99>{});
    app.update(0.016f);
    CHECK(g_missingRuns == 1);
}

// ---- commands -----------------------------------------------------------------------

namespace {

void SpawnOnce(Commands cmd, Res<Time> time) {
    if (time->frame == 1) cmd.spawn(Position{1, 1}, Health{5}).insert<Frozen>();
}

std::atomic<int> g_seenBySameSegment{-1};
void CountHealthy(Query<const Health&> q) {
    int n = 0;
    q.each([&](const Health&) { ++n; });
    g_seenBySameSegment = n;
}

} // namespace

TEST_CASE("Commands: spawn with components, applied after the segment") {
    App app(sequential());
    app.registerComponent<Health>().registerComponent<Position>().registerComponent<Frozen>();
    app.addSystem(Stage::Update, &SpawnOnce);
    app.addSystem(Stage::Update, &CountHealthy);

    app.update(0.016f); // frame 1: spawned, but CountHealthy ran in the same segment
    CHECK(g_seenBySameSegment == 0);
    CHECK(app.world().entityCount() == 1);

    app.update(0.016f);
    CHECK(g_seenBySameSegment == 1);

    OwnedQuery<const Health&, With<Frozen>, const Position&> q(app.world());
    int n = 0;
    q.each([&](const Health& h, const Position& p) {
        CHECK(h.value == 5);
        CHECK(p.x == 1);
        ++n;
    });
    CHECK(n == 1);
}

namespace {

void KillWeak(Commands cmd, Query<Entity, const Health&> q) {
    q.each([&](Entity e, const Health& h) {
        if (h.value < 5) cmd.despawn(e);
        else cmd.entity(e).insert(Position{7, 7}).remove<Health>();
    });
}

} // namespace

TEST_CASE("Commands: despawn, insert and remove on existing entities") {
    App app(sequential());
    app.registerComponent<Health>().registerComponent<Position>();
    World& world = app.world();
    const Entity weak = world.spawn(Health{1});
    const Entity strong = world.spawn(Health{9});
    app.addSystem(Stage::Update, &KillWeak);
    app.update(0.016f);
    CHECK_FALSE(world.isAlive(weak));
    REQUIRE(world.isAlive(strong));
    CHECK(world.get<Position>(strong)->x == 7);
    CHECK_FALSE(world.has<Health>(strong));
}

namespace {

template <int N>
void Spawner(Commands cmd, ResMut<Marker<N>>) {
    cmd.spawn(Health{N});
}

template <int... N>
void addSpawners(App& app, std::integer_sequence<int, N...>) {
    (app.insertResource(Marker<N>{}), ...);
    (app.addSystem(Stage::Update, &Spawner<N>), ...);
}

} // namespace

TEST_CASE("Commands from parallel systems apply in registration order, independent of thread timing") {
    App app(threaded());
    app.registerComponent<Health>();
    addSpawners(app, std::make_integer_sequence<int, 8>{});

    const int kFrames = 25;
    for (int f = 0; f < kFrames; ++f) app.update(0.016f);

    std::vector<int> order;
    OwnedQuery<const Health&> q(app.world());
    q.each([&](const Health& h) { order.push_back(h.value); });
    REQUIRE(order.size() == static_cast<size_t>(8 * kFrames));
    for (size_t i = 0; i < order.size(); ++i) CHECK(order[i] == static_cast<int>(i % 8));
}

// ---- scheduling: parallelism and ordering ---------------------------------------------

namespace {

std::atomic<int> g_inFlight{0};
std::atomic<int> g_maxInFlight{0};

void slowWork() {
    const int now = ++g_inFlight;
    int seen = g_maxInFlight.load();
    while (now > seen && !g_maxInFlight.compare_exchange_weak(seen, now)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    --g_inFlight;
}

void SlowA(ResMut<Marker<1>>) { slowWork(); }
void SlowB(ResMut<Marker<2>>) { slowWork(); }
void SlowC(ResMut<Marker<1>>) { slowWork(); } // conflicts with SlowA

} // namespace

TEST_CASE("Systems with disjoint access run at the same time") {
    g_inFlight = g_maxInFlight = 0;
    App app(threaded());
    app.insertResource(Marker<1>{}).insertResource(Marker<2>{});
    app.addSystem(Stage::Update, &SlowA);
    app.addSystem(Stage::Update, &SlowB);
    app.update(0.016f);
    CHECK(g_maxInFlight == 2);
}

TEST_CASE("Systems with conflicting access never overlap") {
    g_inFlight = g_maxInFlight = 0;
    App app(threaded());
    app.insertResource(Marker<1>{});
    app.addSystem(Stage::Update, &SlowA);
    app.addSystem(Stage::Update, &SlowC);
    app.update(0.016f);
    CHECK(g_maxInFlight == 1);
}

TEST_CASE("Read-only access is shared, a writer is exclusive") {
    struct Local {
        static void ReadA(Res<Marker<1>>) { slowWork(); }
        static void ReadB(Res<Marker<1>>) { slowWork(); }
        static void Write(ResMut<Marker<1>>) { slowWork(); }
    };
    g_inFlight = g_maxInFlight = 0;
    {
        App app(threaded());
        app.insertResource(Marker<1>{});
        app.addSystem(Stage::Update, &Local::ReadA);
        app.addSystem(Stage::Update, &Local::ReadB);
        app.update(0.016f);
        CHECK(g_maxInFlight == 2);
    }
    g_inFlight = g_maxInFlight = 0;
    {
        App app(threaded());
        app.insertResource(Marker<1>{});
        app.addSystem(Stage::Update, &Local::ReadA);
        app.addSystem(Stage::Update, &Local::Write);
        app.update(0.016f);
        CHECK(g_maxInFlight == 1);
    }
}

namespace {

std::atomic<int> g_sequence{0};
std::atomic<int> g_first{-1}, g_second{-1}, g_third{-1};

void StepA(Res<Marker<10>>) { g_first = g_sequence++; }
void StepB(Res<Marker<11>>) { g_second = g_sequence++; }
void StepC(Res<Marker<12>>) { g_third = g_sequence++; }

} // namespace

TEST_CASE("Explicit after()/before() order systems that do not conflict") {
    for (AppConfig config : {sequential(), threaded()}) {
        g_sequence = 0;
        g_first = g_second = g_third = -1;
        App app(config);
        app.insertResource(Marker<10>{}).insertResource(Marker<11>{}).insertResource(Marker<12>{});
        // Registered A, B, C but required to run C, B, A.
        app.addSystem(Stage::Update, &StepA).after(&StepB);
        app.addSystem(Stage::Update, &StepB).after(&StepC);
        app.addSystem(Stage::Update, &StepC);
        app.update(0.016f);
        CHECK(g_third < g_second);
        CHECK(g_second < g_first);
    }
}

TEST_CASE("before() is the mirror of after(); cycles are reported and ignored") {
    g_sequence = 0;
    g_first = g_second = g_third = -1;
    App app(threaded());
    app.insertResource(Marker<10>{}).insertResource(Marker<11>{}).insertResource(Marker<12>{});
    app.addSystem(Stage::Update, &StepA).before(&StepB);
    app.addSystem(Stage::Update, &StepB).before(&StepA); // cycle with the line above
    app.addSystem(Stage::Update, &StepC);
    app.update(0.016f);
    CHECK(g_sequence == 3); // everything still ran
}

namespace {

std::atomic<int> g_entitiesSeenByExclusive{-1};
std::atomic<int> g_entitiesSeenAfter{-1};

void SpawnTwo(Commands cmd, ResMut<Marker<20>>) {
    cmd.spawn(Health{1});
    cmd.spawn(Health{2});
}
void Exclusive(World& world) {
    g_entitiesSeenByExclusive = static_cast<int>(world.entityCount());
    world.spawn(Health{3}); // direct structural change is allowed here
}
void After(Query<const Health&> q) {
    int n = 0;
    q.each([&](const Health&) { ++n; });
    g_entitiesSeenAfter = n;
}

} // namespace

TEST_CASE("Exclusive systems are barriers: earlier commands are applied, later systems see the changes") {
    App app(threaded());
    app.registerComponent<Health>();
    app.insertResource(Marker<20>{});
    app.addSystem(Stage::Update, &SpawnTwo);
    app.addSystem(Stage::Update, &Exclusive);
    app.addSystem(Stage::Update, &After);
    app.update(0.016f);
    CHECK(g_entitiesSeenByExclusive == 2);
    CHECK(g_entitiesSeenAfter == 3);
}

namespace {
std::atomic<int> g_startupRuns{0};
void Startup() { ++g_startupRuns; }
} // namespace

TEST_CASE("Startup systems run once") {
    g_startupRuns = 0;
    App app(sequential());
    app.addSystem(Stage::Startup, &Startup);
    CHECK(app.systemCount() == 1);
    app.update(0.016f);
    app.update(0.016f);
    CHECK(g_startupRuns == 1);
    CHECK(app.systemCount() == 0);
}

// ---- events ---------------------------------------------------------------------------

namespace {

std::atomic<int> g_hitsSeen{0};
std::atomic<int> g_hitSum{0};
void Strike(EventWriter<Hit> out, Res<Time> time) {
    if (time->frame == 1) {
        out.send({1});
        out.send({2});
        out.send({3});
    }
}
void Observe(EventReader<Hit> in) {
    for (const Hit& h : in.read()) {
        ++g_hitsSeen;
        g_hitSum += h.amount;
    }
}

} // namespace

TEST_CASE("Events are delivered once to each reader, then expire") {
    g_hitsSeen = g_hitSum = 0;
    App app(threaded());
    app.addSystem(Stage::PreUpdate, &Strike);
    app.addSystem(Stage::Update, &Observe);
    app.update(0.016f); // sent in PreUpdate, read in Update of the same frame
    CHECK(g_hitsSeen == 3);
    app.update(0.016f);
    app.update(0.016f);
    CHECK(g_hitsSeen == 3); // never delivered twice
    CHECK(g_hitSum == 6);

    // A reader that registers late still sees events of the previous frame only.
    app.update(0.016f);
    EventChannel& channel = eventChannelFor<Hit>(app.world());
    u64 cursor = 0;
    u32 count = 0;
    channel.read(cursor, count);
    CHECK(count == 0);
}

// ---- data-parallel iteration ----------------------------------------------------------

namespace {

void BumpAll(Query<Position&> q) {
    q.parEach([](Position& p) { p.x += 1; });
}

} // namespace

TEST_CASE("parEach touches every entity exactly once") {
    App app(threaded());
    app.registerComponent<Position>().registerComponent<Frozen>();
    World& world = app.world();
    const int kCount = 30000;
    for (int i = 0; i < kCount; ++i) {
        const Entity e = world.spawn(Position{0, static_cast<float>(i)});
        if (i % 3 == 0) world.insert<Frozen>(e); // spread over two archetypes
    }
    app.addSystem(Stage::Update, &BumpAll);
    app.update(0.016f);
    app.update(0.016f);

    int checked = 0;
    OwnedQuery<const Position&> q(world);
    q.each([&](const Position& p) {
        CHECK(p.x == 2.0f);
        ++checked;
    });
    CHECK(checked == kCount);
}

TEST_CASE("parEach spreads work over several threads") {
    static std::atomic<u32> mask;
    mask = 0;
    struct Local {
        static void Work(Query<Position&> q) {
            q.parEach([](Position& p) {
                p.x += 1;
                mask.fetch_or(1u << JobSystem::currentThreadSlot());
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            });
        }
    };
    App app(threaded());
    app.registerComponent<Position>();
    for (int i = 0; i < 2000; ++i) app.world().spawn(Position{});
    app.addSystem(Stage::Update, &Local::Work);
    app.update(0.016f);
    CHECK(std::popcount(mask.load()) >= 2);
}

// ---- hot reload contract -----------------------------------------------------------------

namespace {
std::atomic<int> g_engineRuns{0};
std::atomic<int> g_gameRuns{0};
void EngineSystem(Res<Time>) { ++g_engineRuns; }
void GameSystem(Res<Time>) { ++g_gameRuns; }
} // namespace

TEST_CASE("Systems are removed per owner; world data and components survive") {
    g_engineRuns = g_gameRuns = 0;
    App app(threaded());
    app.registerComponent<Health>();
    const Entity survivor = app.world().spawn(Health{42});

    app.addSystem(Stage::Update, &EngineSystem);
    app.setActiveOwner(kGameOwner);
    app.registerComponent<Position>();
    app.addSystem(Stage::Update, &GameSystem);
    app.setActiveOwner(kEngineOwner);
    CHECK(app.systemCount() == 2);

    app.update(0.016f);
    CHECK(g_engineRuns == 1);
    CHECK(g_gameRuns == 1);

    app.removeSystemsOwnedBy(kGameOwner); // the host does this before unloading the DLL
    CHECK(app.systemCount() == 1);
    app.update(0.016f);
    CHECK(g_engineRuns == 2);
    CHECK(g_gameRuns == 1);

    // "Reloaded" module registers again; its data is still there.
    app.setActiveOwner(kGameOwner);
    app.addSystem(Stage::Update, &GameSystem);
    app.setActiveOwner(kEngineOwner);
    app.update(0.016f);
    CHECK(g_gameRuns == 2);
    CHECK(app.world().get<Health>(survivor)->value == 42);
}
