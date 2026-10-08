add_executable(gtabr_tests
    main.cpp
    test_ecs.cpp
    test_hot_reload.cpp
    test_jobs.cpp
    test_math.cpp
    test_assets.cpp)
target_link_libraries(gtabr_tests PRIVATE gtabr::engine doctest::doctest)
set_target_properties(gtabr_tests PROPERTIES FOLDER "tests")
gtabr_target_defaults(gtabr_tests)

# A small reloadable module for the automated hot reload test (built like the
# real game module, but into the tests' own directory).
add_library(gtabr_test_module MODULE test_module/test_module.cpp)
target_link_libraries(gtabr_test_module PRIVATE gtabr::engine)
target_include_directories(gtabr_test_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(gtabr_test_module PROPERTIES
    OUTPUT_NAME test_module PREFIX "" FOLDER "tests"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_modules"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_modules"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_modules")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(gtabr_test_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin/test_modules"
        LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin/test_modules"
        PDB_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin/test_modules")
endforeach()
gtabr_target_defaults(gtabr_test_module)
gtabr_configure_hot_reload_module(gtabr_test_module)

add_dependencies(gtabr_tests gtabr_test_module)
target_include_directories(gtabr_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gtabr_tests PRIVATE GTABR_TEST_MODULE_PATH="$<TARGET_FILE:gtabr_test_module>")

add_test(NAME gtabr_tests COMMAND gtabr_tests)
