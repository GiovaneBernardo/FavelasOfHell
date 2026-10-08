# The editor: a separate executable that hosts the engine and the game module.
# Nothing in engine/ may depend on this directory.
#
# It links nvrhi directly because it includes the RHI-tier engine headers
# (foh/rhi/*) and renders the UI through them; Dear ImGui's platform glue is ours
# (imgui_platform.cpp) because SDL stays private to the engine DLL.
add_executable(foh_editor
    src/editor.cpp
    src/imgui_platform.cpp
    src/imgui_renderer.cpp
    src/main.cpp)
target_link_libraries(foh_editor PRIVATE foh::engine nvrhi imgui::imgui)
target_compile_definitions(foh_editor PRIVATE
    FOH_GAME_MODULE_FILE="modules/$<TARGET_FILE_NAME:foh_game>")
add_dependencies(foh_editor foh_game)
if(TARGET foh_shaders)
    add_dependencies(foh_editor foh_shaders)
endif()
set_target_properties(foh_editor PROPERTIES FOLDER "apps")
foh_target_defaults(foh_editor)
foh_enable_dev_builder(foh_editor foh_game)
