#include <doctest/doctest.h>

#include "gtabr/ecs.h"

using namespace gtabr;

TEST_CASE("World entities have generational ids") {
    World world;
    const Entity a = world.spawn();
    CHECK(world.isAlive(a));
    CHECK(world.entityCount() == 1);

    CHECK(world.despawn(a));
    CHECK_FALSE(world.isAlive(a));
    CHECK_FALSE(world.despawn(a));

    const Entity b = world.spawn(); // reuses the slot with a new generation
    CHECK(b.index == a.index);
    CHECK(b.generation != a.generation);
    CHECK_FALSE(world.isAlive(a));
    CHECK(world.isAlive(b));
}

namespace {
int g_engineRuns = 0;
int g_gameRuns = 0;
void EngineSystem(SystemContext&) { ++g_engineRuns; }
void GameSystem(SystemContext&) { ++g_gameRuns; }
} // namespace

TEST_CASE("Systems are removed per owner (hot reload contract)") {
    g_engineRuns = g_gameRuns = 0;
    App app;
    app.addSystem(Stage::Update, &EngineSystem, "engine");
    app.setActiveOwner(kGameOwner);
    app.addSystem(Stage::Update, &GameSystem, "game");
    app.setActiveOwner(kEngineOwner);
    CHECK(app.systemCount() == 2);

    app.update(0.016f);
    CHECK(g_engineRuns == 1);
    CHECK(g_gameRuns == 1);

    app.removeSystemsOwnedBy(kGameOwner);
    CHECK(app.systemCount() == 1);
    app.update(0.016f);
    CHECK(g_engineRuns == 2);
    CHECK(g_gameRuns == 1);
}
