add_executable(foh_tests
    main.cpp
    test_ecs.cpp
    test_hot_reload.cpp
    test_jobs.cpp
    test_reflection.cpp
    test_relationships.cpp
    test_scheduling.cpp
    test_snapshot.cpp
    test_storage.cpp
    test_math.cpp
    test_render_graph.cpp
    test_renderer_scene.cpp
    test_observers.cpp
    test_assets.cpp)
# nvrhi: the render graph tests include the RHI tier (NVRHI types) directly.
target_link_libraries(foh_tests PRIVATE foh::engine doctest::doctest nvrhi)
set_target_properties(foh_tests PROPERTIES FOLDER "tests")
foh_target_defaults(foh_tests)

# A small reloadable module for the automated hot reload test (built like the
# real game module, but into the tests' own directory).
add_library(foh_test_module MODULE test_module/test_module.cpp)
target_link_libraries(foh_test_module PRIVATE foh::engine)
target_include_directories(foh_test_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(foh_test_module PROPERTIES
    OUTPUT_NAME test_module PREFIX "" FOLDER "tests"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_modules"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_modules"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_modules")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(foh_test_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin/test_modules"
        LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin/test_modules"
        PDB_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin/test_modules")
endforeach()
foh_target_defaults(foh_test_module)
foh_configure_hot_reload_module(foh_test_module)

add_dependencies(foh_tests foh_test_module)
target_include_directories(foh_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(foh_tests PRIVATE FOH_TEST_MODULE_PATH="$<TARGET_FILE:foh_test_module>")

add_test(NAME foh_tests COMMAND foh_tests)
