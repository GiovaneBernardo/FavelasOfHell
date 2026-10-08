foh_add_engine_module(assets
    SOURCES
        src/asset_container.cpp
    PUBLIC_DEPS foh::core
    PRIVATE_DEPS zstd::libzstd lz4::lz4)
