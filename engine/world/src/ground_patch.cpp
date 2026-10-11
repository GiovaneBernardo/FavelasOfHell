#include "foh/world/ground_patch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "foh/log.h"

namespace foh {

std::shared_ptr<const GroundPatch> GroundPatch::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return nullptr;
    struct Header {
        char magic[4];
        u32 version, w, h;
        f32 cell, x0, z0, unit, scell;
        u32 sw, sh;
    } h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || std::memcmp(h.magic, "FOHP", 4) != 0 || h.version != 1 || h.w < 2 || h.h < 2 || h.w > 16384 || h.h > 16384 || h.sw > 16384 || h.sh > 16384) {
        FOH_ERROR("ground patch: {} is not a valid FOHP v1 file", file.string());
        return nullptr;
    }
    auto p = std::shared_ptr<GroundPatch>(new GroundPatch());
    p->m_w = h.w;
    p->m_h = h.h;
    p->m_cell = h.cell;
    p->m_x0 = h.x0;
    p->m_z0 = h.z0;
    p->m_unit = h.unit;
    p->m_scell = h.scell;
    p->m_sw = h.sw;
    p->m_sh = h.sh;
    p->m_heights.resize(size_t(h.w) * h.h);
    p->m_weights.resize(size_t(h.w) * h.h);
    p->m_surface.resize(size_t(h.sw) * h.sh * 4);
    f.read(reinterpret_cast<char*>(p->m_heights.data()), std::streamsize(p->m_heights.size() * sizeof(i16)));
    f.read(reinterpret_cast<char*>(p->m_weights.data()), std::streamsize(p->m_weights.size()));
    f.read(reinterpret_cast<char*>(p->m_surface.data()), std::streamsize(p->m_surface.size()));
    if (!f) {
        FOH_ERROR("ground patch: {} is truncated", file.string());
        return nullptr;
    }
    FOH_INFO("ground patch: {} x {} at {} m, surface {} x {}", h.w, h.h, h.cell, h.sw, h.sh);
    return p;
}

f32 GroundPatch::weight(f32 x, f32 z) const {
    const f32 u = (x - m_x0) / m_cell - 0.5f, v = (z - m_z0) / m_cell - 0.5f;
    if (u < -1.0f || v < -1.0f || u > f32(m_w) || v > f32(m_h)) return 0.0f;
    const i32 i = i32(std::floor(u)), j = i32(std::floor(v));
    const f32 fx = u - f32(i), fz = v - f32(j);
    auto at = [&](i32 a, i32 b) { return f32(m_weights[size_t(std::clamp(b, 0, i32(m_h) - 1)) * m_w + size_t(std::clamp(a, 0, i32(m_w) - 1))]) * (1.0f / 255.0f); };
    // Outside the grid by up to one cell the border weight fades to 0, so the patch never ends in a step.
    const f32 edge = std::min(std::min(x - m_x0, m_x0 + f32(m_w) * m_cell - x), std::min(z - m_z0, m_z0 + f32(m_h) * m_cell - z));
    const f32 w = (at(i, j) * (1 - fx) + at(i + 1, j) * fx) * (1 - fz) + (at(i, j + 1) * (1 - fx) + at(i + 1, j + 1) * fx) * fz;
    return w * std::clamp(edge / m_cell, 0.0f, 1.0f);
}

f32 GroundPatch::height(f32 x, f32 z) const {
    const f32 u = (x - m_x0) / m_cell - 0.5f, v = (z - m_z0) / m_cell - 0.5f;
    const i32 i = i32(std::floor(u)), j = i32(std::floor(v));
    const f32 fx = u - f32(i), fz = v - f32(j);
    auto at = [&](i32 a, i32 b) { return f32(m_heights[size_t(std::clamp(b, 0, i32(m_h) - 1)) * m_w + size_t(std::clamp(a, 0, i32(m_w) - 1))]) * m_unit; };
    return (at(i, j) * (1 - fx) + at(i + 1, j) * fx) * (1 - fz) + (at(i, j + 1) * (1 - fx) + at(i + 1, j + 1) * fx) * fz;
}

void GroundPatch::surfaceAt(f32 x, f32 z, f32 out[3]) const {
    const i32 i = i32(std::floor((x - m_x0) / m_scell)), j = i32(std::floor((z - m_z0) / m_scell));
    if (i < 0 || j < 0 || i >= i32(m_sw) || j >= i32(m_sh)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const u8* t = &m_surface[(size_t(j) * m_sw + size_t(i)) * 4];
    out[0] = f32(t[0]) * (1.0f / 255.0f);
    out[1] = f32(t[1]) * (1.0f / 255.0f);
    out[2] = f32(t[2]) * (1.0f / 255.0f);
}

} // namespace foh
