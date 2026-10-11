#pragma once

// The Asset Forge: the bridge between an imported model and a game-ready asset (ADR 0015).
//
// A model from Sketchfab is meshes and materials. The game needs more: where the wheels are and how the suspension
// behaves, which part is solid, which bones are the arms. The Forge lets a human add exactly that and stores it next to the
// model as a small text file the game reads (game_asset.h):
//
//   Vehicle    wheels (found automatically from the node names and sizes, or placed), suspension, tires, engine, gearbox,
//              brakes, steering, seat and chase camera; presets from a hatchback to a pickup; test drive in one click.
//   Character  a skinned model is checked against the humanoid the animation library speaks; an unrigged one is rigged
//              automatically (humanoid_rig.h), the Mixamo way: place the joints, the Forge skins it and imports the result.
//   Prop       collision shape fitted to the model, body type, mass, surface.
//
// Every type starts with the same step: the model fix (orientation, size, origin on the ground), saved as the prefab variant
// next to the model, which is also what the prefab editor writes, so the two tools agree.
//
// The Forge shows the model in the editor's Prefab Preview viewport and draws its handles on top of it (wheels, suspension
// travel, centre of mass, collider, skeleton) with ImGuizmo for moving them.
//
// Thread-safety: main thread only. Mesh loading and rigging run on the job system (low priority).

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "asset_picker.h"
#include "foh/asset_db.h"
#include "foh/game_asset.h"
#include "foh/humanoid_rig.h"
#include "foh/jobs.h"

namespace foh {

struct AssetForgeHost {
    // Shows `prefab` in the preview scene (replacing what was shown), aims the camera, and opens / closes the viewport.
    std::function<void(const PrefabAsset& prefab)> show;
    std::function<void(Vec3 center, float radius)> frame;
    std::function<void(bool visible)> setPreviewVisible;
    // Makes the previewed character walk in place at this speed (m/s) so a rig can be judged.
    std::function<void(float speed)> previewWalk;
    std::function<void()> refreshBrowser;
    // Spawns a game asset file (path relative to the asset root) in front of the Game camera, so it can be driven / walked up to.
    std::function<void(const std::string& relPath)> spawn;
    // The vehicle the B key spawns in the game.
    std::function<void(const std::string& relPath)> selectForSpawnKey;
    // Starts an import of a file the Forge wrote; the editor reports the outcome through AssetForge::importFinished.
    std::function<ImportTicket(const std::filesystem::path& file)> importFile;
    // Makes a model the game's player.
    std::function<void(const std::string& relPath)> useAsPlayer;
    std::function<void(const std::string& message)> log;
};

class AssetForge {
public:
    AssetForge();
    ~AssetForge();

    void init(AssetDb& db, AssetPicker& picker, JobSystem& jobs, AssetForgeHost host);
    // Waits for the background jobs. Call before the job system goes away.
    void shutdown();

    // Opens a model source ("imported/Car/Car.gltf"), a prefab variant, or a game asset (.fvehicle / .fprop / .fchar).
    // Returns false (reason in lastError()) when it cannot be used.
    bool open(const std::string& relPath);
    const std::string& lastError() const { return m_error; }
    bool isOpen() const { return m_loaded; }

    // Every frame, also while closed: collects finished jobs.
    void update();
    void draw(bool* visible);
    // Handles on the preview viewport. Camera as the viewport uses it (world matrix, fov, planes); origin/size in screen
    // pixels. `hovered`: the viewport is under the mouse.
    void drawOverlay(const Mat4& cameraWorld, float fovY, float nearZ, float farZ, ImVec2 origin, ImVec2 size, bool hovered);
    // The editor reports every finished import here (the Forge imports the rigged character it writes).
    void importFinished(const ImportResult& result);

    // QA (--forge-qa): does what a user would do, without the mouse. `what` is "vehicle", "character" or "prop".
    // Returns true when the step is complete (call each frame until it is).
    bool runSelfTest(const std::string& what);

    // Where the files of a model live: the model path with the extension swapped (.fvehicle, .fprop, .fchar, .fvariant).
    static std::string SiblingPath(const std::string& sourcePath, const char* extension);

private:
    friend struct ForgeInternals;
    enum class Tab : int { Vehicle, Character, Prop };

    struct MeshLoad;
    struct RigTask {
        JobCounter counter;
        HumanoidRigInput input;
        HumanoidRigResult result;
        std::vector<u8> glb;
        static void run(void* arg) {
            auto* t = static_cast<RigTask*>(arg);
            if (BuildHumanoidRig(t->input, t->result)) t->glb = WriteRiggedGlb(t->input, t->result);
        }
    };
    struct Node {
        Mat4 world{1.0f};
        bool hasBounds = false;
        Vec3 lo{0.0f}, hi{0.0f}; // world-space AABB of the mesh
        std::string path;
        std::string lowerName;
    };

    // ---- model (forge.cpp) ----
    bool loadModel(const std::string& sourcePath);
    void startMeshLoad();
    void finishMeshLoad();
    void rebuildWork();       // m_work from m_workBase and the fix
    void recomputeNodes();    // world matrices and bounds of m_work
    void rebuildPreview();
    void frameModel();
    void saveVariant();
    bool boundsOfModel(Vec3& lo, Vec3& hi) const;
    bool boundsOfNodes(const std::vector<int>& nodes, Vec3& lo, Vec3& hi) const;
    void drawHeader();
    void drawModelFix();
    bool fixed() const;
    void resetFix();
    void centerAndGround();
    std::string slug() const;
    std::string pathFor(const char* extension) const { return SiblingPath(m_sourcePath, extension); }
    bool writeText(const std::string& relPath, const std::string& text);

    // ---- overlay helpers (forge.cpp) ----
    struct View {
        Mat4 view{1.0f};
        Mat4 proj{1.0f};
        float fovY = 1.0f;
        ImVec2 origin, size;
        bool project(const Vec3& p, ImVec2& out) const;
    };
    void line3(const View& v, const Vec3& a, const Vec3& b, ImU32 color, float width = 1.5f) const;
    void circle3(const View& v, const Vec3& center, const Vec3& axisU, const Vec3& axisV, float radius, ImU32 color, float width = 1.5f) const;
    void box3(const View& v, const Vec3& center, const Vec3& half, ImU32 color, float width = 1.5f) const;
    void label3(const View& v, const Vec3& p, const char* text, ImU32 color) const;
    // Moves a point with a translate gizmo; true while it changed.
    bool moveHandle(const View& v, int id, Vec3& position, bool hovered);

    // ---- vehicle (forge_vehicle.cpp) ----
    void drawVehicleTab();
    void drawVehicleOverlay(const View& v, bool hovered);
    void detectWheels();
    void layoutWheelsFromBounds();
    void estimateVehicleFromBounds();
    void applyPreset(int preset);
    void setDriveLayout(int layout); // 0 FWD, 1 RWD, 2 AWD
    void mirrorWheel(int index);
    bool saveVehicle();

    // ---- character (forge_character.cpp) ----
    void drawCharacterTab();
    void drawCharacterOverlay(const View& v, bool hovered);
    void placeLandmarks();
    void startRig();
    void finishRig();
    void applyRiggedImport(const ImportResult& result);
    bool saveCharacter();
    void checkSkeleton();

    // ---- prop (forge_prop.cpp) ----
    void drawPropTab();
    void drawPropOverlay(const View& v, bool hovered);
    void fitProp();
    bool saveProp();

    AssetDb* m_db = nullptr;
    AssetPicker* m_picker = nullptr;
    JobSystem* m_jobs = nullptr;
    AssetForgeHost m_host;

    // model
    bool m_loaded = false;
    std::string m_error;
    std::string m_sourcePath;  // the model, relative to the asset root
    std::string m_title;
    AssetId m_basePrefab;
    PrefabAsset m_base;         // as imported
    PrefabAsset m_workBase;     // base with the saved variant applied
    PrefabAsset m_work;         // m_workBase with the fix applied: what is shown and what the assets are made from
    PrefabVariantAsset m_existingVariant;
    bool m_hasVariant = false;
    std::vector<Node> m_nodes;  // parallel to m_work.nodes
    std::unordered_map<AssetId, MeshAsset, AssetIdHash> m_meshes;
    std::unique_ptr<MeshLoad> m_load;
    bool m_meshesReady = false;
    bool m_previewDirty = false;
    bool m_framePending = false;
    bool m_variantDirty = false;
    Vec3 m_lo{0.0f}, m_hi{0.0f};
    bool m_haveBounds = false;
    // the fix, applied to the root nodes
    int m_fixQuarterTurns = 0;
    float m_fixScale = 1.0f;
    Vec3 m_fixOffset{0.0f};
    float m_targetLength = 4.4f, m_targetHeight = 1.8f;

    Tab m_tab = Tab::Vehicle;
    bool m_tabRequested = false;

    // vehicle
    VehicleDef m_vehicle;
    bool m_vehicleLoaded = false;   // a .fvehicle exists / was started
    bool m_vehicleDirty = false;
    int m_selectedWheel = 0;
    bool m_mirrorWheels = true;
    enum class VehicleHandle : int { Wheel, CenterOfMass, ColliderCenter, DriverSeat, Exit };
    VehicleHandle m_vehicleHandle = VehicleHandle::Wheel;
    int m_preset = 1;
    std::string m_vehicleNote;

    // prop
    PropDef m_prop;
    bool m_propLoaded = false;
    bool m_propDirty = false;
    int m_propHandle = 0; // 0 centre

    // character
    CharacterDef m_character;
    bool m_characterLoaded = false;
    bool m_characterDirty = false;
    bool m_skinnedModel = false;
    int m_selectedJoint = 0;
    bool m_mirrorJoints = true;
    std::string m_skeletonReport;
    int m_skeletonMatched = 0;
    std::unique_ptr<RigTask> m_rig;
    ImportTicket m_rigTicket = 0;
    std::string m_rigStatus;
    HumanoidRigResult m_rigResult;
    std::string m_riggedSource;      // asset-root relative path of the rigged glb
    struct RigSourceNode {
        std::string name;
        int srcNode = -1;
        std::vector<AssetId> submeshMaterials;
    };
    std::vector<RigSourceNode> m_rigSources;
    float m_walkSpeed = 0.0f;
    bool m_rigPreviewShown = false;

    // QA
    int m_qaStep = 0;
    int m_qaWait = 0;
};

} // namespace foh
