foh_add_engine_module(physics
    SOURCES
        src/physics_system.cpp
    PUBLIC_DEPS foh::core
    PRIVATE_DEPS Jolt::Jolt)
