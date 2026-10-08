# The editor: a separate executable that hosts the engine and the game module.
# Nothing in engine/ may depend on this directory.
add_executable(gtabr_editor src/main.cpp)
target_link_libraries(gtabr_editor PRIVATE gtabr::engine imgui::imgui)
target_compile_definitions(gtabr_editor PRIVATE
    GTABR_GAME_MODULE_FILE="modules/$<TARGET_FILE_NAME:gtabr_game>")
add_dependencies(gtabr_editor gtabr_game)
set_target_properties(gtabr_editor PROPERTIES FOLDER "apps")
gtabr_target_defaults(gtabr_editor)
gtabr_enable_dev_builder(gtabr_editor gtabr_game)
