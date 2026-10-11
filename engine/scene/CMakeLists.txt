# Glue between assets, the renderer and worlds: instantiating prefabs into an ECS world,
# uploading imported meshes to the renderer and keeping instances in sync with reimports.
# Nothing here touches NVRHI (the renderer API it uses is the NVRHI-free tier).
foh_add_engine_module(scene
    SOURCES
        src/asset_loader.cpp
        src/asset_scene_bridge.cpp
        src/game_asset_request.cpp
        src/prefab_instance.cpp
        src/prefab_request.cpp
    PUBLIC_DEPS foh::core foh::ecs foh::assets foh::renderer foh::animation)
