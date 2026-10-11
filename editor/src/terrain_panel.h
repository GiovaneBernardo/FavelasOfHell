#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <imgui.h>

#include "foh/asset_db.h"
#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/renderer/renderer.h"
#include "foh/scene/asset_loader.h"
#include "foh/world/terrain.h"
#include "foh/world/terrain_renderer.h"
#include "foh/world/vegetation.h"

namespace foh {

// "Terrain" panel + viewport tool (ADR 0012): edits the procedural placeholder terrain (generator parameters) and
// sculpts on top of it with brushes in the viewport, until a real heightmap replaces the base. The terrain is infinite
// and streams around the camera as LOD nodes in the main world; the sculpt layer and parameters are saved to
// world/terrain.fohterrain.
//
// Thread-safety: main thread (UI) only.
class TerrainPanel {
public:
    // Creates the terrain in `world` (loading or generating it). Cheap to call repeatedly: only the first call works.
    void ensureCreated(Renderer& renderer, JobSystem& jobs, World& world, const std::filesystem::path& file);
    bool created() const { return m_terrain != nullptr; }

    void draw(bool* open);
    // Per frame: sets up the ground material (grass, soil, rock, sand: ambientCG texture sets under assets/workshop,
    // imported on demand and streamed to the renderer; a layer whose files are missing shows a plain colour).
    void updateMaterial(AssetDb& db, AssetLoader& loader);
    // Per frame: streams terrain nodes around `focus` (the camera).
    void update(Vec3 focus);
    // Called inside a viewport window that shows the terrain's scene. Draws the brush ring and applies strokes.
    // Returns true when the left mouse belongs to the tool (the editor then skips click-to-select).
    bool handleViewport(const Mat4& cameraWorld, f32 fovY, ImVec2 origin, ImVec2 size, bool hovered, bool cameraCaptured,
                        bool gizmoBusy, float dt);
    bool sculpting() const { return m_sculptEnabled; }
    void save();
    // Saves and releases everything (node meshes and entities); call while the renderer and the world are alive.
    void shutdown();

    // The district's paving / sand / lawn mask (City::uploadSurfaceMask): the ground material reads it.
    void setSurfaceMask(const AssetId& id, const Vec4& rect);

    Terrain* terrain() { return m_terrain.get(); }
    TerrainRenderer* streamer() { return m_tiles.get(); }
    Vegetation* vegetation() { return m_vegetation.get(); }

private:
    Renderer* m_renderer = nullptr;
    JobSystem* m_jobs = nullptr;
    std::filesystem::path m_file;
    std::unique_ptr<Terrain> m_terrain;
    std::unique_ptr<TerrainRenderer> m_tiles;
    std::unique_ptr<Vegetation> m_vegetation;

    TerrainGenParams m_draft;
    bool m_liveRegenerate = false;

    bool m_sculptEnabled = true;
    BrushStamp m_brush;
    bool m_sampleFlattenOnClick = true;
    bool m_stroking = false;
    bool m_cursorValid = false;
    Vec3 m_cursor{0.0f};
    Vec4 m_color{1.0f, 1.0f, 1.0f, 1.0f};
    struct Layer {
        const char* name;
        std::string albedoPath, normalPath; // relative to the asset root
        AssetId albedo, normal;
        bool askedAlbedo = false, askedNormal = false;
    };
    Layer m_layers[4] = {
        {"Grass", "workshop/ambientCG/Grass004_2K-JPG/Grass004_2K-JPG_Color.jpg", "workshop/ambientCG/Grass004_2K-JPG/Grass004_2K-JPG_NormalGL.jpg", {}, {}},
        {"Soil", "workshop/ambientCG/Ground048_2K-JPG/Ground048_2K-JPG_Color.jpg", "workshop/ambientCG/Ground048_2K-JPG/Ground048_2K-JPG_NormalGL.jpg", {}, {}},
        {"Rock", "workshop/ambientCG/Rock051_2K-JPG/Rock051_2K-JPG_Color.jpg", "workshop/ambientCG/Rock051_2K-JPG/Rock051_2K-JPG_NormalGL.jpg", {}, {}},
        {"Sand", "workshop/ambientCG/Ground_054_2K-JPG/Ground054_2K-JPG_Color.jpg", "workshop/ambientCG/Ground_054_2K-JPG/Ground054_2K-JPG_NormalGL.jpg", {}, {}},
    };
    // Tree bark: a PBR set for the trunks (ambientCG Bark006).
    std::string m_barkColorPath = "workshop/ambientCG/Bark006_2K-JPG/Bark006_2K-JPG_Color.jpg";
    std::string m_barkNormalPath = "workshop/ambientCG/Bark006_2K-JPG/Bark006_2K-JPG_NormalGL.jpg";
    AssetId m_barkColor, m_barkNormal;
    bool m_askedBarkColor = false, m_askedBarkNormal = false, m_barkApplied = false;
    AssetId m_maskId;
    Vec4 m_maskRect{0.0f};
    float m_tileSize = 6.0f;
    bool m_materialDirty = true;
    bool m_texturesDone = false;
    void applyMaterial();
    u64 m_savedRevision = 0;
    u32 m_strokeCount = 0;
};

} // namespace foh
