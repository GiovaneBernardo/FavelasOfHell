# Skeletal animation runtime (ADR 0009): pose math (sampling, blending, inertialization, skinning matrices), the
# animation library (loaded skeletons/clips), the locomotion blend space and the ECS plugin that drives it.
# It produces SkinPalette components; the renderer consumes them (it does not depend on this module).
foh_add_engine_module(animation
    SOURCES
        src/animation_library.cpp
        src/animation_plugin.cpp
        src/inertialization.cpp
        src/locomotion.cpp
        src/move_set.cpp
        src/pose.cpp
    PUBLIC_DEPS foh::core foh::jobs foh::ecs foh::assets foh::renderer)
target_include_directories(foh_animation PRIVATE src)
