#pragma once

// A real-world height grid (a DEM) as the terrain's base layer. Produced by tools/build_rio_heightmap.py from open data
// (Copernicus GLO-30 for now: 30 m, resampled to 15 m cells), world metres with x east and z south from the origin
// (Vidigal). The terrain adds procedural detail between its samples, so a coarse source still reads right at 0.5 m.
//
// Thread-safety: immutable after load; any thread may sample.

#include <filesystem>
#include <memory>
#include <vector>

#include "foh/platform.h"
#include "foh/world/ground_patch.h"
#include "foh/types.h"

namespace foh {

class FOH_API DemHeightmap {
public:
    // Reads a "FOHD" file; null (with the reason logged) when it is missing or malformed.
    static std::shared_ptr<const DemHeightmap> load(const std::filesystem::path& file);

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    f32 cell() const { return m_cell; }
    f32 minX() const { return m_originX; }
    f32 minZ() const { return m_originZ; }
    f32 maxX() const { return m_originX + f32(m_width - 1) * m_cell; }
    f32 maxZ() const { return m_originZ + f32(m_height - 1) * m_cell; }
    bool contains(f32 x, f32 z) const { return x >= minX() && x <= maxX() && z >= minZ() && z <= maxZ(); }
    // Metres from (x, z) to the nearest edge of the grid (negative outside).
    f32 edgeDistance(f32 x, f32 z) const;

    // The same heightmap with a finer ground patch over part of it (null patch = none). The terrain blends the patch in.
    static std::shared_ptr<const DemHeightmap> withPatch(const std::shared_ptr<const DemHeightmap>& dem, std::shared_ptr<const GroundPatch> patch);
    const GroundPatch* patch() const { return m_patch.get(); }
    std::shared_ptr<const GroundPatch> patchShared() const { return m_patch; }

    // Bicubic (Catmull-Rom) height in metres, clamped at the border: smooth normals from a coarse grid.
    f32 sample(f32 x, f32 z) const;

private:
    u32 m_width = 0, m_height = 0;
    f32 m_cell = 15.0f, m_originX = 0.0f, m_originZ = 0.0f, m_unit = 1.0f / 16.0f;
    std::vector<i16> m_data;
    std::shared_ptr<const GroundPatch> m_patch;
};

} // namespace foh
