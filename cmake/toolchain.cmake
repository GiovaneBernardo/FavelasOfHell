# Toolchain wrapper that makes the repo self-contained: it locates vcpkg (or
# bootstraps a pinned copy into <repo>/.vcpkg) and then chains to the vcpkg
# toolchain. This is what lets "open folder in Visual Studio" just work, with
# no manual vcpkg install and no environment setup.
#
# Resolution order:
#   1. $VCPKG_ROOT (if set and valid)  - lets you share one vcpkg between repos
#   2. <repo>/.vcpkg                   - bootstrapped automatically on first configure

set(FOH_VCPKG_COMMIT "2750401336fb7c95f6619657a46a7e798661341c") # keep in sync with vcpkg.json "builtin-baseline"

get_filename_component(_foh_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(_vcpkg_root "$ENV{VCPKG_ROOT}")
else()
    set(_vcpkg_root "${_foh_root}/.vcpkg")
endif()

if(NOT EXISTS "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    find_package(Git REQUIRED)
    message(STATUS "foh: bootstrapping vcpkg ${FOH_VCPKG_COMMIT} into ${_vcpkg_root}")
    file(MAKE_DIRECTORY "${_vcpkg_root}")
    foreach(_cmd
        "init"
        "remote;add;origin;https://github.com/microsoft/vcpkg.git"
        "fetch;--depth;1;origin;${FOH_VCPKG_COMMIT}"
        "checkout;FETCH_HEAD")
        execute_process(COMMAND "${GIT_EXECUTABLE}" ${_cmd}
            WORKING_DIRECTORY "${_vcpkg_root}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "foh: git ${_cmd} failed while bootstrapping vcpkg")
        endif()
    endforeach()
endif()

if(NOT EXISTS "${_vcpkg_root}/vcpkg.exe" AND NOT EXISTS "${_vcpkg_root}/vcpkg")
    if(CMAKE_HOST_WIN32)
        set(_bootstrap "${_vcpkg_root}/bootstrap-vcpkg.bat")
    else()
        set(_bootstrap "${_vcpkg_root}/bootstrap-vcpkg.sh")
    endif()
    message(STATUS "foh: running ${_bootstrap}")
    execute_process(COMMAND "${_bootstrap}" -disableMetrics
        WORKING_DIRECTORY "${_vcpkg_root}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "foh: vcpkg bootstrap failed")
    endif()
endif()

set(ENV{VCPKG_ROOT} "${_vcpkg_root}")
set(VCPKG_DISABLE_METRICS ON)
include("${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
