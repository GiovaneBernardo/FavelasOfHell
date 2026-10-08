# flecs is an implementation detail of this module: it is linked privately and
# its headers are only included from src/. Public headers stay backend-free.
gtabr_add_engine_module(ecs
    SOURCES
        src/access.cpp
        src/app.cpp
        src/commands.cpp
        src/events.cpp
        src/query.cpp
        src/world.cpp
    PUBLIC_DEPS gtabr::core gtabr::jobs
    PRIVATE_DEPS flecs::flecs_static)
