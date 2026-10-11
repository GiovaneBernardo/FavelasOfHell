#pragma once

// A finer ground over part of the heightmap (ADR 0014): the real beach, the flat city, canals, platforms under hillside
// buildings and the surface mask (paving / sand / lawn). Built offline by tools/build_district.py. Where the patch has a
// weight it replaces the heightmap's height (the heightmap keeps its procedural detail wherever the weight is 0).
//
// Thread-safety: immutable after load; any thread may sample.

#include <filesystem>
#include <memory>
#include <vector>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

class FOH_API GroundPatch {
public:
    // Reads a "FOHP" v1 file; null (reason logged) when it is missing or malformed.
    static std::shared_ptr<const GroundPatch> load(const std::filesystem::path& file);

    bool contains(f32 x, f32 z) const { return x >= m_x0 && x < m_x0 + f32(m_w) * m_cell && z >= m_z0 && z < m_z0 + f32(m_h) * m_cell; }
    // Bilinear. Outside the patch: weight 0.
    f32 weight(f32 x, f32 z) const;
    f32 height(f32 x, f32 z) const;

    // The surface mask: RGBA8 (R paving, G sand, B lawn), `surfaceCell()` metres per texel, from (minX, minZ).
    u32 surfaceWidth() const { return m_sw; }
    u32 surfaceHeight() const { return m_sh; }
    f32 surfaceCell() const { return m_scell; }
    f32 minX() const { return m_x0; }
    f32 minZ() const { return m_z0; }
    const std::vector<u8>& surface() const { return m_surface; }
    // Nearest texel of the mask: 0..1 for each channel (paving, sand, lawn). Zero outside the patch.
    void surfaceAt(f32 x, f32 z, f32 out[3]) const;

private:
    u32 m_w = 0, m_h = 0, m_sw = 0, m_sh = 0;
    f32 m_cell = 2.0f, m_x0 = 0.0f, m_z0 = 0.0f, m_unit = 1.0f / 16.0f, m_scell = 4.0f;
    std::vector<i16> m_heights;
    std::vector<u8> m_weights, m_surface;
};

} // namespace foh
