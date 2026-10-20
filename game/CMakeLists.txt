# Set a more convenient ARM flag
# match the __aarch64__ source guards
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" OG_PROCESSOR_LOWER)
if(OG_PROCESSOR_LOWER MATCHES "^(arm64|aarch64)$")
    set(ARM64_ARCH TRUE)
    message(STATUS "ARM64 architecture detected (${CMAKE_SYSTEM_PROCESSOR})")
else()
    set(ARM64_ARCH FALSE)
    message(STATUS "Non-ARM64 architecture detected (${CMAKE_SYSTEM_PROCESSOR})")
endif()

if(ARM64_ARCH)
    # Add your ARM64-specific configuration or build options here
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_arm64.s)
    enable_language(ASM)
    set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)
    # pass -arch only to Apple Clang
    if(APPLE)
        set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-arch arm64 -g")
    else()
        set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-g")
    endif()
else()
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_x86_64.asm)
    enable_language(ASM_NASM)
    set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS} asm)
    set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> -f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -o <OBJECT> <SOURCE>")
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-g")
endif()

set(RUNTIME_SOURCE
        external/discord_jak1.cpp
        external/discord_jak2.cpp
        external/discord_jak3.cpp
        external/discord_jakx.cpp
        external/discord.cpp
        graphics/display.cpp
        graphics/gfx_test.cpp
        graphics/gfx.cpp
        graphics/jak2_texture_remap.cpp
        graphics/jak3_texture_remap.cpp
        graphics/jakx_texture_remap.cpp
        graphics/screenshot.cpp
        graphics/opengl_renderer/background/background_common.cpp
        graphics/opengl_renderer/background/Hfrag.cpp
        graphics/opengl_renderer/background/Shrub.cpp
        graphics/opengl_renderer/background/TFragment.cpp
        graphics/opengl_renderer/background/Tie3.cpp
        graphics/opengl_renderer/BlitDisplays.cpp
        graphics/opengl_renderer/BucketRenderer.cpp
        graphics/opengl_renderer/CollideMeshRenderer.cpp
        graphics/opengl_renderer/debug_gui.cpp
        graphics/opengl_renderer/DepthCue.cpp
        graphics/opengl_renderer/DirectRenderer.cpp
        graphics/opengl_renderer/DirectRenderer2.cpp
        graphics/opengl_renderer/dma_helpers.cpp
        graphics/opengl_renderer/EyeRenderer.cpp
        graphics/opengl_renderer/foreground/Generic2_Build.cpp
        graphics/opengl_renderer/foreground/Generic2_DMA.cpp
        graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp
        graphics/opengl_renderer/foreground/Generic2.cpp
        graphics/opengl_renderer/foreground/Generic2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Merc2.cpp
        graphics/opengl_renderer/foreground/Merc2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Shadow2.cpp
        graphics/opengl_renderer/gl_time_split.cpp
        graphics/opengl_renderer/loader/Loader.cpp
        graphics/opengl_renderer/loader/LoaderStages.cpp
        graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp
        graphics/opengl_renderer/ocean/OceanEnvmap.cpp
        graphics/opengl_renderer/ocean/OceanMid_PS2.cpp
        graphics/opengl_renderer/ocean/OceanMid.cpp
        graphics/opengl_renderer/ocean/OceanMidAndFar.cpp
        graphics/opengl_renderer/ocean/OceanNear_PS2.cpp
        graphics/opengl_renderer/ocean/OceanNear.cpp
        graphics/opengl_renderer/ocean/OceanTexture_PC.cpp
        graphics/opengl_renderer/ocean/OceanTexture.cpp
        graphics/opengl_renderer/opengl_utils.cpp
        graphics/opengl_renderer/OpenGLRenderer.cpp
        graphics/opengl_renderer/Profiler.cpp
        graphics/opengl_renderer/ProgressRenderer.cpp
        graphics/opengl_renderer/Shader.cpp
        graphics/opengl_renderer/Shadow_PS2.cpp
        graphics/opengl_renderer/ShadowRenderer.cpp
        graphics/opengl_renderer/SkyBlendCPU.cpp
        graphics/opengl_renderer/SkyBlendGPU.cpp
        graphics/opengl_renderer/SkyRenderer.cpp
        graphics/opengl_renderer/SlowTimeEffect.cpp
        graphics/opengl_renderer/sprite/GlowRenderer.cpp
        graphics/opengl_renderer/sprite/Sprite3_Distort.cpp
        graphics/opengl_renderer/sprite/Sprite3_Glow.cpp
        graphics/opengl_renderer/sprite/Sprite3.cpp
        graphics/opengl_renderer/TextureAnimator.cpp
        graphics/opengl_renderer/TextureAnimatorDefs.cpp
        graphics/opengl_renderer/TextureUploadHandler.cpp
        graphics/opengl_renderer/VisDataHandler.cpp
        graphics/opengl_renderer/Warp.cpp
        graphics/pipelines/opengl.cpp
        graphics/pipelines/dk_renderer.cpp
        graphics/pipelines/dk_device.cpp
        graphics/deko3d_renderer/dk_state.cpp
        graphics/deko3d_renderer/dk_texture_gpu.cpp
        graphics/deko3d_renderer/dk_gl_census.cpp
        graphics/deko3d_renderer/dk_render_target.cpp
        graphics/deko3d_renderer/TFragmentDk.cpp
        graphics/deko3d_renderer/ShrubDk.cpp
        graphics/deko3d_renderer/TieDk.cpp
        graphics/deko3d_renderer/dk_tfrag_record.cpp
        graphics/deko3d_renderer/MercDk.cpp
        graphics/deko3d_renderer/dk_merc_record.cpp
        graphics/deko3d_renderer/SpriteDk.cpp
        graphics/deko3d_renderer/dk_sprite_record.cpp
        graphics/deko3d_renderer/DirectDk.cpp
        graphics/deko3d_renderer/dk_direct_record.cpp
        graphics/sceGraphicsInterface.cpp
        graphics/texture/jak1_tpage_dir.cpp
        graphics/texture/jak2_tpage_dir.cpp
        graphics/texture/jak3_tpage_dir.cpp
        graphics/texture/TextureConverter.cpp
        graphics/texture/TexturePool.cpp
        ${OG_ASM_FUNCS_FILE}
        kernel/common/fileio.cpp
        kernel/common/kboot.cpp
        kernel/common/kdgo.cpp
        kernel/common/kdsnetm.cpp
        kernel/common/klink.cpp
        kernel/common/klisten.cpp
        kernel/common/kmachine.cpp
        kernel/common/kmalloc.cpp
        kernel/common/kmemcard.cpp
        kernel/common/kprint.cpp
        kernel/common/kscheme.cpp
        kernel/common/ksocket.cpp
        kernel/common/ksound.cpp
        kernel/jak1/fileio.cpp
        kernel/jak1/kboot.cpp
        kernel/jak1/kdgo.cpp
        kernel/jak1/klink.cpp
        kernel/jak1/klisten.cpp
        kernel/jak1/kmachine.cpp
        kernel/jak1/kprint.cpp
        kernel/jak1/kscheme.cpp
        kernel/jak1/ksound.cpp
        kernel/jak2/fileio.cpp
        kernel/jak2/kboot.cpp
        kernel/jak2/kdgo.cpp
        kernel/jak2/klink.cpp
        kernel/jak2/klisten.cpp
        kernel/jak2/kmachine.cpp
        kernel/jak2/kmachine_extras.cpp
        kernel/jak2/kmalloc.cpp
        kernel/jak2/kprint.cpp
        kernel/jak2/kscheme.cpp
        kernel/jak2/ksound.cpp
        kernel/jak3/fileio.cpp
        kernel/jak3/kboot.cpp
        kernel/jak3/kdgo.cpp
        kernel/jak3/kdsnetm.cpp
        kernel/jak3/klink.cpp
        kernel/jak3/klisten.cpp
        kernel/jak3/kmachine.cpp
        kernel/jak3/kmachine_extras.cpp
        kernel/jak3/kmalloc.cpp
        kernel/jak3/kmemcard.cpp
        kernel/jak3/kprint.cpp
        kernel/jak3/kscheme.cpp
        kernel/jak3/ksocket.cpp
        kernel/jak3/ksound.cpp
        kernel/jakx/fileio.cpp
        kernel/jakx/kboot.cpp
        kernel/jakx/kdgo.cpp
        kernel/jakx/kdsnetm.cpp
        kernel/jakx/klink.cpp
        kernel/jakx/klisten.cpp
        kernel/jakx/kmachine.cpp
        kernel/jakx/kmachine_extras.cpp
        kernel/jakx/kmalloc.cpp
        kernel/jakx/kmemcard.cpp
        kernel/jakx/kprint.cpp
        kernel/jakx/kscheme.cpp
        kernel/jakx/ksocket.cpp
        kernel/jakx/ksound.cpp
        mips2c/jak1_functions/bones.cpp
        mips2c/jak1_functions/collide_cache.cpp
        mips2c/jak1_functions/collide_edge_grab.cpp
        mips2c/jak1_functions/collide_func.cpp
        mips2c/jak1_functions/collide_mesh.cpp
        mips2c/jak1_functions/collide_probe.cpp
        mips2c/jak1_functions/draw_string.cpp
        mips2c/jak1_functions/generic_effect.cpp
        mips2c/jak1_functions/generic_effect2.cpp
        mips2c/jak1_functions/generic_merc.cpp
        mips2c/jak1_functions/generic_tie.cpp
        mips2c/jak1_functions/joint.cpp
        mips2c/jak1_functions/merc_blend_shape.cpp
        mips2c/jak1_functions/ocean_vu0.cpp
        mips2c/jak1_functions/ocean.cpp
        mips2c/jak1_functions/ripple.cpp
        mips2c/jak1_functions/shadow.cpp
        mips2c/jak1_functions/sky_tng.cpp
        mips2c/jak1_functions/sparticle_launcher.cpp
        mips2c/jak1_functions/sparticle.cpp
        mips2c/jak1_functions/test_func.cpp
        mips2c/jak1_functions/texture.cpp
        mips2c/jak1_functions/tfrag.cpp
        mips2c/jak1_functions/tie_methods.cpp
        mips2c/jak1_functions/time_of_day.cpp
        mips2c/jak2_functions/bones.cpp
        mips2c/jak2_functions/collide_cache.cpp
        mips2c/jak2_functions/collide_edge_grab.cpp
        mips2c/jak2_functions/collide_func.cpp
        mips2c/jak2_functions/collide_hash.cpp
        mips2c/jak2_functions/collide_mesh.cpp
        mips2c/jak2_functions/debug.cpp
        mips2c/jak2_functions/font.cpp
        mips2c/jak2_functions/foreground.cpp
        mips2c/jak2_functions/generic_effect.cpp
        mips2c/jak2_functions/generic_merc.cpp
        mips2c/jak2_functions/joint.cpp
        mips2c/jak2_functions/lights.cpp
        mips2c/jak2_functions/merc_blend_shape.cpp
        mips2c/jak2_functions/nav_control.cpp
        mips2c/jak2_functions/nav_mesh.cpp
        mips2c/jak2_functions/ocean_vu0.cpp
        mips2c/jak2_functions/ocean.cpp
        mips2c/jak2_functions/ripple.cpp
        mips2c/jak2_functions/shadow.cpp
        mips2c/jak2_functions/sky.cpp
        mips2c/jak2_functions/sparticle_launcher.cpp
        mips2c/jak2_functions/sparticle.cpp
        mips2c/jak2_functions/spatial_hash.cpp
        mips2c/jak2_functions/squid.cpp
        mips2c/jak2_functions/texture.cpp
        mips2c/jak2_functions/vortex.cpp
        mips2c/jak3_functions/collide_func.cpp
        mips2c/jak3_functions/debug.cpp
        mips2c/jak3_functions/font.cpp
        mips2c/jak3_functions/foreground.cpp
        mips2c/jak3_functions/generic_effect.cpp
        mips2c/jak3_functions/joint.cpp
        mips2c/jak3_functions/lights.cpp
        mips2c/jak3_functions/sky.cpp
        mips2c/jak3_functions/texture.cpp
        mips2c/jak3_functions/particle_curves.cpp
        mips2c/jak3_functions/cloth.cpp
        mips2c/jak3_functions/collide_cache.cpp
        mips2c/jak3_functions/collide_hash.cpp
        mips2c/jak3_functions/collide_edge_grab.cpp
        mips2c/jak3_functions/collide_mesh.cpp
        mips2c/jak3_functions/spatial_hash.cpp
        mips2c/jak3_functions/sparticle.cpp
        mips2c/jak3_functions/sparticle_launcher.cpp
        mips2c/jak3_functions/nav_control.cpp
        mips2c/jak3_functions/nav_engine.cpp
        mips2c/jak3_functions/merc_blend_shape.cpp
        mips2c/jak3_functions/wvehicle_part.cpp
        mips2c/jak3_functions/ripple.cpp
        mips2c/jak3_functions/shadow.cpp
        mips2c/jak3_functions/ocean.cpp
        mips2c/jak3_functions/ocean_vu0.cpp
        mips2c/jak3_functions/generic_merc.cpp
        mips2c/mips2c_table.cpp
        overlord/common/dma.cpp
        overlord/common/fake_iso.cpp
        overlord/common/iso_api.cpp
        overlord/common/iso.cpp
        overlord/common/isocommon.cpp
        overlord/common/989_plugins/plugin_strv.cpp
        overlord/common/overlord.cpp
        overlord/common/sbank.cpp
        overlord/common/soundcommon.cpp
        overlord/common/srpc.cpp
        overlord/common/ssound.cpp
        overlord/jak1/dma.cpp
        overlord/jak1/fake_iso.cpp
        overlord/jak1/iso_api.cpp
        overlord/jak1/iso_queue.cpp
        overlord/jak1/iso.cpp
        overlord/jak1/isocommon.cpp
        overlord/jak1/overlord.cpp
        overlord/jak1/ramdisk.cpp
        overlord/jak1/srpc.cpp
        overlord/jak1/ssound.cpp
        overlord/jak1/stream.cpp
        overlord/jak2/dma.cpp
        overlord/jak2/iso_api.cpp
        overlord/jak2/iso_cd.cpp
        overlord/jak2/iso_queue.cpp
        overlord/jak2/iso.cpp
        overlord/jak2/list.cpp
        overlord/jak2/overlord.cpp
        overlord/jak2/pages.cpp
        overlord/jak2/989_plugins/plugin_strv.cpp
        overlord/jak2/spustreams.cpp
        overlord/jak2/srpc.cpp
        overlord/jak2/ssound.cpp
        overlord/jak2/stream.cpp
        overlord/jak2/streamlfo.cpp
        overlord/jak2/streamlist.cpp
        overlord/jak2/vag.cpp
        overlord/jak3/overlord.cpp
        overlord/jak3/pagemanager.cpp
        overlord/jak3/iso_cd.cpp
        overlord/jak3/dma.cpp
        overlord/jak3/iso.cpp
        overlord/jak3/iso_queue.cpp
        overlord/jak3/srpc.cpp
        overlord/jak3/vag.cpp
        overlord/jak3/ssound.cpp
        overlord/jak3/iso_api.cpp
        overlord/jak3/spustreams.cpp
        overlord/jak3/list.cpp
        overlord/jak3/vblank_handler.cpp
        overlord/jak3/dvd_driver.cpp
        overlord/jak3/basefile.cpp
        overlord/jak3/basefilesystem.cpp
        overlord/jak3/ramdisk.cpp
        overlord/jak3/isocommon.cpp
        overlord/jak3/init.cpp
        overlord/jak3/stream.cpp
        overlord/jak3/sbank.cpp
        overlord/jak3/soundcommon.cpp
        overlord/jak3/streamlist.cpp
        overlord/jak3/989_plugins/plugin_strv.cpp
        overlord/jakx/overlord.cpp
        overlord/jakx/pagemanager.cpp
        overlord/jakx/iso_cd.cpp
        overlord/jakx/dma.cpp
        overlord/jakx/iso.cpp
        overlord/jakx/iso_queue.cpp
        overlord/jakx/srpc.cpp
        overlord/jakx/vag.cpp
        overlord/jakx/ssound.cpp
        overlord/jakx/iso_api.cpp
        overlord/jakx/spustreams.cpp
        overlord/jakx/list.cpp
        overlord/jakx/vblank_handler.cpp
        overlord/jakx/dvd_driver.cpp
        overlord/jakx/basefile.cpp
        overlord/jakx/basefilesystem.cpp
        overlord/jakx/ramdisk.cpp
        overlord/jakx/isocommon.cpp
        overlord/jakx/init.cpp
        overlord/jakx/stream.cpp
        overlord/jakx/sbank.cpp
        overlord/jakx/soundcommon.cpp
        overlord/jakx/streamlist.cpp
        runtime.cpp
        sce/deci2.cpp
        sce/iop.cpp
        sce/libcdvd_ee.cpp
        sce/libdma.cpp
        sce/libgraph.cpp
        sce/libpad.cpp
        sce/libscf.cpp
        sce/sif_ee_memcard.cpp
        sce/sif_ee.cpp
        sce/stubs.cpp
        settings/settings.cpp
        switch/platform.cpp
        system/background_worker.cpp
        system/Deci2Server.cpp
        system/hid/devices/dualsense_effects.cpp
        system/hid/devices/game_controller.cpp
        system/hid/devices/keyboard.cpp
        system/hid/devices/mouse.cpp
        system/hid/display_manager.cpp
        system/hid/input_bindings.cpp
        system/hid/input_manager.cpp
        system/hid/sdl_util.cpp
        system/IOP_Kernel.cpp
        system/iop_thread.cpp
        system/SystemThread.cpp
        tools/filter_menu/filter_menu.cpp
        tools/subtitle_editor/subtitle_editor_db.cpp
        tools/subtitle_editor/subtitle_editor_repl_client.cpp
        tools/subtitle_editor/subtitle_editor.cpp
        )

add_subdirectory(sound)

# we build the runtime as a static library.
add_library(runtime STATIC ${RUNTIME_SOURCE} "../third-party/glad/src/glad.c")

if(NINTENDO_SWITCH)
    # No imgui/discord-rpc/libcurl builds on Switch (see top-level CMakeLists.txt) -- their
    # actual call sites are stubbed out in-place (discord_rpc_stub.h, background_worker.cpp,
    # and the #if !defined(__SWITCH__) guards throughout opengl.cpp) rather than linked here.
    # deko3d migration P1 (AI-assisted): libdeko3d ships in libnx/lib of the devkita64 image.
    target_link_libraries(runtime common fmt SDL3::SDL3 sound stb_image libco deko3d nx)

    # deko3d migration P1.3 (AI-assisted): GLSL -> .dksh (UAM) -> embedded headers.
    include(${CMAKE_SOURCE_DIR}/cmake/Deko3dShaders.cmake)
    set(DK_SHADER_GEN ${CMAKE_CURRENT_BINARY_DIR}/dksh_gen/dksh)
    dk_add_shaders(DK_SHADER_HEADERS ${DK_SHADER_GEN}
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/splash.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/splash.frag)
    add_custom_target(dk_shaders DEPENDS ${DK_SHADER_HEADERS})
    add_dependencies(runtime dk_shaders)
    target_include_directories(runtime PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/dksh_gen)

    # deko3d migration P3.0 (AI-assisted, T25): tfrag3 .glsl -> .dksh via the
    # gen-dksh.py UAM vehicle, then embedded the same way as the splash shaders.
    # The .glsl inputs (shaders/tfrag3.*.glsl) are the *already-preprocessed* spike
    # artifacts; gen-dksh.py's HEIGHT_SCALE/SCISSOR_* / #version swaps are therefore a
    # no-op on them, so uam reproduces docs/deko3d/uam-spike/tfrag3.{vert,frag}.dksh
    # byte-for-byte (checked via the gen-dksh.py md5 gate). Inert for now: the
    # embedded headers are generated but included by nothing yet (P3.2 draws tfrag3).
    # Switch-only (the whole block is inside `if(NINTENDO_SWITCH)`); host builds do
    # not invoke python3 / uam.
    set(_TF3_GEN ${CMAKE_CURRENT_BINARY_DIR}/dksh_gen/dksh)
    file(MAKE_DIRECTORY ${_TF3_GEN})
    set(_TF3_PY ${CMAKE_SOURCE_DIR}/scripts/gen-dksh.py)
    set(_TF3_EMBED ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake)

    add_custom_command(
        OUTPUT ${_TF3_GEN}/tfrag3.vert.dksh ${_TF3_GEN}/tfrag3_vert_dksh.h
        COMMAND python3 ${_TF3_PY}
                --input ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/tfrag3.vert.glsl
                --stage vert --output ${_TF3_GEN}/tfrag3.vert.dksh
        COMMAND ${CMAKE_COMMAND} -DIN=${_TF3_GEN}/tfrag3.vert.dksh
                -DOUT=${_TF3_GEN}/tfrag3_vert_dksh.h -DNAME=tfrag3_vert_dksh
                -P ${_TF3_EMBED}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/tfrag3.vert.glsl
                ${_TF3_PY} ${_TF3_EMBED}
        COMMENT "uam tfrag3.vert (T25, AI-assisted)"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${_TF3_GEN}/tfrag3.frag.dksh ${_TF3_GEN}/tfrag3_frag_dksh.h
        COMMAND python3 ${_TF3_PY}
                --input ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/tfrag3.frag.glsl
                --stage frag --output ${_TF3_GEN}/tfrag3.frag.dksh
        COMMAND ${CMAKE_COMMAND} -DIN=${_TF3_GEN}/tfrag3.frag.dksh
                -DOUT=${_TF3_GEN}/tfrag3_frag_dksh.h -DNAME=tfrag3_frag_dksh
                -P ${_TF3_EMBED}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/tfrag3.frag.glsl
                ${_TF3_PY} ${_TF3_EMBED}
        COMMENT "uam tfrag3.frag (T25, AI-assisted)"
        VERBATIM
    )

    add_custom_target(dk_tfrag3 DEPENDS
        ${_TF3_GEN}/tfrag3.vert.dksh ${_TF3_GEN}/tfrag3_vert_dksh.h
        ${_TF3_GEN}/tfrag3.frag.dksh ${_TF3_GEN}/tfrag3_frag_dksh.h)
    # Wire like dk_state.cpp:101: build the embedded shader into the Switch-only
    # runtime (the headers live in dksh_gen, already on the runtime include path).
    add_dependencies(runtime dk_tfrag3)

    # PG.2 (AI-assisted, T44): jak1 background shaders for P3.3 (Tie3/Shrub).
    # Same uam + embed rule as tfrag3 above. Inert: headers included by nothing yet.
    set(_BG_SHADERS shrub.vert shrub.frag tfrag3_no_tex.vert tfrag3_no_tex.frag
                    tie_wind.vert tie_wind.frag
                    etie.vert etie.frag etie_base.vert etie_base.frag
                    merc2.vert merc2.frag
                    sprite3.vert sprite3.frag
                    direct.vert direct.frag)
    set(_BG_OUTS "")
    foreach(_sh ${_BG_SHADERS})
        string(REPLACE "." "_" _nm ${_sh})
        string(REGEX MATCH "[a-z]+$" _stage ${_sh})
        add_custom_command(
            OUTPUT ${_TF3_GEN}/${_sh}.dksh ${_TF3_GEN}/${_nm}_dksh.h
            COMMAND python3 ${_TF3_PY}
                    --input ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/${_sh}.glsl
                    --stage ${_stage} --output ${_TF3_GEN}/${_sh}.dksh
            COMMAND ${CMAKE_COMMAND} -DIN=${_TF3_GEN}/${_sh}.dksh
                    -DOUT=${_TF3_GEN}/${_nm}_dksh.h -DNAME=${_nm}_dksh
                    -P ${_TF3_EMBED}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/${_sh}.glsl
                    ${_TF3_PY} ${_TF3_EMBED}
            COMMENT "uam ${_sh} (T44, AI-assisted)"
            VERBATIM)
        list(APPEND _BG_OUTS ${_TF3_GEN}/${_sh}.dksh ${_TF3_GEN}/${_nm}_dksh.h)
    endforeach()
    add_custom_target(dk_bg_shaders DEPENDS ${_BG_OUTS})
    add_dependencies(runtime dk_bg_shaders)
else()
    target_link_libraries(runtime common fmt SDL3::SDL3 imgui discord-rpc sound stb_image libco libcurl)
endif()
if(WIN32)
    target_link_libraries(runtime mman)
elseif(NOT NINTENDO_SWITCH)
    target_link_libraries(runtime pthread dl)
endif()

add_executable(gk main.cpp)
target_link_libraries(gk runtime)
if(WIN32)
    set_target_properties(gk PROPERTIES VS_DPI_AWARE "PerMonitor")
endif()

if(NINTENDO_SWITCH)
    # FIX 7o -- link-time interposition of the two functions that kill us.
    #
    # The 7n fatal_report proved the death is fatalThrow(0x1159), raised by Mesa's
    # egl_switch.c when nwindowQueueBuffer() fails. fatalThrow gives Atmosphere no register
    # context (its report is all zeros), so --wrap is the only way to learn *which* of the
    # three egl_switch.c call sites fired and *why* nwindow rejected the buffer.
    target_link_options(gk PRIVATE
        "-Wl,--wrap=fatalThrow"
        "-Wl,--wrap=nwindowQueueBuffer"
        # FIX 7q -- threadExit is where the 0x1159 abort actually lives (verified by
        # disassembling libnx's thread.o: cbz ThreadVars.thread_ptr -> diagAbortWithResult).
        "-Wl,--wrap=threadExit"
        # FIX 7r -- close the last two bypasses. --wrap only rewrites *undefined* refs, so
        # anything calling fatalThrowWithPolicy() directly skipped __wrap_fatalThrow, and
        # any svcBreak() not reached through our diagAbortWithResult override was invisible.
        "-Wl,--wrap=fatalThrowWithPolicy"
        "-Wl,--wrap=svcBreak")

    # Package the gk ELF as a homebrew NRO. The toolchain file sets
    # CMAKE_FIND_ROOT_PATH_MODE_PROGRAM to NEVER (correct for cross builds), so locate the
    # host-side devkitPro tools by absolute path instead of relying on the search modes.
    find_program(ELF2NRO_TOOL elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH)
    find_program(NACPTOOL_TOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH)

    # FIX 47 (AI-assisted): per-game NRO identity. Every NRO used to be called
    # "OpenGOAL" with the same gk-icon.jpg, so the three games were
    # indistinguishable on the Switch home menu. SWITCH_GAME (set by the Taskfile
    # for each build directory) now selects both the displayed title and the icon.
    # FIX 51 (AI-assisted): use the games' own artwork for the home-menu icon.
    # The .png files are derived from the extracted game assets
    # (data/game/assets/jakN/app*.png): jak1 ships a true 256x256 app256.png and
    # is copied losslessly; jak2/jak3 only ship 64x64 app64.png and are upscaled
    # 4x to the 256x256 that the home menu expects. The older .jpg files are kept
    # next to them so a missing .png degrades to the previous icon, not to a
    # build failure.
    if(SWITCH_GAME STREQUAL "jak2")
        set(GK_NRO_TITLE "Jak II")
        set(GK_NRO_ICON "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon-jak2.png")
        set(GK_NRO_ICON_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon-jak2.jpg")
    elseif(SWITCH_GAME STREQUAL "jak3")
        set(GK_NRO_TITLE "Jak 3")
        set(GK_NRO_ICON "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon-jak3.png")
        set(GK_NRO_ICON_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon-jak3.jpg")
    else()
        set(GK_NRO_TITLE "Jak and Daxter")
        set(GK_NRO_ICON "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon-jak1.png")
        set(GK_NRO_ICON_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon-jak1.jpg")
    endif()

    if(NOT EXISTS "${GK_NRO_ICON}" AND EXISTS "${GK_NRO_ICON_FALLBACK}")
        message(WARNING "Icon ${GK_NRO_ICON} missing, falling back to ${GK_NRO_ICON_FALLBACK}")
        set(GK_NRO_ICON "${GK_NRO_ICON_FALLBACK}")
    endif()
    # Port credit (AI-assisted): the hbmenu "author" shown under the game title is
    # baked into the NRO's NACP section -- changing it means rebuilding the NRO.
    # OpenGOAL's own credit stays in CREDITS.md / the repository README.
    set(GK_NRO_AUTHOR "fildicio")
    set(GK_NRO_VERSION "1.0.0")

    if(NOT EXISTS "${GK_NRO_ICON}")
        # Never silently fall back to a mismatched icon; the legacy shared icon is
        # the only reasonable substitute and it is obvious in the build log.
        message(WARNING "Per-game icon ${GK_NRO_ICON} missing, using gk-icon.jpg")
        set(GK_NRO_ICON "${CMAKE_CURRENT_SOURCE_DIR}/switch/gk-icon.jpg")
    endif()
    message(STATUS "Switch NRO title: ${GK_NRO_TITLE}, icon: ${GK_NRO_ICON}")

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gk.nacp"
        COMMAND ${NACPTOOL_TOOL} --create "${GK_NRO_TITLE}" "${GK_NRO_AUTHOR}" "${GK_NRO_VERSION}"
                "${CMAKE_CURRENT_BINARY_DIR}/gk.nacp"
        VERBATIM)

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gk.nro"
        COMMAND ${ELF2NRO_TOOL} "$<TARGET_FILE:gk>" "${CMAKE_CURRENT_BINARY_DIR}/gk.nro"
                "--nacp=${CMAKE_CURRENT_BINARY_DIR}/gk.nacp"
                "--icon=${GK_NRO_ICON}"
        DEPENDS gk "${CMAKE_CURRENT_BINARY_DIR}/gk.nacp" "${GK_NRO_ICON}"
        VERBATIM)

    add_custom_target(gk_nro ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gk.nro")
endif()
