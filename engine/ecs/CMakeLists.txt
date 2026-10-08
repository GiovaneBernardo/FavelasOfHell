# flecs is an implementation detail of this module: it is linked privately and
# its headers are only included from src/. Public headers stay backend-free.
foh_add_engine_module(ecs
    SOURCES
        src/access.cpp
        src/app.cpp
        src/buffer_pool.cpp
        src/commands.cpp
        src/events.cpp
        src/inspect.cpp
        src/query.cpp
        src/schema.cpp
        src/snapshot.cpp
        src/world.cpp
    PUBLIC_DEPS foh::core foh::jobs
    PRIVATE_DEPS flecs::flecs_static)
