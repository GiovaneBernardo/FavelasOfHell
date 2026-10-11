#include "probes.h"

#include "foh/renderer/renderer.h"

namespace foh {

void ProbeFaceViews(const Vec3& position, Mat4 view[kProbeFaces], Mat4& proj) {
    static const Vec3 kDir[kProbeFaces] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 kUp[kProbeFaces] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
    for (u32 f = 0; f < kProbeFaces; ++f) view[f] = glm::lookAtRH(position, position + kDir[f], kUp[f]);
    proj = MakePerspectiveReverseZ(1.57079633f, 1.0f, kProbeNear, kProbeFar);
}

void ProbeGrid::reset() { m_slots.clear(); }

bool ProbeGrid::ensureTextures(nvrhi::IDevice* device) {
    if (m_sh[0]) return true;
    for (u32 i = 0; i < 4; ++i) {
        nvrhi::TextureDesc td;
        td.width = td.height = kProbeGridDim;
        td.arraySize = kProbeLayers;
        td.dimension = nvrhi::TextureDimension::Texture2DArray;
        td.format = nvrhi::Format::RGBA16_FLOAT;
        td.isUAV = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource; // imported into the graph with this as initial AND final state
        td.keepInitialState = false;
        td.debugName = "LightProbeSH";
        m_sh[i] = device->createTexture(td);
        if (!m_sh[i]) {
            release();
            return false;
        }
    }
    m_needsClear = true;
    m_slots.clear();
    return true;
}

void ProbeGrid::release() {
    for (auto& t : m_sh) t = nullptr;
    m_slots.clear();
}

} // namespace foh
