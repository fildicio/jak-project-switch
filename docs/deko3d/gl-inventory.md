# GL inventory (deko3d migration P0.2) — generated, do not edit (AI-assisted)

Regenerate: `python3 scripts/gen-gl-inventory.py`

## Summary

- GL call sites: **2243** in **37** files under `game/graphics/`
- Distinct GL functions: **117**
- `glUniform*` call sites (→ UBO migration): **232**
- Shader sources: **94**, default uniforms to migrate: **309**, already using `binding`: **0**

## Per file (port checklist)

| File | Calls | Distinct |
|---|---:|---:|
| `game/graphics/opengl_renderer/TextureAnimator.cpp` | 289 | 38 |
| `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp` | 164 | 35 |
| `game/graphics/opengl_renderer/OpenGLRenderer.cpp` | 115 | 48 |
| `game/graphics/opengl_renderer/DirectRenderer.cpp` | 99 | 34 |
| `game/graphics/opengl_renderer/opengl_utils.cpp` | 91 | 27 |
| `game/graphics/opengl_renderer/DepthCue.cpp` | 89 | 27 |
| `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` | 88 | 29 |
| `game/graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp` | 81 | 24 |
| `game/graphics/opengl_renderer/foreground/Shadow2.cpp` | 80 | 27 |
| `game/graphics/opengl_renderer/sprite/Sprite3_Distort.cpp` | 78 | 22 |
| `game/graphics/opengl_renderer/foreground/Merc2.cpp` | 77 | 23 |
| `game/graphics/opengl_renderer/background/Tie3.cpp` | 75 | 26 |
| `game/graphics/opengl_renderer/DirectRenderer2.cpp` | 69 | 24 |
| `game/graphics/opengl_renderer/background/TFragment.cpp` | 68 | 30 |
| `game/graphics/opengl_renderer/loader/LoaderStages.cpp` | 68 | 21 |
| `game/graphics/opengl_renderer/ShadowRenderer.cpp` | 64 | 22 |
| `game/graphics/opengl_renderer/background/Hfrag.cpp` | 64 | 22 |
| `game/graphics/opengl_renderer/sprite/Sprite3.cpp` | 61 | 20 |
| `game/graphics/opengl_renderer/background/background_common.cpp` | 57 | 19 |
| `game/graphics/pipelines/opengl.cpp` | 54 | 39 |
| `game/graphics/opengl_renderer/ocean/OceanEnvmap.cpp` | 52 | 19 |
| `game/graphics/opengl_renderer/CollideMeshRenderer.cpp` | 50 | 26 |
| `game/graphics/opengl_renderer/background/Shrub.cpp` | 47 | 24 |
| `game/graphics/opengl_renderer/SkyBlendGPU.cpp` | 46 | 28 |
| `game/graphics/opengl_renderer/EyeRenderer.cpp` | 38 | 19 |
| `game/graphics/opengl_renderer/BlitDisplays.cpp` | 34 | 12 |
| `game/graphics/opengl_renderer/ocean/OceanTexture_PC.cpp` | 32 | 15 |
| `game/graphics/opengl_renderer/Shader.cpp` | 25 | 16 |
| `game/graphics/opengl_renderer/ocean/OceanTexture.cpp` | 24 | 14 |
| `game/graphics/opengl_renderer/SlowTimeEffect.cpp` | 15 | 11 |
| `game/graphics/texture/TexturePool.cpp` | 13 | 9 |
| `game/graphics/opengl_renderer/loader/Loader.cpp` | 12 | 6 |
| `game/graphics/opengl_renderer/loader/GpuBufferPool.h` | 9 | 5 |
| `game/graphics/opengl_renderer/SkyBlendCPU.cpp` | 8 | 4 |
| `game/graphics/opengl_renderer/Fbo.h` | 3 | 3 |
| `game/graphics/gfx_test.cpp` | 2 | 1 |
| `game/graphics/opengl_renderer/TextureAnimatorDefs.cpp` | 2 | 1 |

## Per function

| Function | Calls |
|---|---:|
| `glTexParameteri` | 174 |
| `glBindBuffer` | 162 |
| `glBindTexture` | 157 |
| `glBindVertexArray` | 119 |
| `glEnableVertexAttribArray` | 89 |
| `glBufferData` | 82 |
| `glEnable` | 74 |
| `glUniform1f` | 71 |
| `glVertexAttribPointer` | 70 |
| `glDisable` | 66 |
| `glBindFramebuffer` | 62 |
| `glUniform1i` | 60 |
| `glActiveTexture` | 55 |
| `glGenBuffers` | 52 |
| `glBlendEquation` | 47 |
| `glUniform4f` | 46 |
| `glTexImage2D` | 43 |
| `glDrawElements` | 43 |
| `glDepthMask` | 39 |
| `glDrawArrays` | 39 |
| `glBlendFuncSeparate` | 37 |
| `glGenVertexArrays` | 35 |
| `glDeleteBuffers` | 34 |
| `glGenTextures` | 32 |
| `glDepthFunc` | 30 |
| `glViewport` | 25 |
| `glDeleteVertexArrays` | 23 |
| `glColorMask` | 23 |
| `glPrimitiveRestartIndex` | 20 |
| `glVertexAttribIPointer` | 19 |
| `glDeleteTextures` | 19 |
| `glGetIntegerv` | 18 |
| `glClear` | 17 |
| `glClearColor` | 16 |
| `glFramebufferTexture2D` | 16 |
| `glBlendFunc` | 15 |
| `glGetTexLevelParameteriv` | 13 |
| `glUniform4fv` | 13 |
| `glGenerateMipmap` | 13 |
| `glPolygonMode` | 12 |
| `glUniformMatrix4fv` | 11 |
| `glFinish` | 11 |
| `glCheckFramebufferStatus` | 10 |
| `glGenFramebuffers` | 9 |
| `glBlendColor` | 8 |
| `glGetString` | 7 |
| `glDeleteShader` | 7 |
| `glMultiDrawElements` | 7 |
| `glGetError` | 7 |
| `glBlitFramebuffer` | 6 |
| `glStencilFunc` | 6 |
| `glStencilOp` | 6 |
| `glUniform4i` | 6 |
| `glSamplerParameteri` | 6 |
| `glClearDepthf` | 5 |
| `glUniform1ui` | 5 |
| `glDrawBuffers` | 5 |
| `glTexParameterf` | 5 |
| `glUniform3fv` | 5 |
| `glTexSubImage2D` | 5 |
| `glBufferSubData` | 4 |
| `glDeleteFramebuffers` | 4 |
| `glUniform2fv` | 4 |
| `glAttachShader` | 4 |
| `glUseProgram` | 4 |
| `glGetFloatv` | 4 |
| `glUniform2f` | 4 |
| `glClearStencil` | 3 |
| `glUniform1uiv` | 3 |
| `glDeleteSync` | 3 |
| `glColorMaski` | 3 |
| `glPixelStorei` | 3 |
| `glGetFramebufferAttachmentParameteriv` | 3 |
| `glCreateShader` | 3 |
| `glShaderSource` | 3 |
| `glCompileShader` | 3 |
| `glGetShaderiv` | 3 |
| `glGetShaderInfoLog` | 3 |
| `glBindSampler` | 3 |
| `glIsEnabled` | 3 |
| `glUniformBlockBinding` | 2 |
| `glGetUniformBlockIndex` | 2 |
| `glFenceSync` | 2 |
| `glClientWaitSync` | 2 |
| `glClearBufferfv` | 2 |
| `glGenRenderbuffers` | 2 |
| `glBindRenderbuffer` | 2 |
| `glRenderbufferStorage` | 2 |
| `glFramebufferRenderbuffer` | 2 |
| `glReadBuffer` | 2 |
| `glGetUniformLocation` | 2 |
| `glCreateProgram` | 2 |
| `glLinkProgram` | 2 |
| `glGetProgramiv` | 2 |
| `glGetProgramInfoLog` | 2 |
| `glStencilMask` | 2 |
| `glGetTexImage` | 2 |
| `glUniform3f` | 2 |
| `glBindBufferRange` | 2 |
| `glCompressedTexImage2D` | 2 |
| `glVertexAttribDivisor` | 2 |
| `glDeleteProgram` | 2 |
| `glBindBufferBase` | 1 |
| `glMapBufferRange` | 1 |
| `glUnmapBuffer` | 1 |
| `glDeleteRenderbuffers` | 1 |
| `glDebugMessageCallback` | 1 |
| `glDebugMessageControl` | 1 |
| `glTexImage2DMultisample` | 1 |
| `glRenderbufferStorageMultisample` | 1 |
| `glReadPixels` | 1 |
| `glGenSamplers` | 1 |
| `glSamplerParameterf` | 1 |
| `glIsTexture` | 1 |
| `glTexStorage2D` | 1 |
| `glCompressedTexSubImage2D` | 1 |
| `glDrawArraysInstanced` | 1 |

## Shaders

| File | #version | Default uniforms | binding? | Extensions |
|---|---|---:|---|---|
| `game/graphics/opengl_renderer/shaders/collision.frag` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/collision.vert` | 410 core | 16 | no |  |
| `game/graphics/opengl_renderer/shaders/debug_red.frag` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/debug_red.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/depth_cue.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/depth_cue.vert` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/direct2.frag` | 410 core | 13 | no |  |
| `game/graphics/opengl_renderer/shaders/direct2.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/direct_basic.frag` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/direct_basic.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/direct_basic_textured.frag` | 410 core | 11 | no |  |
| `game/graphics/opengl_renderer/shaders/direct_basic_textured.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/direct_basic_textured_multi_unit.frag` | 410 core | 18 | no |  |
| `game/graphics/opengl_renderer/shaders/direct_basic_textured_multi_unit.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/emerc.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/emerc.vert` | 410 core | 4 | no |  |
| `game/graphics/opengl_renderer/shaders/etie.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/etie.vert` | 410 core | 10 | no |  |
| `game/graphics/opengl_renderer/shaders/etie_base.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/etie_base.vert` | 410 core | 10 | no |  |
| `game/graphics/opengl_renderer/shaders/eye.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/eye.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/generic.frag` | 410 core | 6 | no |  |
| `game/graphics/opengl_renderer/shaders/generic.vert` | 410 core | 9 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_depth_copy.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_depth_copy.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_draw.frag` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_draw.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe.frag` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_downsample.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_downsample.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_on_grid.frag` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_on_grid.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_read.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_read.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_read_debug.frag` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/glow_probe_read_debug.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/hfrag.frag` | 410 core | 3 | no |  |
| `game/graphics/opengl_renderer/shaders/hfrag.vert` | 410 core | 8 | no |  |
| `game/graphics/opengl_renderer/shaders/hfrag_montage.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/hfrag_montage.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/merc2.frag` | 410 core | 7 | no |  |
| `game/graphics/opengl_renderer/shaders/merc2.vert` | 410 core | 10 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_common.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_common.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_envmap.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_envmap.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_envmap_haze.frag` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_envmap_haze.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_texture.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_texture.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_texture_mipmap.frag` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/ocean_texture_mipmap.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/plain_texture.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/plain_texture.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/post_processing.frag` | 410 core | 3 | no |  |
| `game/graphics/opengl_renderer/shaders/post_processing.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/post_processing_fsr.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/post_processing_fsr.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/shadow.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/shadow.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/shadow2.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/shadow2.vert` | 410 core | 7 | no |  |
| `game/graphics/opengl_renderer/shaders/shrub.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/shrub.vert` | 410 core | 9 | no |  |
| `game/graphics/opengl_renderer/shaders/simple_texture.frag` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/simple_texture.vert` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/sky.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/sky.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/sky_blend.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/sky_blend.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/slow_time.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/slow_time.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/solid_color.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/solid_color.vert` | 410 core | 0 | no |  |
| `game/graphics/opengl_renderer/shaders/splash.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/splash.vert` | 410 core | 2 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite3_3d.frag` | 410 core | 3 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite3_3d.vert` | 410 core | 17 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite_3d.frag` | 410 core | 10 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite_3d.vert` | 410 core | 19 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite_distort.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite_distort.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite_distort_instanced.frag` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/sprite_distort_instanced.vert` | 410 core | 1 | no |  |
| `game/graphics/opengl_renderer/shaders/tex_anim.frag` | 410 core | 11 | no |  |
| `game/graphics/opengl_renderer/shaders/tex_anim.vert` | 410 core | 3 | no |  |
| `game/graphics/opengl_renderer/shaders/tfrag3.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/tfrag3.vert` | 410 core | 9 | no |  |
| `game/graphics/opengl_renderer/shaders/tfrag3_no_tex.frag` | 410 core | 3 | no |  |
| `game/graphics/opengl_renderer/shaders/tfrag3_no_tex.vert` | 410 core | 3 | no |  |
| `game/graphics/opengl_renderer/shaders/tie_wind.frag` | 410 core | 5 | no |  |
| `game/graphics/opengl_renderer/shaders/tie_wind.vert` | 410 core | 7 | no |  |
