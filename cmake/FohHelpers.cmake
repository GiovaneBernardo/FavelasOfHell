# Shared helpers for all targets in the repo.

# Compiler flags / warnings / language level applied to every first-party target.
function(foh_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
            /wd4251 # STL members in exported classes: fine, single CRT/DLL set
            $<$<BOOL:${FOH_WARNINGS_AS_ERRORS}>:/WX>)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            $<$<BOOL:${FOH_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
endfunction()

# An engine module: an OBJECT library with explicit sources. All modules are
# folded into the single shared library `foh_engine` (see engine/CMakeLists.txt)
# so that the editor, runtime and the (reloadable) game module share ONE copy of
# engine state (ECS world, asset manager, renderer...).
#
#   foh_add_engine_module(<name>
#       SOURCES <files...>        # explicit, no globbing
#       PUBLIC_DEPS <targets...>  # visible to consumers
#       PRIVATE_DEPS <targets...>)
function(foh_add_engine_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    set(target foh_${name})
    add_library(${target} OBJECT ${ARG_SOURCES})
    add_library(foh::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    target_compile_definitions(${target} PRIVATE FOH_ENGINE_BUILD)
    foh_target_defaults(${target})
    set_target_properties(${target} PROPERTIES FOLDER "engine")
    set_property(GLOBAL APPEND PROPERTY FOH_ENGINE_MODULES ${target})
endfunction()

# Hot-reload friendly settings for a game module (MODULE library).
function(foh_configure_hot_reload_module target)
    if(MSVC)
        # Embed the PDB *file name only* in the DLL. The host copies
        # <module>.dll + <module>.pdb to a shadow directory before loading, so
        # the debugger picks the PDB next to the copy and the build output is
        # never locked.
        target_link_options(${target} PRIVATE /PDBALTPATH:%_PDB%)
    endif()
    # Keep incremental rebuilds fast: no unity build for hot-reloadable code.
    set_target_properties(${target} PROPERTIES UNITY_BUILD OFF)
endfunction()

# Development-only auto build: the host (editor/runtime) rebuilds the game module
# when a file in game/ is saved, even while running under a debugger (where the
# IDE's Build command is disabled). Not enabled in Release builds.
function(foh_enable_dev_builder target game_target)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        return()
    endif()
    set(_vcvars "")
    if(MSVC)
        # <VS>/VC/Tools/MSVC/<ver>/bin/.../cl.exe  ->  <VS>/VC/Auxiliary/Build/vcvars64.bat
        string(REGEX REPLACE "/Tools/MSVC/.*$" "/Auxiliary/Build/vcvars64.bat" _vcvars "${CMAKE_CXX_COMPILER}")
        if(NOT EXISTS "${_vcvars}")
            set(_vcvars "")
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE
        FOH_DEV_BUILD_DIR="${CMAKE_BINARY_DIR}"
        FOH_DEV_CMAKE="${CMAKE_COMMAND}"
        FOH_DEV_VCVARS="${_vcvars}"
        FOH_DEV_GAME_TARGET="${game_target}"
        FOH_DEV_GAME_SOURCE_DIR="${CMAKE_SOURCE_DIR}/game")
    # Shader hot rebuild (slangc), when the shaders target exists.
    if(TARGET foh_shaders)
        target_compile_definitions(${target} PRIVATE
            FOH_DEV_SHADER_TARGET="foh_shaders"
            FOH_DEV_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders")
    endif()
endfunction()
