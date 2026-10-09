#pragma once

// The Prefab Editor: fixes an imported model once, for every future placement.
//
// A dropped model becomes an instance of the prefab generated at import. Imported models are often wrong in
// ways that are the same every time (a size in centimeters, a Z-up export, a rotation at the root, a wrong
// material). This window edits those: it shows the model in its own preview viewport, lists its nodes, and
// lets you change each node's position / rotation / scale / materials, remove nodes, or fix the whole model
// at once (scale it to a real height, rotate it in 90 degree steps, put it on the ground).
//
// The edits are saved as a prefab VARIANT (".fvariant", ADR 0004) next to the model, never into the
// re-generated prefab, so they survive a reimport of the source. Placing the model afterwards (drag from the
// content browser) uses the variant automatically.
//
// The scene side (spawning the preview) is the editor's job: it implements PrefabEditorHost::show.
//
// Thread-safety: main thread only. A low priority job reads the mesh files for their bounds.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asset_picker.h"
#include "foh/asset_db.h"
#include "foh/jobs.h"

namespace foh {

struct PrefabEditorHost {
    // Shows `prefab` in the preview scene, replacing what was shown before.
    std::function<void(const PrefabAsset& prefab)> show;
    // Aims the preview camera at a sphere.
    std::function<void(Vec3 center, float radius)> frame;
    // Opens / closes the preview viewport together with this window.
    std::function<void(bool visible)> setPreviewVisible;
    // Places a model or variant (path relative to the asset root) into the main scene.
    std::function<void(const std::string& relPath)> place;
    std::function<void()> refreshBrowser;
};

class PrefabEditor {
public:
    PrefabEditor();
    ~PrefabEditor();

    void init(AssetDb& db, AssetPicker& picker, JobSystem& jobs, PrefabEditorHost host);
    // Waits for the bounds job. Call before the job system goes away.
    void shutdown();

    // Opens a model source ("assets/..." relative path) or an authored ".fvariant". Returns false (and says why
    // in lastError()) when it has no prefab.
    bool open(const std::string& relPath);
    const std::string& lastError() const { return m_error; }
    bool isOpen() const { return m_loaded; }
    // Where the variant of a model source lives: the source path with ".fvariant" instead of its extension.
    static std::string VariantPathFor(const std::string& sourcePath);

    // QA (--prefab-qa): does what a user would do with the buttons: fit the height, rotate, sit on the ground, save,
    // place. Returns false when the model is not open or its size is not known yet.
    bool runSelfTest(float height);

    // Every frame, also while closed: collects the bounds job.
    void update();
    void draw(bool* visible);

private:
    struct BoundsTask;
    struct NodeXf {
        Vec3 position{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f};
    };

    PrefabVariantAsset makeVariant() const;
    bool edited() const;
    void edit(); // something changed: preview and dirty flag
    void rebuildPreview();
    void save();
    void resetAll();
    bool computeBounds(Vec3& lo, Vec3& hi) const;
    void startBoundsJob();
    void commitRoots(); // the current root transforms become the base of the whole-model controls
    void applyWholeModel();
    void sitOnGround();
    void frameView();

    void drawHeader();
    void drawWholeModel();
    void drawTree(int node);
    void drawNode();
    bool isGone(int node) const;

    AssetDb* m_db = nullptr;
    AssetPicker* m_picker = nullptr;
    JobSystem* m_jobs = nullptr;
    PrefabEditorHost m_host;

    bool m_loaded = false;
    std::string m_error;
    std::string m_sourcePath;  // the model
    std::string m_variantPath; // where Save writes
    std::string m_title;
    AssetId m_basePrefab;
    PrefabAsset m_base; // as imported
    PrefabAsset m_work; // base + the edits (same node indices)
    std::vector<u8> m_removed;
    std::vector<std::vector<int>> m_kids;
    std::vector<int> m_roots;
    int m_selected = 0;
    bool m_dirty = false;
    bool m_rebuild = false;
    std::string m_pendingOpen; // Revert: reopening happens between frames, not in the middle of drawing

    // Whole-model controls act relative to the root transforms as they were when last committed.
    std::vector<NodeXf> m_rootBase;
    f32 m_adjScale = 1.0f;
    Quat m_adjRot{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 m_adjOffset{0.0f}; // world-space shift added after scale and rotation (sit on ground)
    f32 m_targetHeight = 1.8f;

    // Mesh bounds (read on a job): they give the model's real size.
    std::unordered_map<AssetId, std::pair<Vec3, Vec3>, AssetIdHash> m_meshBounds;
    std::unique_ptr<BoundsTask> m_bounds;
    bool m_framePending = false;

    bool m_uniformScale = true;
    bool m_eulerActive = false;
    Vec3 m_euler{0.0f};
};

} // namespace foh
