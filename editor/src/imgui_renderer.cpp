#include "imgui_renderer.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "foh/log.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/graphics_device.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/render_graph.h"
#include "foh/rhi/shader_library.h"

namespace foh {

struct ImGuiRenderer::Impl {
    Renderer* renderer = nullptr;
    nvrhi::IDevice* device = nullptr;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::InputLayoutHandle inputLayout;
    nvrhi::SamplerHandle sampler;
    nvrhi::BufferHandle constants;
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    u32 vertexCapacity = 0;
    u32 indexCapacity = 0;
    nvrhi::CommandListHandle uploadList;

    // Textures created for ImGui's own atlases (not viewport images).
    std::unordered_map<ImTextureData*, nvrhi::TextureHandle> owned;
    std::unordered_set<const nvrhi::ITexture*> ownedSet;

    std::vector<ImDrawVert> vertices; // scratch, reused every frame
    std::vector<ImDrawIdx> indices;
};

struct ImGuiConstants {
    float scale[2];
    float translate[2];
};

ImGuiRenderer::ImGuiRenderer() : m_impl(std::make_unique<Impl>()) {}
ImGuiRenderer::~ImGuiRenderer() { shutdown(); }

bool ImGuiRenderer::init(Renderer& renderer) {
    Impl& s = *m_impl;
    s.renderer = &renderer;
    s.device = renderer.device().nvrhi();

    nvrhi::BindingLayoutDesc ld;
    ld.visibility = nvrhi::ShaderType::All;
    ld.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    ld.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    s.layout = s.device->createBindingLayout(ld);

    nvrhi::VertexAttributeDesc attrs[3];
    attrs[0].name = "POSITION";
    attrs[0].format = nvrhi::Format::RG32_FLOAT;
    attrs[0].offset = offsetof(ImDrawVert, pos);
    attrs[0].elementStride = sizeof(ImDrawVert);
    attrs[1].name = "TEXCOORD";
    attrs[1].format = nvrhi::Format::RG32_FLOAT;
    attrs[1].offset = offsetof(ImDrawVert, uv);
    attrs[1].elementStride = sizeof(ImDrawVert);
    attrs[2].name = "COLOR";
    attrs[2].format = nvrhi::Format::RGBA8_UNORM;
    attrs[2].offset = offsetof(ImDrawVert, col);
    attrs[2].elementStride = sizeof(ImDrawVert);
    s.inputLayout = s.device->createInputLayout(attrs, 3, nullptr);

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    s.sampler = s.device->createSampler(sd);

    nvrhi::BufferDesc cb;
    cb.byteSize = sizeof(ImGuiConstants);
    cb.isConstantBuffer = true;
    cb.isVolatile = true;
    cb.maxVersions = 16;
    cb.debugName = "ImGui constants";
    s.constants = s.device->createBuffer(cb);

    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    s.uploadList = s.device->createCommandList(params);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "foh_nvrhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    return s.layout && s.inputLayout && s.sampler && s.constants;
}

void ImGuiRenderer::shutdown() {
    Impl& s = *m_impl;
    if (!s.device) return;
    // Let ImGui know its textures are gone (it may outlive us during teardown).
    for (auto& [data, tex] : s.owned) {
        data->SetTexID(ImTextureID_Invalid);
        data->SetStatus(ImTextureStatus_Destroyed);
    }
    s.owned.clear();
    s.ownedSet.clear();
    s.vertexBuffer = nullptr;
    s.indexBuffer = nullptr;
    s.constants = nullptr;
    s.uploadList = nullptr;
    s.sampler = nullptr;
    s.inputLayout = nullptr;
    s.layout = nullptr;
    s.device = nullptr;
}

void ImGuiRenderer::prepare(ImDrawData* drawData, std::vector<nvrhi::ITexture*>& externalTextures) {
    Impl& s = *m_impl;
    externalTextures.clear();
    if (!drawData || !drawData->Valid) return;

    // 1. Texture requests from ImGui (font atlas growth, reloads).
    bool openedList = false;
    auto ensureOpen = [&] {
        if (!openedList) {
            s.uploadList->open();
            openedList = true;
        }
    };
    if (drawData->Textures) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                const bool create = tex->Status == ImTextureStatus_WantCreate;
                nvrhi::TextureHandle& handle = s.owned[tex];
                if (create || !handle) {
                    nvrhi::TextureDesc td;
                    td.width = static_cast<uint32_t>(tex->Width);
                    td.height = static_cast<uint32_t>(tex->Height);
                    td.format = nvrhi::Format::RGBA8_UNORM;
                    td.initialState = nvrhi::ResourceStates::ShaderResource;
                    td.keepInitialState = true;
                    td.debugName = "ImGui texture";
                    handle = s.device->createTexture(td);
                    s.ownedSet.insert(handle.Get());
                }
                ensureOpen();
                if (tex->Format == ImTextureFormat_RGBA32) {
                    s.uploadList->writeTexture(handle, 0, 0, tex->GetPixels(), static_cast<size_t>(tex->GetPitch()));
                } else {
                    // Alpha8 atlas: expand to white + alpha.
                    std::vector<unsigned char> rgba(static_cast<size_t>(tex->Width) * tex->Height * 4);
                    const unsigned char* src = static_cast<unsigned char*>(tex->GetPixels());
                    for (size_t i = 0; i < static_cast<size_t>(tex->Width) * tex->Height; ++i) {
                        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                        rgba[i * 4 + 3] = src[i];
                    }
                    s.uploadList->writeTexture(handle, 0, 0, rgba.data(), static_cast<size_t>(tex->Width) * 4);
                }
                tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(handle.Get())));
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
                auto it = s.owned.find(tex);
                if (it != s.owned.end()) {
                    s.ownedSet.erase(it->second.Get());
                    s.owned.erase(it); // the GPU may still read it this frame: NVRHI defers the free
                }
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }
    if (openedList) {
        s.uploadList->close();
        nvrhi::ICommandList* list = s.uploadList.Get();
        s.device->executeCommandLists(&list, 1);
    }

    // 2. Textures sampled by the draw lists that are not ours: the pass has to declare them.
    std::unordered_set<nvrhi::ITexture*> seen;
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            const ImTextureID id = cmd.GetTexID();
            if (id == ImTextureID_Invalid) continue;
            auto* tex = reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(id));
            if (s.ownedSet.count(tex) || !seen.insert(tex).second) continue;
            externalTextures.push_back(tex);
        }
    }
}

void ImGuiRenderer::render(ImDrawData* drawData, RgPassContext& ctx) {
    Impl& s = *m_impl;
    if (!drawData || drawData->TotalVtxCount == 0 || drawData->TotalIdxCount == 0) return;
    const float fbWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const float fbHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (fbWidth <= 0.0f || fbHeight <= 0.0f) return;

    nvrhi::ICommandList* cmd = ctx.cmd();

    // Buffers grow with slack so they are not recreated every frame.
    if (!s.vertexBuffer || s.vertexCapacity < static_cast<u32>(drawData->TotalVtxCount)) {
        s.vertexCapacity = static_cast<u32>(drawData->TotalVtxCount) + 5000;
        nvrhi::BufferDesc bd;
        bd.byteSize = static_cast<uint64_t>(s.vertexCapacity) * sizeof(ImDrawVert);
        bd.isVertexBuffer = true;
        bd.initialState = nvrhi::ResourceStates::VertexBuffer;
        bd.keepInitialState = true;
        bd.debugName = "ImGui vertices";
        s.vertexBuffer = s.device->createBuffer(bd);
    }
    if (!s.indexBuffer || s.indexCapacity < static_cast<u32>(drawData->TotalIdxCount)) {
        s.indexCapacity = static_cast<u32>(drawData->TotalIdxCount) + 10000;
        nvrhi::BufferDesc bd;
        bd.byteSize = static_cast<uint64_t>(s.indexCapacity) * sizeof(ImDrawIdx);
        bd.isIndexBuffer = true;
        bd.initialState = nvrhi::ResourceStates::IndexBuffer;
        bd.keepInitialState = true;
        bd.debugName = "ImGui indices";
        s.indexBuffer = s.device->createBuffer(bd);
    }

    s.vertices.resize(static_cast<size_t>(drawData->TotalVtxCount));
    s.indices.resize(static_cast<size_t>(drawData->TotalIdxCount));
    ImDrawVert* vtx = s.vertices.data();
    ImDrawIdx* idx = s.indices.data();
    for (const ImDrawList* list : drawData->CmdLists) {
        std::copy(list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size, vtx);
        std::copy(list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size, idx);
        vtx += list->VtxBuffer.Size;
        idx += list->IdxBuffer.Size;
    }
    cmd->writeBuffer(s.vertexBuffer, s.vertices.data(), s.vertices.size() * sizeof(ImDrawVert));
    cmd->writeBuffer(s.indexBuffer, s.indices.data(), s.indices.size() * sizeof(ImDrawIdx));

    ImGuiConstants constants;
    constants.scale[0] = 2.0f / drawData->DisplaySize.x;
    constants.scale[1] = -2.0f / drawData->DisplaySize.y;
    constants.translate[0] = -1.0f - drawData->DisplayPos.x * constants.scale[0];
    constants.translate[1] = 1.0f - drawData->DisplayPos.y * constants.scale[1];
    cmd->writeBuffer(s.constants, &constants, sizeof(constants));

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.renderer->shaders().get("imgui.vsImGui", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.renderer->shaders().get("imgui.psImGui", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = s.renderer->pipelines().graphics(HashCombine(vs.uid, ps.uid), fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.inputLayout = s.inputLayout;
        pd.VS = vs.shader;
        pd.PS = ps.shader;
        pd.bindingLayouts = {s.layout};
        pd.renderState.depthStencilState.depthTestEnable = false;
        pd.renderState.depthStencilState.depthWriteEnable = false;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pd.renderState.rasterState.scissorEnable = true;
        auto& rt = pd.renderState.blendState.targets[0];
        rt.setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::One)
            .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
        return pd;
    });
    if (!pso) return;

    const ImVec2 clipOff = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    u32 vtxOffset = 0, idxOffset = 0;
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& dc : list->CmdBuffer) {
            if (dc.UserCallback) {
                // ImDrawCallback_ResetRenderState is the only callback we could meaningfully support;
                // our state is set per draw anyway. Other callbacks are unsupported.
                continue;
            }
            const float minX = std::max((dc.ClipRect.x - clipOff.x) * clipScale.x, 0.0f);
            const float minY = std::max((dc.ClipRect.y - clipOff.y) * clipScale.y, 0.0f);
            const float maxX = std::min((dc.ClipRect.z - clipOff.x) * clipScale.x, fbWidth);
            const float maxY = std::min((dc.ClipRect.w - clipOff.y) * clipScale.y, fbHeight);
            if (maxX <= minX || maxY <= minY) continue;

            auto* tex = reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(dc.GetTexID()));
            if (!tex) continue;
            nvrhi::BindingSetDesc bd;
            bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, s.constants));
            bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, tex));
            bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.sampler));

            nvrhi::GraphicsState gs;
            gs.setPipeline(pso).setFramebuffer(fb);
            gs.viewport.addViewport(nvrhi::Viewport(fbWidth, fbHeight));
            gs.viewport.addScissorRect(nvrhi::Rect(static_cast<int>(minX), static_cast<int>(maxX),
                                                   static_cast<int>(minY), static_cast<int>(maxY)));
            gs.addBindingSet(ctx.bindingSet(bd, s.layout));
            gs.addVertexBuffer({s.vertexBuffer, 0, 0});
            gs.setIndexBuffer({s.indexBuffer, sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT, 0});
            cmd->setGraphicsState(gs);
            cmd->drawIndexed(nvrhi::DrawArguments()
                                 .setVertexCount(dc.ElemCount)
                                 .setStartIndexLocation(idxOffset + dc.IdxOffset)
                                 .setStartVertexLocation(vtxOffset + dc.VtxOffset));
        }
        vtxOffset += static_cast<u32>(list->VtxBuffer.Size);
        idxOffset += static_cast<u32>(list->IdxBuffer.Size);
    }
}

} // namespace foh
