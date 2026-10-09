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
    "${CMAKE_CURRENT_SOURCE_DIR}/common/fullscreen.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/gbuffer.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/material.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/instance.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/atmosphere_core.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/atmosphere.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/clouds.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/clouds_march.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/water.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/noise.slangh"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/post.slangh")

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
    foh_shader(fullscreen.slang vsFullscreen vertex) # shared by every fullscreen pass
    foh_shader(depth.slang vsDepth vertex)
    foh_shader(depth.slang vsDepthMasked vertex)
    foh_shader(depth.slang psDepthMasked fragment)
    foh_shader(shadow.slang vsShadow vertex)
    foh_shader(shadow.slang vsShadowMasked vertex)
    foh_shader(shadow.slang psShadowMasked fragment)
    foh_shader(opaque.slang vsOpaque vertex)
    foh_shader(opaque.slang psOpaque fragment)
    foh_shader(opaque.slang psOpaqueMasked fragment)
    foh_shader(opaque.slang psPick fragment)
    foh_shader(opaque.slang psPickMasked fragment)
    foh_shader(deferred.slang psDeferred fragment)
    foh_shader(sky.slang psSky fragment)
    # Precomputed atmosphere (Bruneton): LUT generation (on demand) and the cache dump.
    foreach(_entry csTransmittance csDirectIrradiance csIndirectIrradiance csSingleScattering csScatteringDensity csMultipleScattering)
        foh_shader(atmosphere_precompute.slang ${_entry} compute)
    endforeach()
    foh_shader(atmosphere_pack.slang csPack2D compute)
    foh_shader(atmosphere_pack.slang csPack3D compute)
    # Volumetric clouds: noise volumes (generated once), half-resolution ray march, temporal resolve, composite.
    foreach(_entry csShape csDetail)
        foh_shader(clouds_noise.slang ${_entry} compute)
    endforeach()
    foh_shader(clouds_weather.slang csWeather compute)
    foh_shader(clouds.slang psClouds fragment)
    foh_shader(clouds_post.slang psCloudResolve fragment)
    foh_shader(clouds_post.slang psCloudComposite fragment)
    # Water: FFT wave simulation (compute, once per frame), the sky probe it reflects, and the surface.
    foreach(_entry csSpectrum csEvolve csFftH csFftV csFinalize csMip)
        foh_shader(water_sim.slang ${_entry} compute)
    endforeach()
    foh_shader(sky_probe.slang psSkyProbe fragment)
    foh_shader(water.slang vsWaterRing vertex)
    foh_shader(water.slang vsWaterCenter vertex)
    foh_shader(water.slang psWater fragment)
    foh_shader(grid.slang psGrid fragment)
    # Post-processing: bloom chain (Call of Duty: Advanced Warfare), luminance-histogram auto exposure, tonemap.
    foreach(_entry psBloomDownFirst psBloomDown psBloomUp)
        foh_shader(bloom.slang ${_entry} fragment)
    endforeach()
    foreach(_entry csHistogram csAverage)
        foh_shader(exposure.slang ${_entry} compute)
    endforeach()
    foh_shader(tonemap.slang psTonemap fragment)
    foh_shader(blit.slang psBlit fragment)
    foh_shader(imgui.slang vsImGui vertex)
    foh_shader(imgui.slang psImGui fragment)

    add_custom_target(foh_shaders ALL DEPENDS ${FOH_SHADER_OUTPUTS})
    set_target_properties(foh_shaders PROPERTIES FOLDER "shaders")
else()
    message(WARNING "slangc not found: shaders will not be built and the renderer cannot run. "
                    "Install the Vulkan SDK (sets VULKAN_SDK) or put slangc on PATH.")
endif()
