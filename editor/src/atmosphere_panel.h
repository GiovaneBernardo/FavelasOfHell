#pragma once

#include "foh/renderer/renderer.h"

namespace foh {

// "Atmosphere" panel: edits the physical parameters of the precomputed sky and rebuilds its lookup tables on
// demand. The tables are cached on disk, so this is only needed after changing a parameter.
//
// Thread-safety: main thread (UI) only.
class AtmospherePanel {
public:
    void draw(Renderer& renderer, bool* open);

private:
    AtmosphereDesc m_draft; // what the sliders edit; applied by the Generate button
    bool m_inited = false;
};

} // namespace foh
