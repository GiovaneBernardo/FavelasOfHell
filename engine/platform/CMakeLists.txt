foh_add_engine_module(platform
    SOURCES
        src/dynamic_library.cpp
        src/file_system.cpp
        src/png.cpp
        src/process.cpp
        src/shell.cpp
        src/thread_priority.cpp
        src/window.cpp
    PUBLIC_DEPS foh::core
    PRIVATE_DEPS SDL3::SDL3)
