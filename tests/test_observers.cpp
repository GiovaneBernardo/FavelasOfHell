#include <doctest/doctest.h>

#include <vector>

#include "foh/ecs.h"

using namespace foh;

namespace obs {
struct Health { i32 value; };
struct Enemy {};  // tag
struct Counters { i32 added = 0, set = 0, removed = 0, lastValue = -1; };
struct Spawned { i32 from; };
} // namespace obs
FOH_REFLECT(obs::Health, value);
FOH_REFLECT(obs::Counters, added, set, removed, lastValue);
FOH_REFLECT(obs::Spawned, from);

using namespace obs;

namespace {

std::vector<Entity> g_addedFor, g_removedFor;

void OnHealthAdd(Trigger<Health> t, ResMut<Counters> c) {
    ++c->added;
    g_addedFor.push_back(t.entity);
}
void OnHealthSet(Trigger<Health> t, ResMut<Counters> c) {
    ++c->set;
    c->lastValue = t.value ? t.value->value : -2;
}
void OnHealthRemove(Trigger<Health> t, ResMut<Counters> c) {
    ++c->removed;
    c->lastValue = t.value ? t.value->value : -2; // the last value is still readable
    g_removedFor.push_back(t.entity);
}

} // namespace

TEST_CASE("Observers fire for add, set and remove with the entity and value") {
    g_addedFor.clear();
    g_removedFor.clear();
    App app({.workerThreads = 0});
    app.registerComponent<Health>();
    app.insertResource(Counters{});
    app.onAdd(&OnHealthAdd).onSet(&OnHealthSet).onRemove(&OnHealthRemove);
    World& world = app.world();
    const Counters* c = world.resource<Counters>();

    const Entity e = world.spawn(Health{7});
    CHECK(c->added == 1);
    CHECK(c->set == 1);
    CHECK(c->lastValue == 7);
    CHECK(g_addedFor.size() == 1);
    CHECK(g_addedFor[0] == e);

    world.insert(e, Health{9}); // overwrite: set again, not added again
    CHECK(c->added == 1);
    CHECK(c->set == 2);
    CHECK(c->lastValue == 9);

    world.remove<Health>(e);
    CHECK(c->removed == 1);
    CHECK(c->lastValue == 9);
    CHECK(g_removedFor[0] == e);

    const Entity d = world.spawn(Health{3});
    world.despawn(d); // despawning removes the component too
    CHECK(c->removed == 2);
    CHECK(c->lastValue == 3);
}

namespace {

void OnEnemyAdd(Trigger<Enemy> t, Commands cmd) {
    cmd.spawn(Spawned{static_cast<i32>(t.entity.index())});
}

void SpawnEnemies(Commands cmd, Res<Time> time) {
    if (time->frame == 1) {
        cmd.spawn(Health{1}).insert<Enemy>();
        cmd.spawn(Health{2}).insert<Enemy>();
    }
}

} // namespace

TEST_CASE("Observers can use Commands, and fire for changes made by systems") {
    App app({.workerThreads = 2});
    app.registerComponent<Health>().registerComponent<Enemy>().registerComponent<Spawned>();
    app.onAdd(&OnEnemyAdd);
    app.addSystem(Stage::Update, &SpawnEnemies);
    app.update(0.016f);

    int spawned = 0;
    OwnedQuery<const Spawned&> q(app.world());
    q.each([&](const Spawned&) { ++spawned; });
    CHECK(spawned == 2); // one per enemy
    CHECK(app.world().entityCount() == 4);
}

namespace {
int g_calls = 0;
void CountCalls(Trigger<Health>) { ++g_calls; }
} // namespace

TEST_CASE("Observers are removed with their owner and can be registered again") {
    g_calls = 0;
    App app({.workerThreads = 0});
    app.registerComponent<Health>();
    app.setActiveOwner(kGameOwner);
    app.onAdd(&CountCalls);
    app.setActiveOwner(kEngineOwner);

    app.world().spawn(Health{1});
    CHECK(g_calls == 1);

    app.removeSystemsOwnedBy(kGameOwner);
    app.world().spawn(Health{2});
    CHECK(g_calls == 1); // gone

    app.setActiveOwner(kGameOwner);
    app.onAdd(&CountCalls); // "reloaded module"
    app.setActiveOwner(kEngineOwner);
    app.world().spawn(Health{3});
    CHECK(g_calls == 2); // exactly once, not duplicated
}

namespace {
void OnEnemyAddsEnemy(Trigger<Enemy> t, Commands cmd) {
    cmd.spawn().insert<Enemy>(); // changes what it observes
    (void)t;
}
} // namespace

TEST_CASE("An observer that triggers itself is stopped by the per-frame call limit") {
    App app({.workerThreads = 0, .maxObserverCalls = 500});
    app.registerComponent<Enemy>();
    app.onAdd(&OnEnemyAddsEnemy);
    const Entity seed = app.world().spawn();
    app.world().insert<Enemy>(seed); // would never return without the limit
    CHECK(app.world().entityCount() > 100);
    CHECK(app.world().entityCount() <= 502);
}
