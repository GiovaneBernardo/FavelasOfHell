#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "foh/ecs.h"

using namespace foh;

namespace rel {
struct Label { i32 id; };
struct Crate { BufferHandle<i32> contents; };
struct Likes {};  // custom relationship (tag)
struct Owns {};   // custom relationship with cascade delete
} // namespace rel
FOH_REFLECT(rel::Label, id);
FOH_REFLECT(rel::Crate, contents);

using namespace rel;

TEST_CASE("Parent/child: set, replace, clear, iterate") {
    World world;
    const Entity a = world.spawn(Label{1});
    const Entity b = world.spawn(Label{2});
    const Entity c = world.spawn(Label{3});

    CHECK(world.parentOf(c) == kNullEntity);
    world.setParent(c, a);
    CHECK(world.parentOf(c) == a);
    CHECK(world.childCount(a) == 1);

    world.setParent(c, b); // replaces the previous parent
    CHECK(world.parentOf(c) == b);
    CHECK(world.childCount(a) == 0);
    CHECK(world.childCount(b) == 1);

    const Entity d = world.spawn(Label{4});
    world.setParent(d, b);
    std::vector<Entity> kids;
    world.forEachChild(b, [&](Entity child) { kids.push_back(child); });
    CHECK(kids.size() == 2);
    CHECK(std::find(kids.begin(), kids.end(), c) != kids.end());
    CHECK(std::find(kids.begin(), kids.end(), d) != kids.end());

    world.clearParent(d);
    CHECK(world.parentOf(d) == kNullEntity);
    CHECK(world.childCount(b) == 1);
}

TEST_CASE("Despawning a parent despawns its descendants and frees what they own") {
    World world;
    BufferPool& pool = world.buffers();
    const Entity root = world.spawn(Label{0});
    const Entity child = world.spawn(Crate{{pool.create(sizeof(i32), nullptr, 4)}});
    const Entity grandchild = world.spawn(Crate{{pool.create(sizeof(i32), nullptr, 8)}});
    world.setParent(child, root);
    world.setParent(grandchild, child);
    CHECK(world.entityCount() == 3);
    CHECK(pool.liveCount() == 2);

    world.despawn(root);
    CHECK_FALSE(world.isAlive(child));
    CHECK_FALSE(world.isAlive(grandchild));
    CHECK(world.entityCount() == 0);
    CHECK(pool.liveCount() == 0);
}

TEST_CASE("Queries can read the parent and filter on having one") {
    World world;
    const Entity p1 = world.spawn(Label{100});
    const Entity p2 = world.spawn(Label{200});
    const Entity c1 = world.spawn(Label{1});
    const Entity c2 = world.spawn(Label{2});
    const Entity c3 = world.spawn(Label{3});
    world.setParent(c1, p1);
    world.setParent(c2, p1);
    world.setParent(c3, p2);

    {
        OwnedQuery<Entity, const Label&, Parent> q(world);
        int n = 0;
        q.each([&](Entity e, const Label& l, Entity parent) {
            ++n;
            CHECK(parent == world.parentOf(e));
            CHECK(parent == (l.id == 3 ? p2 : p1));
        });
        CHECK(n == 3);
    }
    {
        OwnedQuery<const Label&, Without<Parent>> roots(world);
        int n = 0;
        roots.each([&](const Label& l) {
            CHECK(l.id >= 100);
            ++n;
        });
        CHECK(n == 2);
    }
    {
        OwnedQuery<const Label&, With<Parent>> kids(world);
        int n = 0;
        kids.each([&](const Label&) { ++n; });
        CHECK(n == 3);
    }
}

TEST_CASE("Custom relationships: pairs, several targets, optional cascade") {
    World world;
    const Entity alice = world.spawn(Label{1});
    const Entity bob = world.spawn(Label{2});
    const Entity carol = world.spawn(Label{3});

    world.addPair<Likes>(alice, bob);
    world.addPair<Likes>(alice, carol);
    CHECK(world.hasPair<Likes>(alice, bob));
    CHECK_FALSE(world.hasPair<Likes>(bob, alice));
    CHECK(world.pairTarget<Likes>(alice, 0) != world.pairTarget<Likes>(alice, 1));

    world.removePair<Likes>(alice, bob);
    CHECK_FALSE(world.hasPair<Likes>(alice, bob));
    CHECK(world.hasPair<Likes>(alice, carol));

    // Default: despawning the target only drops the pair.
    world.despawn(carol);
    CHECK(world.isAlive(alice));
    CHECK_FALSE(world.hasPair<Likes>(alice, carol));

    // With cascadeDelete, the sources go too.
    world.cascadeDelete<Owns>();
    const Entity owner = world.spawn(Label{10});
    const Entity owned = world.spawn(Label{11});
    world.addPair<Owns>(owned, owner);
    world.despawn(owner);
    CHECK_FALSE(world.isAlive(owned));
}

// ---- commands: reserved ids and relationships ---------------------------------------

namespace {

std::atomic<int> g_hadIds{0};
std::atomic<int> g_spawned{0};

void SpawnFamily(Commands cmd, Res<Time> time) {
    if (time->frame != 1) return;
    // The parent's id is real immediately, so children can point at it directly.
    auto parent = cmd.spawn(Label{1});
    const Entity parentId = parent.id();
    if (parent.hasId()) ++g_hadIds;
    for (int i = 0; i < 3; ++i) cmd.spawn(Label{10 + i}).setParent(parentId);
}

void SpawnMany(Commands cmd, Res<Time> time) {
    if (time->frame > 2) return;
    // Far more than the initial reservation: the first frame overflows, which must
    // still work; the queue then sizes its reservation from that.
    auto root = cmd.spawn(Label{-1});
    g_hadIds += root.hasId() ? 1 : 0;
    for (int i = 0; i < 100; ++i) {
        auto child = cmd.spawn(Label{i});
        g_hadIds += child.hasId() ? 1 : 0;
        ++g_spawned;
        child.setParent(root); // works with a real id and with an overflow spawn alike
    }
}

} // namespace

TEST_CASE("Commands::spawn returns a usable id and can build hierarchies in one frame") {
    g_hadIds = 0;
    App app({.workerThreads = 2});
    app.registerComponent<Label>();
    app.addSystem(Stage::Update, &SpawnFamily);
    app.update(0.016f);

    CHECK(g_hadIds == 1);
    World& world = app.world();
    CHECK(world.entityCount() == 4); // reserved-but-unused ids are not entities
    OwnedQuery<Entity, const Label&, Parent> q(world);
    int children = 0;
    Entity parentSeen{};
    q.each([&](Entity, const Label& l, Entity parent) {
        CHECK(l.id >= 10);
        parentSeen = parent;
        ++children;
    });
    CHECK(children == 3);
    CHECK(world.get<Label>(parentSeen)->id == 1);
}

TEST_CASE("Spawning beyond the reservation still works, and the reservation adapts") {
    g_hadIds = 0;
    g_spawned = 0;
    App app({.workerThreads = 2});
    app.registerComponent<Label>();
    app.addSystem(Stage::Update, &SpawnMany);

    app.update(0.016f); // frame 1: 101 spawns, only 16 ids were reserved
    const int firstFrameIds = g_hadIds;
    CHECK(firstFrameIds >= 16);
    CHECK(firstFrameIds < 101);

    World& world = app.world();
    CHECK(world.entityCount() == 101);
    {
        OwnedQuery<Entity, const Label&, Parent> q(world);
        int children = 0;
        q.each([&](Entity, const Label&, Entity) { ++children; });
        CHECK(children == 100); // overflow spawns got their parent too
    }

    app.update(0.016f); // frame 2: reservation grew to cover the demand
    CHECK(g_hadIds - firstFrameIds == 101);
    CHECK(world.entityCount() == 202);
}
