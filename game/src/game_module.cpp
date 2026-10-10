#include <cmath>
#include <cstdint>

#include "foh/ecs.h"
#include "foh/game_module.h"
#include "foh/log.h"
#include "foh/renderer/components.h"
#include "foh/scene/prefab_request.h"
#include "player.h"

// Placeholder game content that exercises the engine end to end: a small test
// "block" scene with a playable character (player.h: WASD, Space, C, V) and a
// few spinning objects. Edit a number below while the editor/runtime is running,
// save, and the change appears within seconds (game module hot reload).

namespace foh::game {

// ---- components (registered by the plugin; the data survives reloads) ----

struct Spin {
    Vec3 axis{0.0f, 1.0f, 0.0f};
    f32 radiansPerSecond = 1.0f;
};

struct OrbitCamera {
    Vec3 target{0.0f, 1.0f, 0.0f};
    f32 radius = 22.0f;
    f32 height = 9.0f;
    f32 radiansPerSecond = 0.25f;
};

// Marks that the test scene was spawned, so a reload does not spawn it twice.
struct TestSceneState {
    u32 version = 1;
};

} // namespace foh::game

FOH_REFLECT(foh::game::Spin, axis, radiansPerSecond);
FOH_REFLECT(foh::game::OrbitCamera, target, radius, height, radiansPerSecond);
FOH_REFLECT(foh::game::TestSceneState, version);

namespace foh::game {

// ---- systems ----

void HelloSystem(Res<Time> time) {
    const auto now = static_cast<std::uint64_t>(time->elapsed);
    const auto before = static_cast<std::uint64_t>(time->elapsed - time->delta);
    if (now != before && now % 10 == 0) {
        FOH_INFO("[game] Ola, Vidigal! t={}s frame={}", now, time->frame);
    }
}

void SpinSystem(Res<Time> time, Query<Transform&, const Spin&> spinners) {
    const f32 dt = time->delta;
    spinners.parEach([dt](Transform& t, const Spin& s) {
        t.rotation = glm::normalize(glm::angleAxis(s.radiansPerSecond * dt, s.axis) * t.rotation);
    });
}

void OrbitCameraSystem(Res<Time> time, Query<Transform&, const OrbitCamera&> cameras) {
    const f32 angle = static_cast<f32>(time->elapsed);
    cameras.each([angle](Transform& t, const OrbitCamera& o) {
        const f32 a = angle * o.radiansPerSecond;
        t.position = o.target + Vec3(std::sin(a) * o.radius, o.height, std::cos(a) * o.radius);
        const Vec3 forward = glm::normalize(o.target - t.position);
        t.rotation = glm::quatLookAt(forward, Vec3(0.0f, 1.0f, 0.0f));
    });
}

// Exclusive startup system: spawns the test scene once.
void SpawnTestScene(World& world) {
    if (world.resource<TestSceneState>()) return;
    world.insertResource(TestSceneState{});

    // Ground.
    world.spawn(Transform{{0, -0.05f, 0}, Quat(1, 0, 0, 0), {80, 1, 80}},
                MeshRenderer{static_cast<u32>(BuiltinMesh::Plane), {0.32f, 0.34f, 0.30f, 1}});

    // A grid of blocks with pseudo-random heights and colors: a stand-in for dense buildings.
    constexpr int kHalf = 8;
    for (int x = -kHalf; x <= kHalf; ++x) {
        for (int z = -kHalf; z <= kHalf; ++z) {
            if (x == 0 && z == 0) continue;
            const u32 h = static_cast<u32>((x * 73856093) ^ (z * 19349663));
            const f32 r = static_cast<f32>(h & 255) / 255.0f;
            const f32 g = static_cast<f32>((h >> 8) & 255) / 255.0f;
            const f32 b = static_cast<f32>((h >> 16) & 255) / 255.0f;
            const f32 height = 0.6f + static_cast<f32>((h >> 24) & 7) * 0.7f;
            world.spawn(Transform{{x * 2.2f, height * 0.5f, z * 2.2f}, Quat(1, 0, 0, 0), {1.6f, height, 1.6f}},
                        MeshRenderer{static_cast<u32>(BuiltinMesh::Cube), {0.35f + 0.5f * r, 0.3f + 0.5f * g, 0.3f + 0.5f * b, 1}},
                        StaticBox{});
        }
    }

    // Centerpiece: a sphere on a spinning cube.
    world.spawn(Transform{{0, 1.0f, 0}, Quat(1, 0, 0, 0), {2, 2, 2}},
                MeshRenderer{static_cast<u32>(BuiltinMesh::Cube), {0.9f, 0.25f, 0.2f, 1}},
                Spin{{0, 1, 0}, 0.8f}, StaticBox{});
    world.spawn(Transform{{0, 2.9f, 0}, Quat(1, 0, 0, 0), {1.6f, 1.6f, 1.6f}},
                MeshRenderer{static_cast<u32>(BuiltinMesh::Sphere), {0.95f, 0.8f, 0.2f, 1}});

    // Sun.
    const Vec3 sunDir = glm::normalize(Vec3(-0.45f, -0.8f, -0.35f));
    world.spawn(Transform{{0, 0, 0}, glm::quatLookAt(sunDir, Vec3(0, 1, 0)), {1, 1, 1}}, DirectionalLight{});

    // The player: outside the block grid, facing the city. The model is a PrefabSlot: change its path in the inspector (or
    // right click a model in the content browser > "Use as player model") and the player swaps to it. Until the model is
    // imported the player still moves, invisibly.
    Transform start;
    start.position = Vec3(0.0f, 0.0f, 21.0f);
    start.rotation = glm::angleAxis(math::kPi, Vec3(0.0f, 1.0f, 0.0f)); // the character faces +Z; turn it toward -Z
    world.spawn(start, Name::Make("Player"), Player{}, LocomotionControl{}, MakePrefabSlot("imported/Y Bot/Y Bot.fbx"));

    // The game camera: follows the player (third or first person, V toggles).
    Camera camera;
    camera.nearZ = 0.05f;
    world.spawn(Transform{}, camera, PlayerCamera{}, Name::Make("Player Camera"));
}

class GamePlugin final : public Plugin {
public:
    void build(App& app) override {
        app.registerComponent<Spin>();
        app.registerComponent<OrbitCamera>();
        app.registerComponent<TestSceneState>();
        app.addSystem(Stage::Startup, &SpawnTestScene).name("SpawnTestScene");
        app.addSystem(Stage::Update, &HelloSystem).name("HelloSystem");
        app.addSystem(Stage::Update, &SpinSystem).name("SpinSystem");
        RegisterPlayer(app);
    }
};

void Build(App& app) {
    GamePlugin plugin;
    app.addPlugin(plugin);
}

void Teardown(App&) {}

} // namespace foh::game

FOH_GAME_MODULE("foh_game", &foh::game::Build, &foh::game::Teardown)
