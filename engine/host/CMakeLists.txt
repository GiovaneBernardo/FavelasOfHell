foh_add_engine_module(host
    SOURCES
        src/game_module_host.cpp
        src/dev_builder.cpp
    PUBLIC_DEPS foh::core foh::platform foh::ecs)
