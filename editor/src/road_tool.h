#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "foh/asset_db.h"
#include "foh/ecs.h"
#include "foh/renderer/renderer.h"
#include "foh/scene/asset_loader.h"
#include "foh/world/roads.h"
#include "foh/world/terrain.h"
#include "foh/world/vegetation.h"

namespace foh {

// "Roads" window + viewport tool (ADR 0012): hand-placed spline roads on the terrain. Click on the ground to add points to
// the active road, drag a point to move it, Ctrl+click a point to delete it. "Carve terrain" conforms the ground to the
// road (one undo step in the terrain's history); the road's surface and sidewalk meshes drape over whatever the ground is.
// Roads are saved to world/roads.fohroads and keep vegetation off their path.
//
// Thread-safety: main thread (UI) only.
class RoadTool {
public:
    void init(Renderer& renderer, World& world, Terrain& terrain, Vegetation* vegetation, const std::filesystem::path& file);
    void shutdown(); // saves, then removes the road entities and meshes
    bool created() const { return m_terrain != nullptr; }

    void draw(bool* open);
    // Per frame: sets up the road materials (ambientCG asphalt and paving; plain colours when the files are missing).
    void updateMaterial(AssetDb& db, AssetLoader& loader);
    // Inside a viewport window showing the road's world, while the tool is on. Returns true when it used the left mouse.
    bool handleViewport(const Mat4& cameraWorld, f32 fovY, ImVec2 origin, ImVec2 size, bool hovered, bool cameraCaptured, bool gizmoBusy, float dt);
    bool editing() const { return m_editing; }
    // Adds a finished road (QA and scripts); `carve` conforms the terrain to it first.
    void addRoad(Road road, bool carve);

private:
    struct View {
        u32 serial = 0; // names the meshes
        Entity surface{}, sidewalk{};
        bool surfaceUp = false, sidewalkUp = false;
    };
    struct Texture {
        std::string path;
        AssetId id;
        bool asked = false;
    };

    void rebuild(size_t index);
    void rebuildAll();
    void removeEntities(View& v);
    void commit(); // saves, replants around the roads
    void applyMaterials();
    AssetId surfaceAsset(const View& v) const;
    AssetId sidewalkAsset(const View& v) const;

    Renderer* m_renderer = nullptr;
    World* m_world = nullptr;
    Terrain* m_terrain = nullptr;
    Vegetation* m_vegetation = nullptr;
    std::filesystem::path m_file;
    RoadNetwork m_network;
    std::vector<View> m_views;
    u32 m_nextSerial = 1;
    int m_active = -1;

    bool m_editing = false;
    int m_hoverPoint = -1, m_hoverRoad = -1;
    int m_dragPoint = -1, m_dragRoad = -1;
    u32 m_framesSinceDragBuild = 0;
    bool m_cursorValid = false;
    Vec3 m_cursor{0.0f};

    Texture m_asphaltColor{"workshop/ambientCG/Road008A_2K-JPG/Road008A_2K-JPG_Color.jpg", {}, false};
    Texture m_asphaltNormal{"workshop/ambientCG/Road008A_2K-JPG/Road008A_2K-JPG_NormalGL.jpg", {}, false};
    Texture m_paveColor{"workshop/ambientCG/Concrete047A_2K-JPG/Concrete047A_2K-JPG_Color.jpg", {}, false};
    Texture m_paveNormal{"workshop/ambientCG/Concrete047A_2K-JPG/Concrete047A_2K-JPG_NormalGL.jpg", {}, false};
    bool m_materialDirty = true, m_texturesDone = false;
};

} // namespace foh
