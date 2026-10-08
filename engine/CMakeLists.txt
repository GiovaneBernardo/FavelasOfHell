# The engine is a set of modules (OBJECT libraries with explicit sources) folded
# into ONE shared library, gtabr_engine. Editor, runtime and game module all link
# this DLL, so they share a single copy of engine state. This is what makes
# reloading the game DLL safe, and it leaves the door open to splitting engine
# modules into their own DLLs later for engine hot reload.
#
# Module dependency order (lowest first):
#   core -> platform -> ecs -> host
#   core -> assets, renderer, physics
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(ecs)
add_subdirectory(host)
add_subdirectory(assets)
add_subdirectory(renderer)
add_subdirectory(physics)

get_property(_gtabr_modules GLOBAL PROPERTY GTABR_ENGINE_MODULES)

add_library(gtabr_engine SHARED)
add_library(gtabr::engine ALIAS gtabr_engine)
# Linking object libraries directly pulls in their objects + usage requirements.
target_link_libraries(gtabr_engine
    PUBLIC  ${_gtabr_modules}
    PRIVATE Jolt::Jolt nvrhi SDL3::SDL3 zstd::libzstd lz4::lz4)
target_compile_definitions(gtabr_engine PRIVATE GTABR_ENGINE_BUILD)
set_target_properties(gtabr_engine PROPERTIES OUTPUT_NAME "gtabr_engine" FOLDER "engine")
gtabr_target_defaults(gtabr_engine)
