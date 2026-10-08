cmake_minimum_required(VERSION 3.25)

project(gtabr
    VERSION 0.0.1
    DESCRIPTION "Brazilian open-world game: custom C++ engine, editor and game"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GTABR_BUILD_EDITOR   "Build the editor"          ON)
option(GTABR_BUILD_TESTS    "Build unit tests"          ON)
option(GTABR_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GtaBRHelpers)

# All binaries (exes, the engine DLL and the game DLL) land in one directory.
# The hot-reload host finds the game module next to the executable, and the
# engine DLL is resolved from the same place by every module that links it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/lib")
endforeach()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Plain /Zi in every config, deliberately NOT /ZI (Edit and Continue): with /ZI the
# Visual Studio debugger keeps the compiler databases (vc140.pdb/.idb) open, so
# rebuilding while debugging fails with C1041/LNK1168. Our own DLL hot reload
# (engine/host) replaces Edit and Continue and keeps breakpoints working.
# Needs CMP0141 (NEW via cmake_minimum_required >= 3.25).
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

# ---------------------------------------------------------------------------
# Third-party (vcpkg manifest mode, see vcpkg.json)
# ---------------------------------------------------------------------------
find_package(glm CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(nvrhi CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
if(GTABR_BUILD_EDITOR)
    find_package(imgui CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Layers. Dependency direction:  game -> engine <- editor / runtime
# The engine never references editor/, runtime/ or game/.
# ---------------------------------------------------------------------------
add_subdirectory(engine)   # -> target gtabr::engine (shared library)
add_subdirectory(game)     # -> hot-reloadable game module (MODULE library)
add_subdirectory(runtime)  # -> standalone player executable
if(GTABR_BUILD_EDITOR)
    add_subdirectory(editor)
endif()

if(GTABR_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
