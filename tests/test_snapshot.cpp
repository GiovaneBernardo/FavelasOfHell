#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <string_view>
#include <vector>

#include "foh/ecs.h"
#include "foh/log.h"

using namespace foh;

namespace snap {

struct Stats { i32 hp; f32 speed; };
struct Link {
    Entity target;
    BufferHandle<Entity> friends;
};
struct Settings {
    u32 level;
    char name[8];
};
struct Knows {}; // relationship

} // namespace snap
FOH_REFLECT(snap::Stats, hp, speed);
FOH_REFLECT(snap::Link, target, friends);
FOH_REFLECT(snap::Settings, level, name);

namespace {

void registerAll(World& world) {
    world.component<snap::Stats>();
    world.component<snap::Link>();
    world.component<snap::Settings>();
    world.component<snap::Knows>();
}

struct Built {
    Entity a, b, c;
};

Built buildWorld(World& world) {
    registerAll(world);
    Built w;
    w.a = world.spawn(snap::Stats{10, 1.5f});
    w.b = world.spawn(snap::Stats{20, 2.5f});
    w.c = world.spawn(snap::Stats{30, 3.5f});
    const Entity friends[] = {w.b, w.c};
    world.insert(w.a, snap::Link{w.b, {world.buffers().create(sizeof(Entity), friends, 2)}});
    world.setParent(w.c, w.a);
    world.addPair<snap::Knows>(w.b, w.c);
    snap::Settings settings{7, {}};
    std::strcpy(settings.name, "vidigal");
    world.insertResource(settings);
    return w;
}

} // namespace

TEST_CASE("Snapshot round trip restores components, buffers, parents, relationships and resources") {
    World source;
    buildWorld(source);
    const std::vector<std::byte> bytes = saveSnapshot(source);
    CHECK(bytes.size() > 100);

    World target;
    registerAll(target);
    target.spawn(snap::Stats{999, 0}); // existing content is replaced
    const SnapshotReport report = loadSnapshot(target, bytes);
    REQUIRE(report.ok);
    CHECK(report.entities == 3);
    CHECK(report.resources == 1);
    CHECK(report.skippedComponents == 0);
    CHECK(target.entityCount() == 3);

    // Find the entities again by their data: ids are not preserved.
    Entity a, b, c;
    {
        OwnedQuery<Entity, const snap::Stats&> q(target);
        q.each([&](Entity e, const snap::Stats& s) {
            if (s.hp == 10) a = e;
            if (s.hp == 20) b = e;
            if (s.hp == 30) c = e;
            CHECK(s.hp != 999);
        });
    }
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(target.get<snap::Stats>(c)->speed == 3.5f);

    // Entity fields and buffers of entities were remapped to the NEW ids.
    const snap::Link* link = target.get<snap::Link>(a);
    REQUIRE(link != nullptr);
    CHECK(link->target == b);
    REQUIRE(target.buffers().size(link->friends.raw) == 2);
    const auto* friends = static_cast<const Entity*>(target.buffers().data(link->friends.raw, sizeof(Entity)));
    CHECK(friends[0] == b);
    CHECK(friends[1] == c);

    CHECK(target.parentOf(c) == a);
    CHECK(target.hasPair<snap::Knows>(b, c));

    REQUIRE(target.resource<snap::Settings>() != nullptr);
    CHECK(target.resource<snap::Settings>()->level == 7);
    CHECK(std::string_view(target.resource<snap::Settings>()->name) == "vidigal");

    // The old buffer of the replaced entity was freed, only the loaded one lives.
    CHECK(target.buffers().liveCount() == 1);
}

TEST_CASE("Snapshot is deterministic for the same world") {
    World world;
    buildWorld(world);
    CHECK(saveSnapshot(world) == saveSnapshot(world));
}

namespace oldv {
struct Unit {
    static constexpr std::string_view kName = "snap::Unit";
    i32 hp;
    f32 speed;
    i16 level;
};
} // namespace oldv
namespace newv {
struct Unit {
    static constexpr std::string_view kName = "snap::Unit";
    f64 speed;
    i32 health;
    i32 armor;
};
} // namespace newv
FOH_REFLECT(oldv::Unit, hp, speed, level);
template <>
struct foh::Reflect<newv::Unit> {
    static void describe(SchemaBuilder<newv::Unit>& b) {
        b.field("speed", &newv::Unit::speed).field("health", &newv::Unit::health, "hp").field("armor", &newv::Unit::armor);
    }
};

TEST_CASE("A snapshot saved with the old layout loads into the new layout") {
    World source;
    for (int i = 0; i < 20; ++i) source.spawn(oldv::Unit{50 + i, 0.5f * static_cast<f32>(i), static_cast<i16>(i)});
    const std::vector<std::byte> bytes = saveSnapshot(source);

    World target;
    target.component<newv::Unit>(); // this build only knows the new layout
    const SnapshotReport report = loadSnapshot(target, bytes);
    REQUIRE(report.ok);
    CHECK(report.entities == 20);
    CHECK(report.migratedComponents == 20);

    int n = 0;
    OwnedQuery<const newv::Unit&> q(target);
    q.each([&](const newv::Unit& u) {
        CHECK(u.health >= 50);
        CHECK(u.health < 70);
        CHECK(u.speed == doctest::Approx(0.5 * (u.health - 50)));
        CHECK(u.armor == 0);
        ++n;
    });
    CHECK(n == 20);
}

TEST_CASE("Components this build does not know are skipped, the rest still loads") {
    World source;
    buildWorld(source);
    const std::vector<std::byte> bytes = saveSnapshot(source);

    World target;
    target.component<snap::Stats>(); // Link, Settings, Knows are "from a module that is not loaded"
    const SnapshotReport report = loadSnapshot(target, bytes);
    REQUIRE(report.ok);
    CHECK(report.entities == 3);
    CHECK(report.skippedComponents > 0);
    CHECK(target.entityCount() == 3);
}

TEST_CASE("Malformed snapshots are rejected without touching the world") {
    World source;
    buildWorld(source);
    const std::vector<std::byte> good = saveSnapshot(source);

    World target;
    registerAll(target);
    const Entity keep = target.spawn(snap::Stats{1, 1});
    BufferPool& pool = target.buffers();
    const size_t buffersBefore = pool.liveCount();

    auto expectRejected = [&](std::span<const std::byte> data) {
        const SnapshotReport r = loadSnapshot(target, data);
        CHECK_FALSE(r.ok);
        CHECK(target.isAlive(keep));
        CHECK(target.entityCount() == 1);
        CHECK(pool.liveCount() == buffersBefore);
    };

    expectRejected({});
    expectRejected(std::span<const std::byte>(good).first(3));

    std::vector<std::byte> badMagic = good;
    badMagic[0] = std::byte{'X'};
    expectRejected(badMagic);

    std::vector<std::byte> trailing = good;
    trailing.push_back(std::byte{0});
    expectRejected(trailing);

    // Every truncation is rejected (the format has no optional tail).
    for (size_t len = 0; len < good.size(); len += 7) expectRejected(std::span<const std::byte>(good).first(len));
}

TEST_CASE("Corrupted snapshots never crash the loader") {
    World source;
    buildWorld(source);
    const std::vector<std::byte> good = saveSnapshot(source);

    SetMinLogLevel(LogLevel::Fatal); // hundreds of expected "rejected" errors
    std::srand(12345);
    for (int round = 0; round < 400; ++round) {
        World target;
        registerAll(target);
        std::vector<std::byte> data = good;
        const int flips = 1 + std::rand() % 4;
        for (int i = 0; i < flips; ++i) {
            data[static_cast<size_t>(std::rand()) % data.size()] = static_cast<std::byte>(std::rand() & 0xFF);
        }
        const SnapshotReport r = loadSnapshot(target, data); // ok or not, but must return
        if (!r.ok) CHECK(target.entityCount() == 0);          // nothing half-loaded
    }
    SetMinLogLevel(LogLevel::Trace);
}
