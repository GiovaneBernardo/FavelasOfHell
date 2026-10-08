gtabr_add_engine_module(host
    SOURCES
        src/game_module_host.cpp
        src/dev_builder.cpp
    PUBLIC_DEPS gtabr::core gtabr::platform gtabr::ecs)
