add_executable(gtabr_tests
    main.cpp
    test_ecs.cpp
    test_math.cpp
    test_assets.cpp)
target_link_libraries(gtabr_tests PRIVATE gtabr::engine doctest::doctest)
set_target_properties(gtabr_tests PROPERTIES FOLDER "tests")
gtabr_target_defaults(gtabr_tests)

add_test(NAME gtabr_tests COMMAND gtabr_tests)
