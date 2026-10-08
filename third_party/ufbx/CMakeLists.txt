# ufbx: single-file C FBX loader (MIT / public domain), vendored because it is not in our vcpkg baseline.
enable_language(C)
add_library(ufbx STATIC ufbx.c)
add_library(third_party::ufbx ALIAS ufbx)
target_include_directories(ufbx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(ufbx PRIVATE /w) # third-party code: not held to our warning level
else()
    target_compile_options(ufbx PRIVATE -w)
endif()
set_target_properties(ufbx PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)
