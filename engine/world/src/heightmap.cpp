#include "foh/world/heightmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "foh/log.h"

namespace foh {

std::shared_ptr<const DemHeightmap> DemHeightmap::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return nullptr;
    struct Header {
        char magic[4];
        u32 version, width, height;
        f32 cell, originX, originZ, unit;
    } h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || std::memcmp(h.magic, "FOHD", 4) != 0 || h.version != 1 || h.width < 4 || h.height < 4 || h.width > 32768 || h.height > 32768) {
        FOH_ERROR("heightmap: {} is not a valid FOHD v1 file", file.string());
        return nullptr;
    }
    auto dem = std::shared_ptr<DemHeightmap>(new DemHeightmap());
    dem->m_width = h.width;
    dem->m_height = h.height;
    dem->m_cell = h.cell;
    dem->m_originX = h.originX;
    dem->m_originZ = h.originZ;
    dem->m_unit = h.unit;
    dem->m_data.resize(size_t(h.width) * h.height);
    f.read(reinterpret_cast<char*>(dem->m_data.data()), std::streamsize(dem->m_data.size() * sizeof(i16)));
    if (!f) {
        FOH_ERROR("heightmap: {} is truncated", file.string());
        return nullptr;
    }
    FOH_INFO("heightmap: {} x {} at {} m ({:.1f} x {:.1f} km)", h.width, h.height, h.cell, double(h.width) * h.cell / 1000.0, double(h.height) * h.cell / 1000.0);
    return dem;
}

std::shared_ptr<const DemHeightmap> DemHeightmap::withPatch(const std::shared_ptr<const DemHeightmap>& dem, std::shared_ptr<const GroundPatch> patch) {
    if (!dem) return nullptr;
    auto copy = std::shared_ptr<DemHeightmap>(new DemHeightmap(*dem));
    copy->m_patch = std::move(patch);
    return copy;
}

f32 DemHeightmap::edgeDistance(f32 x, f32 z) const {
    return std::min(std::min(x - minX(), maxX() - x), std::min(z - minZ(), maxZ() - z));
}

namespace {
// Catmull-Rom weights for the four samples around a point at fraction t.
void Weights(f32 t, f32 w[4]) {
    const f32 t2 = t * t, t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
    w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
    w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}
} // namespace

f32 DemHeightmap::sample(f32 x, f32 z) const {
    const f32 u = std::clamp((x - m_originX) / m_cell, 0.0f, f32(m_width - 1)), v = std::clamp((z - m_originZ) / m_cell, 0.0f, f32(m_height - 1));
    const i32 i = std::min(i32(u), i32(m_width) - 2), j = std::min(i32(v), i32(m_height) - 2);
    f32 wx[4], wz[4];
    Weights(u - f32(i), wx);
    Weights(v - f32(j), wz);
    f32 sum = 0.0f;
    for (int dj = 0; dj < 4; ++dj) {
        const i32 jj = std::clamp(j - 1 + dj, 0, i32(m_height) - 1);
        f32 row = 0.0f;
        for (int di = 0; di < 4; ++di) {
            const i32 ii = std::clamp(i - 1 + di, 0, i32(m_width) - 1);
            row += wx[di] * f32(m_data[size_t(jj) * m_width + size_t(ii)]);
        }
        sum += wz[dj] * row;
    }
    return sum * m_unit;
}

} // namespace foh
