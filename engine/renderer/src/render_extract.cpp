#include "foh/renderer/render_scene.h"

namespace foh {

namespace {

// Copies render data out of the world into the renderer's snapshot. Runs in
// PostUpdate so it sees the frame's final transforms. ResMut on the link makes
// the scheduler serialize it against anything else touching the link.
void extractRenderScene(ResMut<RenderSceneLink> link, Res<Time> time,
                        Query<Entity, const GlobalTransform&, const MeshRenderer&> meshes,
                        Query<Entity, const GlobalTransform&, const Camera&> cameras,
                        Query<const GlobalTransform&, const DirectionalLight&> suns) {
    RenderScene* scene = link->scene;
    if (!scene) return; // world not attached to a renderer scene (yet)

    ExtractedScene& out = scene->write();
    out.frame = time->frame;
    out.cameras.clear();
    out.sun = {};

    // Meshes: each job thread appends to its own staging vector, then one
    // sequential concatenation. Order across threads is not deterministic, which
    // is fine: the renderer sorts instances by mesh and nothing depends on order.
    for (u32 i = 0; i < kMaxJobThreads; ++i) scene->staging(i).clear();
    meshes.parEach([scene](Entity e, const GlobalTransform& t, const MeshRenderer& m) {
        u32 mesh = m.mesh;
        if (m.asset.isValid()) {
            // Not uploaded (yet): skip it instead of drawing the wrong thing.
            mesh = scene->meshLookup ? scene->meshLookup->find(m.asset, m.submesh) : MeshLookup::kInvalid;
            if (mesh == MeshLookup::kInvalid) return;
        }
        const u32 material = m.material.isValid() && scene->materialLookup ? scene->materialLookup->find(m.material) : 0u;
        scene->staging(JobSystem::currentThreadSlot()).push_back({t.matrix(), m.color, mesh, material, e.id});
    });
    size_t total = 0;
    for (u32 i = 0; i < kMaxJobThreads; ++i) total += scene->staging(i).size();
    out.meshes.clear();
    out.meshes.reserve(total);
    for (u32 i = 0; i < kMaxJobThreads; ++i) {
        const auto& s = scene->staging(i);
        out.meshes.insert(out.meshes.end(), s.begin(), s.end());
    }

    cameras.each([&out](Entity e, const GlobalTransform& t, const Camera& c) {
        // Cameras look through a rigid transform: scale (inherited from a parent) must not reach the view matrix.
        const Vec3 x = glm::normalize(t.axisX()), y = glm::normalize(t.axisY()), z = glm::normalize(t.axisZ());
        const Mat4 world(Vec4(x, 0.0f), Vec4(y, 0.0f), Vec4(z, 0.0f), Vec4(t.position(), 1.0f));
        out.cameras.push_back({e.id, world, c.fovY, c.nearZ, c.farZ, c.active});
    });

    suns.each([&out](const GlobalTransform& t, const DirectionalLight& l) {
        if (out.sun.present) return; // first one wins; multiple suns are a content error
        out.sun.present = true;
        out.sun.direction = -glm::normalize(t.axisZ()); // shines along the entity's local -Z
        out.sun.color = l.color * l.intensity;
    });
}

} // namespace

void RegisterRenderExtract(App& app) {
    app.registerComponent<Name>();
    RegisterTransformPropagation(app); // Transform, GlobalTransform and their systems; must precede the extraction system below
    app.registerComponent<MeshRenderer>();
    app.registerComponent<Camera>();
    app.registerComponent<DirectionalLight>();
    app.insertResource(RenderSceneLink{});
    app.addSystem(Stage::PostUpdate, &extractRenderScene).name("extractRenderScene");
}

} // namespace foh
