# World authoring runtime (ADR 0012): infinite heightmap terrain (procedural placeholder base + sculpt layer), its LOD
# streaming and the vegetation on it. Roads, parcels and the building generator join here. Uses the renderer's NVRHI-free tier only.
foh_add_engine_module(world
    SOURCES
        src/city.cpp
        src/city_assets.cpp
        src/city_buildings.cpp
        src/city_data.cpp
        src/city_geometry.cpp
        src/city_props.cpp
        src/city_roads.cpp
        src/ground_patch.cpp
        src/heightmap.cpp
        src/roads.cpp
        src/terrain.cpp
        src/terrain_renderer.cpp
        src/vegetation.cpp
        src/vegetation_assets.cpp
    PUBLIC_DEPS foh::core foh::jobs foh::ecs foh::renderer)
target_include_directories(foh_world PRIVATE src)
