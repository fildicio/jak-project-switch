# cmake/Deko3dShaders.cmake — the UAM build rule (AI-assisted)
#
# Plan item P1.3. Compiles .vert/.frag shader sources with UAM and embeds each
# compiled .dksh into a C header via cmake/EmbedBinary.cmake, so the NRO carries
# the shaders inside it (no shader files on the SD card).
#
# API:
#   dk_add_shaders(<out_headers_var> <gen_dir> <shader files...>)
#   <out_headers_var> - variable (set in PARENT_SCOPE) holding the generated header paths
#   <gen_dir>         - directory for the generated .dksh and .h files
#   <shader files...> - absolute paths to .vert/.frag sources
#
# The project targets CMake 3.10, so CMAKE_CURRENT_FUNCTION_LIST_DIR (3.16+) is
# not available. We capture the directory of this file at file scope instead.

set(_DK_SHADERS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(dk_add_shaders out_headers_var gen_dir)
    # DEVKITPRO must be set (provides tools/bin/uam).
    if(NOT DEFINED DEVKITPRO OR "${DEVKITPRO}" STREQUAL "")
        message(FATAL_ERROR "Deko3dShaders: DEVKITPRO is not set; cannot find uam")
    endif()

    # Create the generation directory once.
    file(MAKE_DIRECTORY "${gen_dir}")

    set(out_headers "")

    foreach(shader ${ARGN})
        # Stage from the extension: .vert -> vert, .frag -> frag.
        get_filename_component(ext "${shader}" EXT)
        if(ext STREQUAL ".vert")
            set(stage vert)
        elseif(ext STREQUAL ".frag")
            set(stage frag)
        else()
            message(FATAL_ERROR "Deko3dShaders: unrecognised stage for '${shader}' (need .vert or .frag)")
        endif()

        # name = <basename>_<stage> (e.g. splash_vert).
        get_filename_component(srcname "${shader}" NAME_WE)
        set(name "${srcname}_${stage}")
        set(dksh "${gen_dir}/${name}.dksh")
        set(header "${gen_dir}/${name}.h")

        add_custom_command(
            OUTPUT ${header} ${dksh}
            COMMAND ${DEVKITPRO}/tools/bin/uam --stage ${stage} -o ${dksh} ${shader}
            COMMAND ${CMAKE_COMMAND} -DIN=${dksh} -DOUT=${header} -DNAME=${name} -P ${_DK_SHADERS_DIR}/EmbedBinary.cmake
            DEPENDS ${shader} ${_DK_SHADERS_DIR}/EmbedBinary.cmake
            COMMENT "uam ${name}"
            VERBATIM
        )

        list(APPEND out_headers ${header})
    endforeach()

    set(${out_headers_var} ${out_headers} PARENT_SCOPE)
endfunction()
