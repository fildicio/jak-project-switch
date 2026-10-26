# T105 — dkgl textures/FBO + programs/uniforms + TexturePool seam

**Depends on:** T103, T104b. **Category:** D dkgl (brief item 2, third quarter).
**Size:** medium (1 session). **Law:** zero design freedom — replace the named stubs only;
every dktexgpu/dkdev call below already exists in-tree (cite kept). Anything else → STOP.

## Goal

Two new implementation files + one 6-line seam:

1. **dkgl textures/FBO** — `glGenTextures` objects are dktexgpu render targets created
   lazily; FBO 0 = the dk frame target.
2. **dkgl programs** — program identity from the brief-sanctioned name hint; uniforms
   write a CPU shadow sized by the T103 tables.
3. **TexturePool seam** — so the ocean's 128×128 mip texture (tbp 8160) resolves for
   NATIVE renderers too (they sample it via `slot.gpu_texture`).

## Create 1 — `game/graphics/deko3d_renderer/dkgl/dkgl_texture.cpp` (+ small decl block in `dkgl_state.h`)

Texture table (kMaxTextures): `{live, w, h, levels, min_filter, mag_filter, wrap_s,
wrap_t, dk_tex_id, was_rendered_to, barrier_done}`. GL texture ids: `kTexIdBase + 1..`.

| entry | behavior |
|---|---|
| `gen_textures` | ids from `kTexIdBase+1`, defaults: min/mag `LINEAR` (0x2601), wrap `CLAMP_TO_EDGE` (0x812F) — same defaults `upload_to_gpu` sets (TexturePool.cpp:92-95) |
| `bind_texture(name)` | `name >= kTexIdBase` → record on the CURRENT unit (`active_unit()`); `name` is a dktexgpu id (from the pool path) → record that instead; `0` → unbind. No GL work |
| `tex_parameteri(p,v)` | store into the texture bound on the current unit (MIN/MAG FILTER, WRAP_S/T; anything else → counter, still store) |
| `tex_image_2d(fmt,w,h,fmt2,type,data)` | level bookkeeping: first call fixes w/h, each call `levels++` (the mip chain is 8× `glTexImage2D` in `FramebufferTexturePair`, opengl_utils.cpp). Accept `RGBA`/`RGBA8` + `UNSIGNED_INT_8_8_8_8_REV`/`UNSIGNED_BYTE`; else counter + keep recording. `data != nullptr` → counter + one-time log `[dkgl] tex_image_2d with data unsupported (ocean never does this)` and still return true (pixels ignored — pixel uploads are NOT in the ocean surface; the census stub would have logged it if the ocean did) |
| `gen/bind/delete_framebuffers` | id table (kMaxFbos); `bind_framebuffer(0)` → `emitter->bind_frame_target()` + mark the PREVIOUS offscreen FBO's texture `was_rendered_to = true` (barrier data for T106); `bind_framebuffer(n)` → `emitter->bind_offscreen_target(tex_id)` for its attached texture |
| `framebuffer_texture_2d(target,att,tex,level)` | attach `tex` to the bound FBO (record `attached_tex`); **materialize**: if `tex.dk_tex_id == 0` → `dktexgpu::create_render_target_mipped(w, h, levels)` (dk_texture_gpu.h:112 — same call `dkocean` uses for the same 128×128×8 target, dk_ocean_driver.cpp). levels = recorded `levels` (min 1). Unknown w/h (no tex_image_2d seen) → 1×1, levels 1, counter. Return false only if `create_render_target_mipped` returns 0 |
| `draw_buffers(n,bufs)` | accept exactly `{COLOR_ATTACHMENT0}`; anything else → counter |
| `check_framebuffer_status` | return `FRAMEBUFFER_COMPLETE` |

Add `u32 resolve_texture_id(u32 gl_id);` to `dkgl_state.h`: `gl_id >= kTexIdBase` →
materialize on demand (same rule as above) and return the dktexgpu id; else return
`gl_id` unchanged. This is what the pool seam calls.

## Edit 2 — `game/graphics/texture/TexturePool.cpp` (the seam, 6 lines)

At the TOP of `TexturePool::give_texture` (line 100), before anything else:

```cpp
#if defined(__SWITCH__)
  // T105 (AI-assisted): dkgl seam. A TextureInput can carry a dkgl-created texture id
  // (kDkglTexBase | n, e.g. the ocean's FramebufferTexturePair result). Resolve it to the
  // real dktexgpu id so tbp slots (ocean 8160) resolve for native renderers as well.
  // Resolved ids are < kDkglTexBase, so this recurses at most once.
  if (in.gpu_texture >= 0x4000'0000u) {
    TextureInput fixed = in;
    fixed.gpu_texture = dkgl::resolve_texture_id(in.gpu_texture);
    return give_texture(fixed);
  }
#endif
```

with the `#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"` added under a
matching `#if defined(__SWITCH__)` include guard at the top of the file (copy how the file
already includes dk headers for the T23 seam — grep `dktexgpu` in TexturePool.cpp; if it
includes unconditionally, do the same).

Rationale (cite in the comment): `OceanTexture::init_textures` passes
`in.gpu_texture = m_result_texture.texture()` — a `GLuint` (TexturePool.h:210,
opengl_utils.h:18) — into `give_texture_and_load_to_vram(..., 8160)`, which stores it in
`slot.gpu_texture` (TexturePool.cpp:150/154). Native dk renderers resolve that through
`dktexgpu::slot_of`, so the pseudo id must become the real one before storage.

## Create 3 — `game/graphics/deko3d_renderer/dkgl/dkgl_program.cpp`

Shader table (128): `{stage, name[64]}`. Program table (128):
`{name[64], const ProgramUniformTable* table /*null = inert*/, u32 vsh, u32 fsh,
u8 uniform_shadow[256], bool uniform_dirty, bool ever_used}`.

| entry | behavior |
|---|---|
| `hint_program_name(name)` | copy into `g_pending_hint` (the one-line hook lands in T107; unit tests call this directly) |
| `create_shader(stage)` | id 1..; record `{stage, g_pending_hint}` |
| `shader_source/compile_shader` | no-op (source not needed — identity comes from the hint) |
| `create_program` | id 1.. |
| `attach_shader` | record vsh/fsh into the program |
| `link_program` | `name = vert shader's hint name`; linear-search `kProgramUniformTables` (T103 .inc, include it) by `program_name`; hit → `table = &entry`; miss → `table = nullptr` + one-time log `[dkgl] program '<name>' not in table (inert)`. Zero the uniform_shadow, `uniform_dirty = true` |
| `use_program` | set bound program |
| `get_shaderiv/get_programiv` | `*params = 1` (GL_TRUE) — the GL `Shader` ctor only reads COMPILE/LINK status |
| `get_shader_info_log` | `log[0] = '\0'; if (len) *len = 0;` |
| `get_uniform_location(prog,name)` | inert → -1. `name` matches `tex_T<0-7>` → return `0x8000'0000 \| <digit>` (sampler pseudo-location; unit == binding == digit). Else search the program's `table->vert` then `table->frag` slots by name → return the index into a COMBINED slot array (vert first, then frag; build the combined view on the fly from the two tables). Miss → -1 |
| `uniform1i/1f/4f` | `-1` or sampler pseudo-loc → no-op (+first-use log for samplers: `[dkgl] sampler uniform ignored (binding is explicit)`). Else resolve location → slot: write 4/4/16 bytes into `uniform_shadow` — **combined layout**: vert slots at their table `offset`, frag slots at `vert.block_size + offset` (one 256-byte shadow backs both stage blocks; T106 uploads it as ONE UBO slice, vert at 0, frag at `vert.block_size`). For EVERY stage whose slots contain that NAME (GL exposes one location per name; if `bucket` is in both stages, one `uniform1i` must update both copies — iterate both slot arrays and write all matches). Set `uniform_dirty` |
| `get_uniform_block_index` | return -1 |
| `uniform_block_binding` | no-op |
| `get_error` | return 0 |

Expose for T106/T107/T108 (add to `dkgl_state.h`):
```cpp
const ProgramUniformTable* program_table(u32 program);  // null = inert
const u8* program_uniform_shadow(u32 program);           // 256 bytes
bool program_uniform_dirty(u32 program);
void program_uniform_clear_dirty(u32 program);
u32 materialized_dk_tex(u32 gl_id);                      // 0 = none
bool texture_needs_barrier(u32 gl_id);                   // was_rendered_to && !barrier_done
void texture_barrier_sent(u32 gl_id);
```

## Edit 4 — `dkgl_glad.cpp`: add the texture/FBO/program overrides

Extend the install list (T104b style, same census pattern) with:
`glad_glGenTextures, glad_glDeleteTextures, glad_glActiveTexture, glad_glBindTexture,
glad_glTexParameteri, glad_glTexImage2D, glad_glGenFramebuffers, glad_glDeleteFramebuffers,
glad_glBindFramebuffer, glad_glFramebufferTexture2D, glad_glDrawBuffers,
glad_glCheckFramebufferStatus, glad_glCreateShader, glad_glShaderSource,
glad_glCompileShader, glad_glCreateProgram, glad_glAttachShader, glad_glLinkProgram,
glad_glDeleteShader, glad_glUseProgram, glad_glGetShaderiv, glad_glGetProgramiv,
glad_glGetShaderInfoLog, glad_glGetUniformLocation, glad_glUniform1i, glad_glUniform1f,
glad_glUniform4f, glad_glGetUniformBlockIndex, glad_glUniformBlockBinding, glad_glGetError`
(+ `glad_glUniform1ui` is NOT wired — Generic2 only, later). Update the install log count.
`glGetUniformLocation` wrapper passes the `const GLchar*` through; `glShaderSource`
ignores length (`nullptr`) and stores nothing.

## Create 5 — host tests (`test/test_dkgl_state.cpp` extend or new `test_dkgl_texture_program.cpp`)

With a MockEmitter (T104b) installed:
1. hint identity: `hint_program_name("ocean_common")` → create vsh+fsh → attach → link →
   `program_table()` != null and `program_name == "ocean_common"`; name "solid_color" →
   inert (null table).
2. uniforms: `get_uniform_location(p, "bucket")` >= 0; `uniform1f` then
   `program_uniform_shadow(p)` bytes at the table offset changed; if a name exists in both
   stages, BOTH offsets changed; `get_uniform_location(p,"tex_T1")` returns the sampler
   pseudo-loc and `uniform1i` on it is a no-op (shadow unchanged).
3. RT: gen_texture (id `kTexIdBase+1`) → 8× `tex_image_2d` (128..1) → `framebuffer_texture_2d`
   → mock `bind_offscreen_target` NOT called yet; `bind_framebuffer(id)` → called once with
   the pseudo id; `resolve_texture_id(kTexIdBase+1)` returns the mock's materialized id
   (MockEmitter records; for host tests stub `create_render_target_mipped` — see below);
   second resolve → same id, one creation.
   **Host stubbing:** `create_render_target_mipped` is dk-only. Guard its call with the
   existing dktexgpu activity check pattern (`dkdev::is_up() && dktexgpu::active()` —
   TexturePool.cpp:70): on host, materialize to the fake id `kTexIdBase + 0x1000 + n`
   instead (same branch style; cite the T23 seam).
4. pool seam: call `dkgl::resolve_texture_id` for a non-dkgl id → passthrough.

## Checks

- Host build + tests PASS.
- `python3 scripts/ai/check-dk.py` → PASS (new dk names used:
  `dktexgpu::create_render_target_mipped`, `dkdev::is_up`, `dktexgpu::active` — all
  already in-tree).
- `git diff game/graphics/texture/TexturePool.cpp` must show ONLY the seam block + include.

## Report

Report: diffs, gtest tail, check-dk output, STOP conditions (especially any
`gl[A-Z]` hit in `opengl_utils.cpp`/`ocean/*.cpp` NOT covered by dkgl or the census — run
`grep -oE 'gl[A-Z][A-Za-z]*' game/graphics/opengl_renderer/opengl_utils.cpp game/graphics/opengl_renderer/ocean/*.cpp | sort -u` and include the full list in the report).
