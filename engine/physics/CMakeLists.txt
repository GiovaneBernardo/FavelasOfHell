gtabr_add_engine_module(physics
    SOURCES
        src/physics_system.cpp
    PUBLIC_DEPS gtabr::core
    PRIVATE_DEPS Jolt::Jolt)
