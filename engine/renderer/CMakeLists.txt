# The renderer module has two header tiers (see AGENTS.md rule on third-party headers):
#   include/foh/renderer/  NVRHI-free API: components, scenes, viewports, Renderer.
#                          Safe for game modules.
#   include/foh/rhi/       the NVRHI adapter: device, swapchain, render graph, shader
#                          library, pipeline cache. Engine internals and the editor only;
#                          consumers must link `nvrhi` themselves for its headers.
set(_rhi_libs nvrhi)
if(WIN32)
    list(APPEND _rhi_libs nvrhi_d3d12 dxgi d3d12)
endif()

foh_add_engine_module(renderer
    SOURCES
        src/atmosphere.cpp
        src/clouds.cpp
        src/gpu_format.cpp
        src/material_store.cpp
        src/mesh_store.cpp
        src/post_process.cpp
        src/probes.cpp
        src/render_extract.cpp
        src/renderer.cpp
        src/shadow_cascades.cpp
        src/texture_store.cpp
        src/transform_propagation.cpp
        src/viewport_graph.cpp
        src/water.cpp
        src/rhi/graphics_device.cpp
        src/rhi/pipeline_cache.cpp
        src/rhi/render_graph.cpp
        src/rhi/render_graph_helpers.cpp
        src/rhi/shader_library.cpp
        src/rhi/swapchain.cpp
    PUBLIC_DEPS foh::core foh::jobs foh::platform foh::ecs
    PRIVATE_DEPS ${_rhi_libs})
target_include_directories(foh_renderer PRIVATE src)
