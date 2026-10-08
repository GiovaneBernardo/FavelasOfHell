#include <doctest/doctest.h>

#include <atomic>

#include "foh/ecs.h"

using namespace foh;

namespace sched {
struct Item { i32 value; };
struct Note { i32 value; };
template <int N>
struct Probe { i32 value = 0; };
struct Physics {}; // system sets are plain labels
struct Rendering {};
} // namespace sched
FOH_REFLECT(sched::Item, value);
template <int N>
struct foh::Reflect<sched::Probe<N>> {
    static void describe(SchemaBuilder<sched::Probe<N>>& b) { b.field("value", &sched::Probe<N>::value); }
};

using namespace sched;

namespace {

std::atomic<int> g_clock{0};
std::atomic<int> g_t1{-1}, g_t2{-1}, g_t3{-1}, g_t4{-1};

void PhysA(Res<Probe<1>>) { g_t1 = g_clock++; }
void PhysB(Res<Probe<2>>) { g_t2 = g_clock++; }
void Gameplay(Res<Probe<3>>) { g_t3 = g_clock++; }
void Draw(Res<Probe<4>>) { g_t4 = g_clock++; }

void resetClock() {
    g_clock = 0;
    g_t1 = g_t2 = g_t3 = g_t4 = -1;
}

App makeApp(u32 workers) {
    return App({.workerThreads = workers});
}

} // namespace

TEST_CASE("System sets order against every member, whatever the registration order") {
    for (u32 workers : {0u, 3u}) {
        resetClock();
        App app = makeApp(workers);
        app.insertResource(Probe<1>{}).insertResource(Probe<2>{}).insertResource(Probe<3>{}).insertResource(Probe<4>{});

        // Draw is registered FIRST but must run after all of Physics; Gameplay must run before it.
        app.addSystem(Stage::Update, &Draw).afterSet<Physics>().inSet<Rendering>();
        app.addSystem(Stage::Update, &PhysA).inSet<Physics>();
        app.addSystem(Stage::Update, &PhysB).inSet<Physics>();
        app.addSystem(Stage::Update, &Gameplay).beforeSet<Rendering>();
        app.update(0.016f);

        CHECK(g_clock == 4);
        CHECK(g_t4 > g_t1);
        CHECK(g_t4 > g_t2);
        CHECK(g_t4 > g_t3);
    }
}

namespace {

std::atomic<int> g_seenAfterSpawn{-1};
std::atomic<int> g_seenWithoutOrder{-1};

void SpawnItems(Commands cmd) {
    cmd.spawn(Item{1});
    cmd.spawn(Item{2});
}
void CountItemsOrdered(Query<const Item&> q) {
    int n = 0;
    q.each([&](const Item&) { ++n; });
    g_seenAfterSpawn = n;
}
void CountItemsUnordered(Query<const Item&> q) {
    int n = 0;
    q.each([&](const Item&) { ++n; });
    g_seenWithoutOrder = n;
}

} // namespace

TEST_CASE("A system ordered after one that uses Commands sees its effects in the same frame") {
    for (u32 workers : {0u, 3u}) {
        g_seenAfterSpawn = g_seenWithoutOrder = -1;
        App app = makeApp(workers);
        app.registerComponent<Item>();
        app.addSystem(Stage::Update, &SpawnItems);
        app.addSystem(Stage::Update, &CountItemsOrdered).after(&SpawnItems); // explicit order: sync point
        app.addSystem(Stage::Update, &CountItemsUnordered);                  // no order: runs alongside
        app.update(0.016f);
        CHECK(g_seenAfterSpawn == 2);
        CHECK(g_seenWithoutOrder == 0); // unrelated systems still see the previous state
        app.update(0.016f);
        CHECK(g_seenWithoutOrder == 2);
    }
}

namespace {

std::atomic<int> g_notesSeen{0};

void SendNotes(EventWriter<Note> out) {
    out.send({1});
    out.send({2});
}
void ReadNotes(EventReader<Note> in) {
    for (const Note&n : in.read()) g_notesSeen += n.value;
}

} // namespace

TEST_CASE("Events are published before systems ordered after the writer") {
    g_notesSeen = 0;
    App app = makeApp(3);
    app.addSystem(Stage::Update, &SendNotes);
    app.addSystem(Stage::Update, &ReadNotes).after(&SendNotes);
    app.update(0.016f);
    CHECK(g_notesSeen == 3); // same frame, not one frame late
}

namespace {

void ChainA(Commands cmd, Res<Probe<1>>) { cmd.spawn(Item{10}); }
void ChainB(Commands cmd, Query<const Item&> q, Res<Probe<2>>) {
    q.each([&](const Item& i) { cmd.spawn(Item{i.value + 1}); });
}
std::atomic<int> g_chainTotal{0};
void ChainC(Query<const Item&> q, Res<Probe<3>>) {
    int total = 0;
    q.each([&](const Item& i) { total += i.value; });
    g_chainTotal = total;
}

} // namespace

TEST_CASE("Sync points chain: each ordered stage sees everything before it") {
    g_chainTotal = 0;
    App app = makeApp(3);
    app.registerComponent<Item>();
    app.insertResource(Probe<1>{}).insertResource(Probe<2>{}).insertResource(Probe<3>{});
    app.addSystem(Stage::Update, &ChainC).after(&ChainB);
    app.addSystem(Stage::Update, &ChainB).after(&ChainA);
    app.addSystem(Stage::Update, &ChainA);
    app.update(0.016f);
    // A spawned 10; B saw it and spawned 11; C saw both.
    CHECK(g_chainTotal == 21);
}
