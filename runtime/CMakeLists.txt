# Standalone player. Hosts the game module (and hot reloads it too).
# Links nvrhi only for the RHI-tier headers (Swapchain); it draws through Renderer.
add_executable(foh_runtime src/main.cpp)
target_link_libraries(foh_runtime PRIVATE foh::engine nvrhi)
target_compile_definitions(foh_runtime PRIVATE
    FOH_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    FOH_GAME_MODULE_FILE="modules/$<TARGET_FILE_NAME:foh_game>")
# Building the runtime always builds the current game module and shaders as well.
add_dependencies(foh_runtime foh_game)
if(TARGET foh_shaders)
    add_dependencies(foh_runtime foh_shaders)
endif()
set_target_properties(foh_runtime PROPERTIES FOLDER "apps")
foh_target_defaults(foh_runtime)
foh_enable_dev_builder(foh_runtime foh_game)
