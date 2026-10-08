#include <doctest/doctest.h>

#include <atomic>
#include <ostream>
#include <string_view>

#include "foh/ecs.h"
#include "foh/math.h"

using namespace foh;

// ---- schema generation ---------------------------------------------------------------

namespace refl {

struct Inner { f32 a; i16 b; };

enum class Mode : u8 { Idle, Run };

struct Everything {
    bool flag;
    i8 small;
    u16 medium;
    i32 normal;
    u64 big;
    f32 single;
    f64 dbl;
    Mode mode;
    Vec3 position;
    Quat rotation;
    Entity target;
    char name[16];
    f32 weights[3];
    Inner inner;
    Inner pair[2];
};

} // namespace refl

FOH_REFLECT(refl::Inner, a, b);
FOH_REFLECT(refl::Everything, flag, small, medium, normal, big, single, dbl, mode, position, rotation, target, name,
            weights, inner, pair);

TEST_CASE("FOH_REFLECT produces a data-only schema with kinds, offsets and nesting") {
    World world;
    const ComponentId id = world.component<refl::Everything>();
    const Schema* s = world.schema(id);
    REQUIRE(s != nullptr);
    CHECK(s->reflected);
    CHECK(s->size == sizeof(refl::Everything));
    REQUIRE(s->fields.size() == 15);

    auto field = [&](std::string_view name) -> const SchemaField& {
        for (const SchemaField& f : s->fields)
            if (f.name == name) return f;
        FAIL("missing field " << name);
        return s->fields[0];
    };
    CHECK(field("flag").kind == FieldKind::Bool);
    CHECK(field("small").kind == FieldKind::I8);
    CHECK(field("medium").kind == FieldKind::U16);
    CHECK(field("big").kind == FieldKind::U64);
    CHECK(field("single").kind == FieldKind::F32);
    CHECK(field("dbl").kind == FieldKind::F64);
    CHECK(field("mode").kind == FieldKind::U8); // enum -> underlying type
    CHECK(field("position").kind == FieldKind::Vec3);
    CHECK(field("rotation").kind == FieldKind::Quat);
    CHECK(field("target").kind == FieldKind::Entity);
    CHECK(field("name").kind == FieldKind::Text);
    CHECK(field("name").count == 16);
    CHECK(field("weights").kind == FieldKind::F32);
    CHECK(field("weights").count == 3);
    CHECK(field("inner").kind == FieldKind::Struct);
    CHECK(field("inner").children.size() == 2);
    CHECK(field("pair").count == 2);
    CHECK(field("big").offset == offsetof(refl::Everything, big));
    CHECK(field("inner").offset == offsetof(refl::Everything, inner));
}

TEST_CASE("An unreflected component still works but is marked as such") {
    struct Opaque { int a; };
    World world;
    const Entity e = world.spawn(Opaque{5});
    CHECK(world.get<Opaque>(e)->a == 5);
    CHECK_FALSE(world.schema<Opaque>()->reflected);
}

// ---- migration -------------------------------------------------------------------------

namespace v1 {
struct Unit {
    static constexpr std::string_view kName = "test::Unit";
    i32 hp;
    f32 speed;
    i16 level;
    u8 tag;
};
} // namespace v1

namespace v2 {
struct Unit {
    static constexpr std::string_view kName = "test::Unit";
    f64 speed;  // widened from f32
    i32 health; // renamed from hp
    i32 armor;  // new
    Vec3 aim;   // new
    u8 tag;     // kept; level was removed
};
} // namespace v2

template <>
struct foh::Reflect<v1::Unit> {
    static void describe(SchemaBuilder<v1::Unit>& b) {
        b.field("hp", &v1::Unit::hp).field("speed", &v1::Unit::speed).field("level", &v1::Unit::level).field("tag", &v1::Unit::tag);
    }
};
template <>
struct foh::Reflect<v2::Unit> {
    static void describe(SchemaBuilder<v2::Unit>& b) {
        b.field("speed", &v2::Unit::speed)
            .field("health", &v2::Unit::health, "hp") // the former name carries the data over
            .field("armor", &v2::Unit::armor)
            .field("aim", &v2::Unit::aim)
            .field("tag", &v2::Unit::tag);
    }
};

TEST_CASE("Component layout changes migrate every instance by field name") {
    World world;
    std::vector<Entity> units;
    for (int i = 0; i < 100; ++i) {
        units.push_back(world.spawn(v1::Unit{100 + i, 1.5f + static_cast<f32>(i), static_cast<i16>(i), static_cast<u8>(i)}));
    }
    const Entity untouched = world.spawn(); // a different archetype

    world.component<v2::Unit>(); // same name, new layout: migrates in place

    for (int i = 0; i < 100; ++i) {
        const v2::Unit* u = world.get<v2::Unit>(units[i]);
        REQUIRE(u != nullptr);
        CHECK(u->health == 100 + i);                       // renamed field kept its value
        CHECK(u->speed == doctest::Approx(1.5 + i));       // f32 -> f64
        CHECK(u->tag == static_cast<u8>(i));               // unchanged field
        CHECK(u->armor == 0);                              // new fields start zeroed
        CHECK(u->aim == Vec3(0.0f));
    }
    CHECK(world.isAlive(untouched));
    CHECK(world.entityCount() == 101);

    // The world now holds exactly one definition of the component.
    OwnedQuery<const v2::Unit&> q(world);
    int n = 0;
    q.each([&](const v2::Unit&) { ++n; });
    CHECK(n == 100);
}

namespace v1 {
struct Settings {
    static constexpr std::string_view kName = "test::Settings";
    i32 volume;
};
} // namespace v1
namespace v2 {
struct Settings {
    static constexpr std::string_view kName = "test::Settings";
    i32 volume;
    f32 gamma;
};
} // namespace v2
FOH_REFLECT(v1::Settings, volume);
FOH_REFLECT(v2::Settings, volume, gamma);

TEST_CASE("Resources migrate too") {
    World world;
    world.insertResource(v1::Settings{7});
    world.component<v2::Settings>();
    REQUIRE(world.resource<v2::Settings>() != nullptr);
    CHECK(world.resource<v2::Settings>()->volume == 7);
    CHECK(world.resource<v2::Settings>()->gamma == 0.0f);
}

// ---- buffers ----------------------------------------------------------------------------------

namespace buf {
struct Item { i32 id; i32 qty; };
struct Bag {
    u32 gold;
    BufferHandle<Item> items;
};
} // namespace buf
FOH_REFLECT(buf::Item, id, qty);
FOH_REFLECT(buf::Bag, gold, items);

TEST_CASE("Buffer fields are described and owned by their component") {
    World world;
    const Schema* s = world.schema<buf::Bag>();
    REQUIRE(s != nullptr);
    CHECK(s->fields[1].kind == FieldKind::Buffer);
    CHECK(s->fields[1].children.size() == 2);
    CHECK(s->fields[1].childSize == sizeof(buf::Item));

    BufferPool& pool = world.buffers();
    CHECK(pool.liveCount() == 0);

    const buf::Item initial[] = {{1, 10}, {2, 20}, {3, 30}};
    const Entity a = world.spawn(buf::Bag{5, {pool.create(sizeof(buf::Item), initial, 3)}});
    const Entity b = world.spawn(buf::Bag{6, {pool.create(sizeof(buf::Item), nullptr, 0)}});
    CHECK(pool.liveCount() == 2);

    const buf::Bag* bag = world.get<buf::Bag>(a);
    REQUIRE(pool.size(bag->items.raw) == 3);
    CHECK(static_cast<buf::Item*>(pool.data(bag->items.raw, sizeof(buf::Item)))[2].qty == 30);

    world.despawn(a); // the entity's buffer goes with it
    CHECK(pool.liveCount() == 1);
    world.remove<buf::Bag>(b); // so does removing the component
    CHECK(pool.liveCount() == 0);
}

TEST_CASE("Stale buffer handles are rejected, slots are reused") {
    BufferPool pool;
    const u64 h = pool.create(4, nullptr, 2);
    CHECK(pool.isValid(h));
    pool.release(h);
    CHECK_FALSE(pool.isValid(h));
    CHECK(pool.data(h, 4) == nullptr);
    CHECK(pool.size(h) == 0);
    const u64 h2 = pool.create(4, nullptr, 1);
    CHECK(h2 != h);          // same slot, new generation
    CHECK_FALSE(pool.isValid(h));
    CHECK(pool.isValid(h2));
}

TEST_CASE("BufferPool push, erase and resize") {
    BufferPool pool;
    const u64 h = pool.create(sizeof(int), nullptr, 0);
    for (int i = 0; i < 100; ++i) pool.push(h, sizeof(int), &i);
    CHECK(pool.size(h) == 100);
    pool.erase(h, sizeof(int), 0); // keeps order
    auto* v = static_cast<int*>(pool.data(h, sizeof(int)));
    CHECK(v[0] == 1);
    CHECK(v[98] == 99);
    pool.resize(h, sizeof(int), 120);
    v = static_cast<int*>(pool.data(h, sizeof(int)));
    CHECK(v[99] == 0); // new elements are zeroed
    pool.clear(h);
    CHECK(pool.size(h) == 0);
}

namespace {

void Loot(Query<buf::Bag&> bags, BuffersMut<buf::Item> items) {
    // Every row owns a different buffer, so threads may push at the same time.
    bags.parEach([&](buf::Bag& bag) {
        for (int i = 0; i < 5; ++i) items.push(bag.items, buf::Item{i, i * 2});
        bag.gold += 1;
    });
}

std::atomic<int> g_total{0};
void Sum(Query<const buf::Bag&> bags, Buffers<buf::Item> items) {
    int total = 0;
    bags.each([&](const buf::Bag& bag) {
        for (const buf::Item& item : items.get(bag.items)) total += item.qty;
    });
    g_total = total;
}

} // namespace

TEST_CASE("Systems use buffers in parallel; readers and writers are scheduled apart") {
    App app({.workerThreads = 3});
    app.registerComponent<buf::Bag>();
    World& world = app.world();
    const int kBags = 2000;
    for (int i = 0; i < kBags; ++i) {
        world.spawn(buf::Bag{0, {world.buffers().create(sizeof(buf::Item), nullptr, 0)}});
    }
    app.addSystem(Stage::Update, &Loot);
    app.addSystem(Stage::Update, &Sum); // reads what Loot writes: must run after it
    app.update(0.016f);
    CHECK(g_total == kBags * (0 + 2 + 4 + 6 + 8));

    int checked = 0;
    OwnedQuery<const buf::Bag&> q(world);
    q.each([&](const buf::Bag& bag) {
        CHECK(world.buffers().size(bag.items.raw) == 5);
        CHECK(bag.gold == 1);
        ++checked;
    });
    CHECK(checked == kBags);
}

namespace v1 {
struct ItemV1 { i32 id; i32 qty; };
struct Bag {
    static constexpr std::string_view kName = "test::Bag";
    BufferHandle<ItemV1> items;
};
} // namespace v1
namespace v2 {
struct ItemV2 { i32 id; i64 qty; f32 weight; };
struct Bag {
    static constexpr std::string_view kName = "test::Bag";
    BufferHandle<ItemV2> items;
};
} // namespace v2
FOH_REFLECT(v1::ItemV1, id, qty);
FOH_REFLECT(v2::ItemV2, id, qty, weight);
FOH_REFLECT(v1::Bag, items);
FOH_REFLECT(v2::Bag, items);

TEST_CASE("Migration also re-lays out the elements of buffers") {
    World world;
    BufferPool& pool = world.buffers();
    const v1::ItemV1 items[] = {{1, 11}, {2, 22}, {3, 33}};
    const Entity e = world.spawn(v1::Bag{{pool.create(sizeof(v1::ItemV1), items, 3)}});
    const u64 handle = world.get<v1::Bag>(e)->items.raw;

    world.component<v2::Bag>(); // element layout changed

    const v2::Bag* bag = world.get<v2::Bag>(e);
    REQUIRE(bag != nullptr);
    CHECK(bag->items.raw == handle); // same buffer, just re-laid out
    REQUIRE(pool.size(handle) == 3);
    const auto* data = static_cast<const v2::ItemV2*>(pool.data(handle, sizeof(v2::ItemV2)));
    CHECK(data[1].id == 2);
    CHECK(data[1].qty == 22);
    CHECK(data[2].weight == 0.0f);
    CHECK(pool.liveCount() == 1); // migration neither leaked nor double-freed
    world.despawn(e);
    CHECK(pool.liveCount() == 0);
}
