# Shader binaries: Slang -> DXIL (D3D12) and/or SPIR-V (Vulkan), one file per
# entry point, written to <bin>/shaders/<file>.<entry>.<ext>. The engine ShaderLibrary
# loads them from there and hot reloads them when they change.
#
# slangc comes from the Vulkan SDK (or any PATH install). Without it the shaders
# target is not created and the renderer cannot start; install the Vulkan SDK
# (it ships slangc) or the vcpkg "shaders" feature.

find_program(FOH_SLANGC slangc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shader-slang")

if(WIN32)
    set(FOH_SHADER_TARGETS dxil)
else()
    set(FOH_SHADER_TARGETS spirv)
endif()
set(FOH_SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/bin/shaders")

set(_common_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/common/view.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/fullscreen.slangh")

set(FOH_SHADER_OUTPUTS "")

# foh_shader(<file.slang> <entry> <stage>)
function(foh_shader file entry stage)
    get_filename_component(_base "${file}" NAME_WE)
    foreach(_target IN LISTS FOH_SHADER_TARGETS)
        if(_target STREQUAL "dxil")
            set(_ext dxil)
            set(_args -target dxil -profile sm_6_2)
        else()
            set(_ext spv)
            set(_args -target spirv -profile spirv_1_5)
        endif()
        set(_out "${FOH_SHADER_OUT_DIR}/${_base}.${entry}.${_ext}")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${FOH_SHADER_OUT_DIR}"
            COMMAND "${FOH_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/${file}"
                    -entry ${entry} -stage ${stage} ${_args}
                    -matrix-layout-column-major -O2
                    -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    -o "${_out}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${file}" ${_common_headers}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "slangc ${file}:${entry} -> ${_ext}"
            VERBATIM)
        list(APPEND FOH_SHADER_OUTPUTS "${_out}")
    endforeach()
    set(FOH_SHADER_OUTPUTS "${FOH_SHADER_OUTPUTS}" PARENT_SCOPE)
endfunction()

if(FOH_SLANGC)
    foh_shader(mesh_forward.slang vsMesh vertex)
    foh_shader(mesh_forward.slang psMesh fragment)
    foh_shader(sky.slang vsFullscreen vertex)
    foh_shader(sky.slang psSky fragment)
    foh_shader(grid.slang vsFullscreen vertex)
    foh_shader(grid.slang psGrid fragment)
    foh_shader(tonemap.slang vsFullscreen vertex)
    foh_shader(tonemap.slang psTonemap fragment)
    foh_shader(blit.slang vsFullscreen vertex)
    foh_shader(blit.slang psBlit fragment)
    foh_shader(imgui.slang vsImGui vertex)
    foh_shader(imgui.slang psImGui fragment)

    add_custom_target(foh_shaders ALL DEPENDS ${FOH_SHADER_OUTPUTS})
    set_target_properties(foh_shaders PROPERTIES FOLDER "shaders")
else()
    message(WARNING "slangc not found: shaders will not be built and the renderer cannot run. "
                    "Install the Vulkan SDK (sets VULKAN_SDK) or put slangc on PATH.")
endif()
