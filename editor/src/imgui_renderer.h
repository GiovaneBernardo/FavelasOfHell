#pragma once

// Dear ImGui renderer on top of NVRHI and the render graph. Editor only.
//
// Frame flow (all on the main thread except the pass body):
//   ImGui::Render();
//   renderer.prepare(drawData, externalTextures)   // texture create/update/destroy, collect viewport images
//   ... renderer.renderWindow(...): build() adds a pass that calls
//   imgui.render(drawData, ctx) from its body.

#include <memory>
#include <vector>

#include <imgui.h>
#include <nvrhi/nvrhi.h>

namespace foh {

class Renderer;
class RgPassContext;

class ImGuiRenderer {
public:
    ImGuiRenderer();
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    bool init(Renderer& renderer);
    void shutdown();

    // Handles ImGui's texture requests (font atlas) and lists the textures the
    // draw data samples that this renderer does NOT own (viewport images). The
    // render graph pass must declare a read on each of them.
    void prepare(ImDrawData* drawData, std::vector<nvrhi::ITexture*>& externalTextures);

    // Records the draw lists; call from the body of the UI pass. Thread-safe only
    // in the sense that it runs on one recording thread at a time.
    void render(ImDrawData* drawData, RgPassContext& ctx);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
