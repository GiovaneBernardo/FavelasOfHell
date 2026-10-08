# The engine is a set of modules (OBJECT libraries with explicit sources) folded
# into ONE shared library, foh_engine. Editor, runtime and game module all link
# this DLL, so they share a single copy of engine state. This is what makes
# reloading the game DLL safe, and it leaves the door open to splitting engine
# modules into their own DLLs later for engine hot reload.
#
# Module dependency order (lowest first):
#   core -> platform -> jobs -> ecs -> host   (jobs sets OS thread priorities through platform)
#   core -> platform -> host
#   core -> jobs, platform, ecs -> renderer   (the renderer extracts render data through ECS plugins)
#   core -> assets, physics
#   assets, ecs, renderer -> scene           (prefab instantiation, mesh upload, reimport sync)
add_subdirectory(core)
add_subdirectory(jobs)
add_subdirectory(platform)
add_subdirectory(ecs)
add_subdirectory(host)
add_subdirectory(assets)
add_subdirectory(renderer)
add_subdirectory(scene)
add_subdirectory(physics)

get_property(_foh_modules GLOBAL PROPERTY FOH_ENGINE_MODULES)

add_library(foh_engine SHARED)
add_library(foh::engine ALIAS foh_engine)
# Linking object libraries directly pulls in their objects + usage requirements.
target_link_libraries(foh_engine
    PUBLIC  ${_foh_modules}
    PRIVATE flecs::flecs_static Jolt::Jolt nvrhi SDL3::SDL3 zstd::libzstd lz4::lz4)
target_compile_definitions(foh_engine PRIVATE FOH_ENGINE_BUILD)
set_target_properties(foh_engine PROPERTIES OUTPUT_NAME "foh_engine" FOLDER "engine")
foh_target_defaults(foh_engine)
