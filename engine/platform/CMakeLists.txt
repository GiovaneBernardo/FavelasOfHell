gtabr_add_engine_module(platform
    SOURCES
        src/dynamic_library.cpp
        src/file_system.cpp
        src/process.cpp
        src/window.cpp
    PUBLIC_DEPS gtabr::core
    PRIVATE_DEPS SDL3::SDL3)
