# Standalone player. Hosts the game module (and hot reloads it too).
add_executable(gtabr_runtime src/main.cpp)
target_link_libraries(gtabr_runtime PRIVATE gtabr::engine)
target_compile_definitions(gtabr_runtime PRIVATE
    GTABR_GAME_MODULE_FILE="modules/$<TARGET_FILE_NAME:gtabr_game>")
# Building the runtime always builds the current game module as well.
add_dependencies(gtabr_runtime gtabr_game)
set_target_properties(gtabr_runtime PROPERTIES FOLDER "apps")
gtabr_target_defaults(gtabr_runtime)
gtabr_enable_dev_builder(gtabr_runtime gtabr_game)
