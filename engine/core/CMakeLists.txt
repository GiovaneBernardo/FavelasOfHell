gtabr_add_engine_module(core
    SOURCES
        src/log.cpp
        src/assert.cpp
    PUBLIC_DEPS glm::glm)

# Engine math conventions, applied to every consumer of glm through core.
target_compile_definitions(gtabr_core PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # D3D/Vulkan clip space
    GLM_FORCE_SILENT_WARNINGS)
