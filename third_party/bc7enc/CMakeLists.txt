# bc7enc_rdo (BC7) + rgbcx (BC1-BC5), MIT / public domain. Vendored: not in our vcpkg baseline.
# Only the plain block encoders are used (no RDO, no ISPC, no image I/O).
# Always optimized, even in Debug builds: block compression of a 4K texture is minutes in an unoptimized build.
# MSVC rejects /RTC1 together with /O2, so the runtime checks are dropped for this directory only.
if(MSVC)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()
add_library(bc7enc STATIC bc7enc.cpp rgbcx.cpp)
add_library(third_party::bc7enc ALIAS bc7enc)
target_include_directories(bc7enc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(bc7enc PRIVATE RGBCX_USE_SMALLER_TABLES=1) # 110 KB table instead of 450 KB, negligible quality loss
target_compile_features(bc7enc PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(bc7enc PRIVATE /w /O2) # third-party code: not held to our warning level, always optimized
else()
    target_compile_options(bc7enc PRIVATE -w -O2)
endif()
set_target_properties(bc7enc PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)

# Reference BC7 decoder: used by the tests to check the encoder's output, not linked into the engine.
add_library(bc7decomp STATIC bc7decomp.cpp)
add_library(third_party::bc7decomp ALIAS bc7decomp)
target_include_directories(bc7decomp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(bc7decomp PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(bc7decomp PRIVATE /w)
else()
    target_compile_options(bc7decomp PRIVATE -w)
endif()
set_target_properties(bc7decomp PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)
