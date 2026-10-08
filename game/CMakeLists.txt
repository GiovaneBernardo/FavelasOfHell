# The game: a MODULE library (DLL) that the editor/runtime load and hot reload.
# It links the shared engine DLL, so it never carries its own copy of engine state.
add_library(gtabr_game MODULE
    src/game_module.cpp)

target_link_libraries(gtabr_game PRIVATE gtabr::engine)
# The module (and its PDB) is built into bin/modules/, NOT next to the executables.
# A debugger resolves the DLL's relative PDB name by searching directories such as
# the process working directory (bin/); if the build's PDB sits there it gets
# opened and locked, and the next rebuild fails with LNK1201. The host loads a
# shadow copy (with its own PDB copy) from .hotreload/, which the debugger uses.
set(_game_out "${CMAKE_BINARY_DIR}/bin/modules")
set_target_properties(gtabr_game PROPERTIES
    OUTPUT_NAME gtabr_game
    PREFIX ""
    FOLDER "game"
    RUNTIME_OUTPUT_DIRECTORY "${_game_out}"
    LIBRARY_OUTPUT_DIRECTORY "${_game_out}"
    PDB_OUTPUT_DIRECTORY "${_game_out}")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(gtabr_game PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${_game_out}"
        LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${_game_out}"
        PDB_OUTPUT_DIRECTORY_${_cfg} "${_game_out}")
endforeach()

gtabr_target_defaults(gtabr_game)
gtabr_configure_hot_reload_module(gtabr_game)

# Precompiled header keeps incremental rebuilds of game code fast.
target_precompile_headers(gtabr_game PRIVATE
    <string>
    <vector>
    <format>)
