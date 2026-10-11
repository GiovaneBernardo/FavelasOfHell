# The editor: a separate executable that hosts the engine and the game module.
# Nothing in engine/ may depend on this directory.
#
# It links nvrhi directly because it includes the RHI-tier engine headers
# (foh/rhi/*) and renders the UI through them; Dear ImGui's platform glue is ours
# (imgui_platform.cpp) because SDL stays private to the engine DLL.
find_path(FOH_STB_INCLUDE_DIR stb_image.h REQUIRED)
# Assets Workshop: HTTPS downloads (libcurl, multi interface) and zip extraction (miniz).
find_package(CURL CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
add_executable(foh_editor
    src/asset_forge.cpp
    src/atmosphere_panel.cpp
    src/asset_inspector.cpp
    src/asset_picker.cpp
    src/content_browser.cpp
    src/editor.cpp
    src/editor_prefs.cpp
    src/file_icons.cpp
    src/forge_character.cpp
    src/forge_prop.cpp
    src/forge_vehicle.cpp
    src/game_assets_panel.cpp
    src/imgui_platform.cpp
    src/imgui_renderer.cpp
    src/inspector.cpp
    src/main.cpp
    src/prefab_editor.cpp
    src/city_panel.cpp
    src/road_tool.cpp
    src/stb_impl.cpp
    src/terrain_panel.cpp
    src/theme.cpp
    src/thumb_loaders.cpp
    src/thumbnails.cpp
    src/water_test_scene.cpp
    src/workshop/http_client.cpp
    src/workshop/json.cpp
    src/workshop/material_recipe.cpp
    src/workshop/providers.cpp
    src/workshop/workshop.cpp
    src/workshop/zip_extract.cpp)
target_include_directories(foh_editor PRIVATE src src/workshop "${FOH_STB_INCLUDE_DIR}")
target_link_libraries(foh_editor PRIVATE foh::engine nvrhi imgui::imgui imguizmo::imguizmo CURL::libcurl miniz::miniz)
target_compile_definitions(foh_editor PRIVATE
    FOH_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    FOH_GAME_MODULE_FILE="modules/$<TARGET_FILE_NAME:foh_game>")
add_dependencies(foh_editor foh_game)
if(TARGET foh_shaders)
    add_dependencies(foh_editor foh_shaders)
endif()
set_target_properties(foh_editor PROPERTIES FOLDER "apps")
foh_target_defaults(foh_editor)
foh_enable_dev_builder(foh_editor foh_game)
