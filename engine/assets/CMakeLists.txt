# Header-only third-party code used by the importers (installed by vcpkg).
find_path(FOH_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
find_path(FOH_STB_INCLUDE_DIR stb_image.h REQUIRED)

foh_add_engine_module(assets
    SOURCES
        src/asset_container.cpp
        src/asset_types.cpp
        src/asset_db.cpp
        src/import/cook.cpp
        src/import/fbx_importer.cpp
        src/import/gltf_importer.cpp
        src/import/image_importer.cpp
        src/import/import_util.cpp
        src/import/texture_cook.cpp
    PUBLIC_DEPS foh::core foh::jobs
    PRIVATE_DEPS zstd::libzstd lz4::lz4 third_party::ufbx third_party::bc7enc)
target_include_directories(foh_assets PRIVATE "${FOH_CGLTF_INCLUDE_DIR}" "${FOH_STB_INCLUDE_DIR}")
