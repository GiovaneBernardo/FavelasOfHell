// Keeps GlobalTransform in sync with Transform and the parent hierarchy.
//
// Cost model (the point of this file): nothing happens for entities that did not change.
//   - Tables of entities nobody wrote to since the last run are skipped (ChangedTables), so a static
//     city costs nothing per frame however large it is.
//   - For a changed entity the new world matrix is computed once, then pushed down its subtree.
//   - Both changed queries are separate (roots / children) so the parent entity comes from the table
//     (one value per table) instead of a lookup per entity, and its GlobalTransform is fetched only
//     when the parent changes between tables.
//
// Correctness does not depend on the order tables are visited in: whenever an entity is (re)computed,
// everything below it is recomputed after it, and an ancestor visited later recomputes the whole
// subtree again. The per-frame stamp only skips work that is provably done (a descendant that was
// already refreshed through an ancestor earlier in this same frame).

#include "foh/renderer/render_scene.h"

namespace foh {

namespace {

// Row-major 3x4 (R|t) helpers on GlobalTransform.
Mat3 basisOf(const GlobalTransform& g) {
    return Mat3(g.axisX(), g.axisY(), g.axisZ());
}

GlobalTransform compose(const GlobalTransform& parent, const Transform& local, u32 stamp) {
    const Mat3 localBasis = glm::mat3_cast(local.rotation) * Mat3(local.scale.x, 0, 0, 0, local.scale.y, 0, 0, 0, local.scale.z);
    const Mat3 pb = basisOf(parent);
    const Mat3 basis = pb * localBasis;
    const Vec3 t = parent.position() + pb * local.position;
    GlobalTransform g;
    g.row0 = Vec4(basis[0][0], basis[1][0], basis[2][0], t.x);
    g.row1 = Vec4(basis[0][1], basis[1][1], basis[2][1], t.y);
    g.row2 = Vec4(basis[0][2], basis[1][2], basis[2][2], t.z);
    g.stamp = stamp;
    return g;
}

GlobalTransform fromLocal(const Transform& local, u32 stamp) { return compose(GlobalTransform{}, local, stamp); }

// Recomputes every descendant of `parent` (whose GlobalTransform is final) from its Transform.
// A child without a Transform is transparent: its children hang off the nearest ancestor that has one.
void propagateDown(World& world, Entity parent, const GlobalTransform& parentGlobal, u32 stamp, u32 depth) {
    if (depth > 512) return; // a cycle cannot exist (the ECS forbids it); this only bounds a corrupted world
    const World& readOnly = world;
    world.forEachChild(parent, [&](Entity child) {
        const Transform* local = readOnly.get<Transform>(child);
        GlobalTransform* global = world.get<GlobalTransform>(child);
        if (local && global) {
            *global = compose(parentGlobal, *local, stamp);
            propagateDown(world, child, *global, stamp, depth + 1);
        } else {
            propagateDown(world, child, parentGlobal, stamp, depth + 1);
        }
    });
}

struct PropagateState {
    u32 frame = 0;
};

// Entities with a Transform but no GlobalTransform yet (just spawned, or loaded from a save that predates it).
void addGlobalTransforms(Commands commands, Query<Entity, With<Transform>, Without<GlobalTransform>> missing) {
    missing.each([&](Entity e) { commands.entity(e).insert(GlobalTransform{}); });
}

// Exclusive: it walks hierarchies through the world. Runs alone, on the main thread.
void propagateTransforms(World& world, Local<PropagateState> state,
                         Query<Entity, const Transform&, With<GlobalTransform>, Without<Parent>, ChangedTables> roots,
                         Query<Entity, const Transform&, Parent, With<GlobalTransform>, ChangedTables> children) {
    const u32 stamp = ++state->frame;

    roots.each([&](Entity e, const Transform& local) {
        GlobalTransform* global = world.get<GlobalTransform>(e);
        if (!global) return;
        *global = fromLocal(local, stamp);
        if (world.childCount(e) != 0) propagateDown(world, e, *global, stamp, 0);
    });

    // The parent is one value per table; remember its GlobalTransform until it changes.
    Entity lastParent = kNullEntity;
    const GlobalTransform* parentGlobal = nullptr;
    children.each([&](Entity e, const Transform& local, Entity parent) {
        GlobalTransform* global = world.get<GlobalTransform>(e);
        if (!global || global->stamp == stamp) return; // already refreshed through its ancestor this frame
        if (parent != lastParent) {
            lastParent = parent;
            parentGlobal = static_cast<const World&>(world).get<GlobalTransform>(parent);
        }
        // A parent without a GlobalTransform (a pure grouping entity) is the identity.
        const GlobalTransform identity;
        *global = compose(parentGlobal ? *parentGlobal : identity, local, stamp);
        if (world.childCount(e) != 0) propagateDown(world, e, *global, stamp, 0);
    });
}

} // namespace

void RegisterTransformPropagation(App& app) {
    app.registerComponent<Transform>();
    app.registerComponent<GlobalTransform>();
    app.addSystem(Stage::PostUpdate, &addGlobalTransforms).name("addGlobalTransforms");
    app.addSystem(Stage::PostUpdate, &propagateTransforms).name("propagateTransforms");
}

} // namespace foh
