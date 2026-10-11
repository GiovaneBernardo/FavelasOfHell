#pragma once

#include <filesystem>
#include <memory>

#include <imgui.h>

#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/renderer/renderer.h"
#include "foh/world/city.h"
#include "foh/world/terrain.h"
#include "foh/world/vegetation.h"

namespace foh {

class TerrainPanel;

// "City" window (ADR 0014): the district's streets, buildings and props around the camera, with the generation settings
// (the same seed builds the same city) and what was built. Needs the terrain: it stands on it.
//
// Thread-safety: main thread (UI) only.
class CityPanel {
public:
    // Loads world/district_leblon.fohcity (when present), hands the surface mask to the ground material and the street
    // trees to the vegetation. Cheap to call repeatedly: only the first call works.
    void ensureCreated(Renderer& renderer, JobSystem& jobs, World& world, TerrainPanel& terrain, const std::filesystem::path& worldDir);
    bool created() const { return m_city != nullptr; }
    City* city() { return m_city.get(); }

    void update(const Terrain& terrain, Vec3 focus);
    void draw(bool* open);
    void shutdown();

private:
    std::unique_ptr<City> m_city;
    Vegetation* m_vegetation = nullptr;
};

} // namespace foh
