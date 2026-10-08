#include <doctest/doctest.h>

#include <atomic>
#include <string_view>
#include <vector>

#include "foh/ecs.h"

using namespace foh;

namespace store {

struct Position { f32 x, y; };
struct Heat {
    static constexpr bool kSparse = true;
    i32 degrees;
};
struct Marked {
    static constexpr bool kSparse = true; // sparse tag
};
struct Cold { i32 degrees; }; // same shape as Heat, but archetype storage
struct Stash {
    static constexpr bool kSparse = true;
    BufferHandle<i32> items;
};

} // namespace store
FOH_REFLECT(store::Position, x, y);
FOH_REFLECT(store::Heat, degrees);
FOH_REFLECT(store::Cold, degrees);
FOH_REFLECT(store::Stash, items);

using namespace store;

TEST_CASE("Sparse components never change an entity's archetype") {
    World world;
    world.component<Position>();
    world.component<Heat>();
    world.component<Marked>();
    world.component<Cold>();
    std::vector<Entity> entities;
    for (int i = 0; i < 1000; ++i) entities.push_back(world.spawn(Position{static_cast<f32>(i), 0}));
    const size_t before = world.archetypeCount();

    for (int i = 0; i < 1000; i += 2) world.insert(entities[i], Heat{i});
    for (int i = 0; i < 1000; i += 4) world.insert<Marked>(entities[i]);
    CHECK(world.archetypeCount() == before); // 500 + 250 toggles, zero table moves

    for (int i = 0; i < 1000; i += 2) world.insert(entities[i], Cold{i});
    CHECK(world.archetypeCount() > before); // the same thing with archetype storage fragments

    CHECK(world.has<Heat>(entities[2]));
    CHECK_FALSE(world.has<Heat>(entities[3]));
    CHECK(world.get<Heat>(entities[10])->degrees == 10);
    world.remove<Heat>(entities[10]);
    CHECK_FALSE(world.has<Heat>(entities[10]));
}

TEST_CASE("Queries read sparse components, as data and as filters") {
    World world;
    for (int i = 0; i < 100; ++i) {
        const Entity e = world.spawn(Position{static_cast<f32>(i), 0});
        if (i % 2 == 0) world.insert(e, Heat{i * 10});
        if (i % 5 == 0) world.insert<Marked>(e);
    }

    {
        OwnedQuery<const Position&, Heat&> q(world);
        int n = 0;
        q.each([&](const Position& p, Heat& h) {
            CHECK(h.degrees == static_cast<i32>(p.x) * 10);
            h.degrees += 1; // write through the reference
            ++n;
        });
        CHECK(n == 50);
    }
    {
        OwnedQuery<const Heat&> q(world);
        q.each([&](const Heat& h) { CHECK(h.degrees % 10 == 1); });
    }
    {
        OwnedQuery<const Position&, With<Marked>> q(world);
        int n = 0;
        q.each([&](const Position&) { ++n; });
        CHECK(n == 20);
    }
    {
        OwnedQuery<const Position&, Without<Heat>> q(world);
        int n = 0;
        q.each([&](const Position&) { ++n; });
        CHECK(n == 50);
    }
}

namespace {
void CoolDown(Query<Heat&> q) {
    q.parEach([](Heat& h) { h.degrees -= 1; });
}
} // namespace

TEST_CASE("Sparse components work with parEach") {
    App app({.workerThreads = 3});
    app.registerComponent<Heat>();
    World& world = app.world();
    for (int i = 0; i < 20000; ++i) world.insert(world.spawn(), Heat{100});
    app.addSystem(Stage::Update, &CoolDown);
    app.update(0.016f);
    int n = 0;
    OwnedQuery<const Heat&> q(world);
    q.each([&](const Heat& h) {
        CHECK(h.degrees == 99);
        ++n;
    });
    CHECK(n == 20000);
}

TEST_CASE("A sparse component that owns a buffer frees it") {
    World world;
    BufferPool& pool = world.buffers();
    const Entity a = world.spawn();
    const Entity b = world.spawn();
    world.insert(a, Stash{{pool.create(sizeof(i32), nullptr, 4)}});
    world.insert(b, Stash{{pool.create(sizeof(i32), nullptr, 4)}});
    CHECK(pool.liveCount() == 2);
    world.remove<Stash>(a);
    CHECK(pool.liveCount() == 1);
    world.despawn(b);
    CHECK(pool.liveCount() == 0);
}

TEST_CASE("Snapshots include sparse components") {
    World source;
    for (int i = 0; i < 10; ++i) {
        const Entity e = source.spawn(Position{static_cast<f32>(i), 0});
        if (i % 2 == 0) source.insert(e, Heat{i});
        if (i % 3 == 0) source.insert<Marked>(e);
    }
    const std::vector<std::byte> bytes = saveSnapshot(source);

    World target;
    target.component<Position>();
    target.component<Heat>();
    target.component<Marked>();
    const SnapshotReport report = loadSnapshot(target, bytes);
    REQUIRE(report.ok);

    int heated = 0, marked = 0;
    OwnedQuery<const Position&, const Heat&> q(target);
    q.each([&](const Position& p, const Heat& h) {
        CHECK(h.degrees == static_cast<i32>(p.x));
        ++heated;
    });
    OwnedQuery<const Position&, With<Marked>> m(target);
    m.each([&](const Position&) { ++marked; });
    CHECK(heated == 5);
    CHECK(marked == 4);
}

namespace oldstorage {
struct Flag {
    static constexpr std::string_view kName = "store::Flag";
    i32 value;
};
} // namespace oldstorage
namespace newstorage {
struct Flag {
    static constexpr std::string_view kName = "store::Flag";
    static constexpr bool kSparse = true;
    i32 value;
};
} // namespace newstorage
FOH_REFLECT(oldstorage::Flag, value);
FOH_REFLECT(newstorage::Flag, value);

TEST_CASE("Changing a component to sparse storage migrates it") {
    World world;
    std::vector<Entity> entities;
    for (int i = 0; i < 50; ++i) entities.push_back(world.spawn(oldstorage::Flag{i}));
    world.component<newstorage::Flag>();
    for (int i = 0; i < 50; ++i) {
        REQUIRE(world.get<newstorage::Flag>(entities[i]) != nullptr);
        CHECK(world.get<newstorage::Flag>(entities[i])->value == i);
    }
}

// ---- ChangedTables -----------------------------------------------------------------------

TEST_CASE("ChangedTables visits only tables written since the query last saw them") {
    World world;
    const Entity moving = world.spawn(Position{0, 0});
    const Entity still = world.spawn(Position{0, 0}, Cold{1}); // a different table
    OwnedQuery<const Position&, ChangedTables> changed(world);

    auto visit = [&] {
        std::vector<f32> seen;
        changed.each([&](const Position& p) { seen.push_back(p.x); });
        return seen.size();
    };
    CHECK(visit() == 2); // first look: everything is new
    CHECK(visit() == 0); // nothing happened since

    world.insert(moving, Position{5, 0}); // set on one entity: its table is dirty
    CHECK(visit() == 1);
    CHECK(visit() == 0);

    // An entity moves into the other table by gaining a component.
    world.insert(moving, Cold{2});
    CHECK(visit() >= 1);
    CHECK(visit() == 0);
    (void)still;

    // A writer iterating the table counts as a write, even if it changes nothing.
    {
        OwnedQuery<Position&> writer(world);
        writer.each([](Position&) {});
    }
    CHECK(visit() >= 1);
    CHECK(visit() == 0);
}

namespace {

std::atomic<int> g_framesSeen{0};
std::atomic<int> g_entitiesSeen{0};

void Watch(Query<const Position&, ChangedTables> q) {
    int n = 0;
    q.each([&](const Position&) { ++n; });
    if (n > 0) ++g_framesSeen;
    g_entitiesSeen += n;
}
void Wiggle(Query<Position&> q, Res<Time> time) {
    if (time->frame % 2 == 0) q.each([](Position& p) { p.x += 1; });
}

} // namespace

TEST_CASE("ChangedTables in a system skips frames in which nothing wrote the data") {
    g_framesSeen = g_entitiesSeen = 0;
    App app({.workerThreads = 2});
    app.registerComponent<Position>();
    for (int i = 0; i < 10; ++i) app.world().spawn(Position{0, 0});
    app.addSystem(Stage::Update, &Watch).after(&Wiggle);
    app.addSystem(Stage::Update, &Wiggle);
    for (int frame = 0; frame < 6; ++frame) app.update(0.016f);
    // Frame 1: first look. Frames 2, 4, 6: Wiggle iterated (wrote). Others: untouched.
    CHECK(g_framesSeen == 4);
    CHECK(g_entitiesSeen == 40);
}
