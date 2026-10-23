# Ocean renderer survey — for the deko3d port (AI-assisted)

Report-only facts gathered for the deko3d ocean port design. No design, no opinions,
no code changes. "UNCLEAR: …" marks anything that could not be determined from the files.
Scope: `game/graphics/opengl_renderer/ocean/` (12 files, 12592 LOC).

## 1. File inventory

| File | LOC | Main class(es) | What it does |
|---|---|---|---|
| OceanMidAndFar.h | 38 | OceanMidAndFar | Handler for the first ocean bucket; wraps DirectRenderer + OceanEnvmap + OceanTexture + OceanMid. |
| OceanMidAndFar.cpp | 203 | OceanMidAndFar | Sequences ocean-texture -> far -> mid for jak1/jak2; handles `handle_ocean_far` (GIF draws) and `handle_ocean_mid` (dispatches to OceanMid). |
| OceanNear.h | 136 | OceanNear | Handler for the near ocean bucket (BucketId 63); owns a VU1 data buffer + CommonOceanRenderer + OceanTexture. |
| OceanNear.cpp | 223 | OceanNear | Render loop for near ocean: texture gen, then VIF STCYCL/UNPACK + MSCALF/STMOD dispatch to VU1 calls, then flush. |
| OceanNear_PS2.cpp | 4232 | OceanNear (VU1) | Pure VU1 emulation for the near ocean: `run_call*`/`run_L*` functions (jak1 + jak2 variants). No GL calls. |
| OceanMid.h | 192 | OceanMid | OceanMid VU1 host class; owns 1024 Vf buffer, m_vu_data, CommonOceanRenderer, Constants struct (0x240). |
| OceanMid.cpp | 263 | OceanMid | Render driver for mid ocean: base/offset, constants, call0, then VIF dispatch to VU1 calls; `xgkick` -> kick_from_mid. |
| OceanMid_PS2.cpp | 4991 | OceanMid (VU1) | Pure VU1 emulation for the mid ocean: `run_call*`/`run_L*` functions (jak1 + jak2). No GL calls. |
| CommonOceanRenderer.h | 54 | CommonOceanRenderer | Shared GL vertex/index accumulation used by both near and mid: 3 buckets (RGB_TEXTURE, ALPHA, ENV_MAP). |
| CommonOceanRenderer.cpp | 542 | CommonOceanRenderer | Decodes GIF vertex/adgif data into vertices; `flush_near`/`flush_mid` issue the GL draw (2 glDrawElements, bucket 3 & 4). |
| OceanEnvmap.h | 40 | OceanEnvmap | Generates the ocean envmap texture via sky + time-of-day (ocean-method-89); offscreen 64x64 FBO + haze/radial passes. |
| OceanEnvmap.cpp | 314 | OceanEnvmap | `handle_ocean_envmap_jak2`: scans DMA for scissor/frame, renders offscreen, render_haze + render_envmap, moves result to VRAM. |
| OceanTexture.h | 161 | OceanTexture | Builds the animated ocean surface texture (128x128, up to 8 mips) using VU1 (jak1) or PC path. |
| OceanTexture.cpp | 467 | OceanTexture | `handle_ocean_texture_jak1/jak2`: reads VIF data into VU1 buffers + `make_texture_with_mipmaps` GL draw. |
| OceanTexture_PC.cpp | 736 | OceanTexture | PC (jak2/3) path `run_L*_PC`/`flush`: builds vertex arrays and issues the 2 ocean-texture GL draws. |

Total ocean LOC: ~12592. The 2 PS2 .cpp files (9223 LOC combined) are pure CPU VU1 emulation with ZERO GL calls.

## 2. Bucket wiring

Both ocean renderers are `BucketRenderer` subclasses registered in `OpenGLRenderer.cpp`.

| Renderer class | BucketId | Name | Registration sites (OpenGLRenderer.cpp) | jak version |
|---|---|---|---|---|
| OceanMidAndFar | OCEAN_MID_FAR / OCEAN_MID_AND_FAR | "ocean-mid-far" | lines 527, 800, 1020 (jak1=4: OCEAN_MID_AND_FAR) | jak1, jak2, jak3, jakX |
| OceanNear | OCEAN_NEAR | "ocean-near" | lines 687, 946, 1228 (comment "// 63") | jak1, jak2, jak3, jakX |

- jak1 register ocean-mid-far as **bucket 4 (OCEAN_MID_AND_FAR)** — see the `// 4 : OCEAN_MID_AND_FAR` comment at line 1018.
- jak1 register ocean-near as **bucket 63 (OCEAN_NEAR)** — see `// 63` comment at line 1228. (jak2 uses a different id near line 946/687; UNCLEAR: exact jak2 bucket number for ocean-mid-far not confirmed from the grep alone.)
- Each renderer has `render_jak1` vs `render_jak2` (jak2 covers jak2/jak3/jakX in the switch).

### Render order (draw order, from the render() methods)

OceanMidAndFar (jak1) — `OceanMidAndFar.cpp:52-93`:
1. `m_texture_renderer.handle_ocean_texture_jak1` (animated ocean texture; renders OFFSCREEN into a 128x128 texture, not the main framebuffer).
2. `handle_ocean_far` — direct GIF draws (far ocean / horizon).
3. `m_direct.flush_pending` then `set_mipmap(true)`.
4. `handle_ocean_mid` -> `m_mid_renderer.run` (VU1 mid ocean) -> CommonOceanRenderer accumulates, then flush.
5. trailing DMA reads + final flush + `set_mipmap(false)`.

OceanMidAndFar (jak2) — `OceanMidAndFar.cpp:95-152` (adds envmap):
1. `m_envmap_renderer.handle_ocean_envmap_jak2` (envmap + haze OFFSCREEN).
2. `m_texture_renderer.handle_ocean_texture_jak2`.
3. `handle_ocean_far` + flush.
4. `set_mipmap(true)` then `handle_ocean_mid` -> `m_mid_renderer.run_jak2`.
5. trailing reads + flush + `set_mipmap(false)`.

OceanNear (jak1 & jak2 identical apart from call names) — `OceanNear.cpp:52-219`:
1. `m_texture_renderer.handle_ocean_texture_jak1/jak2` (a second ocean-texture gen — comment in code notes it "looks the same" and asks why it is repeated; UNCLEAR: not documented).
2. `m_common_ocean_renderer.init_for_near()`, then a 32-byte setup DMA, then BASE/OFFSET DMA.
3. Main loop: VIF STCYCL 0x404 + UNPACK_V4_32 -> memcpy into `m_vu_data`; MSCALF + STMOD -> `run_call0`/`run_call39` (jak1) or `*_jak2`.
4. Trailing drain + `m_common_ocean_renderer.flush_near`.

UNCLEAR: the relative order of OceanMidAndFar vs OceanNear across the whole frame (which bucket the GOAL scheduler runs first) is not visible in these files; it is driven by the GOAL bucket order, not C++. 

## 3. GL call census (counts only)

Counts gathered from a grep census per file (draw / tex-upload / read-back / uniform / buffer-data / FBO).

| File | glDraw | tex upload | read-back | uniform | buffer-data | FBO | PS2/VU1? |
|---|---|---|---|---|---|---|---|
| CommonOceanRenderer.cpp | 2 | 4 | 0 | 10 | 6 | 0 | no (GL) |
| OceanEnvmap.cpp | 2 | 4 | 0 | 0 | 2 | 0 | no (GL) |
| OceanMid.cpp | 0 | 0 | 0 | 0 | 0 | 0 | driver only |
| OceanMidAndFar.cpp | 0 | 0 | 0 | 0 | 0 | 0 | sequencing only |
| OceanNear.cpp | 0 | 0 | 0 | 0 | 0 | 0 | driver only |
| OceanNear_PS2.cpp | 0 | 0 | 0 | 0 | 0 | 0 | pure VU1 |
| OceanMid_PS2.cpp | 0 | 0 | 0 | 0 | 0 | 0 | pure VU1 |
| OceanTexture.cpp | 1 | 1 | 0 | 4 | 1 | 0 | GL (jak1 mipmap) |
| OceanTexture_PC.cpp | 2 | 1 | 0 | 1 | 4 | 0 | GL (jak2/3 PC path) |

### Distinct read-back calls (glGet*/glReadPixels/glMapBuffer)

**NONE.** No ocean file contains any `glGet…`, `glReadPixels`, or `glMapBuffer` call (census read-back column = 0 for every file).

This is the key fact for the deko3d port: the ocean code never reads pixels back from the GPU. All OFFSCREEN results (ocean envmap FBO, ocean-texture FBO) are kept on the GPU as textures and sampled in later passes. There is no GPU->CPU read path to translate. (UNCLEAR: none detected; if a read-back exists elsewhere in the engine it would not be in these files.)

## 4. Shaders used by the ocean

5 shader pairs, all in `game/graphics/opengl_renderer/shaders/ocean_*.{vert,frag}`. ShaderId enum values from `Shader.h`.

| Shader file | ShaderId (Shader.h) | Used by | Vertex attributes | Uniforms (frag) |
|---|---|---|---|---|
| ocean_common.{vert,frag} | OCEAN_COMMON = 15 | CommonOceanRenderer flush (draws the accumulated near+mid geometry) | loc0 position_in vec3; loc1 rgba_in vec4; loc2 tex_coord_in vec3; loc3 fog_in uint | color_mult, alpha_mult, fog_color vec4, bucket int, sampler2D tex_T0 |
| ocean_texture.{vert,frag} | OCEAN_TEXTURE = 13 | OceanTexture_PC flush (jak2/3 ocean surface draw) | loc0 position_in vec2; loc1 rgba_in vec4; loc2 tex_coord_in vec2 | sampler2D tex_T0 |
| ocean_texture_mipmap.{vert,frag} | OCEAN_TEXTURE_MIPMAP = 14 | OceanTexture::make_texture_with_mipmaps | loc0 position_in vec2; loc1 tex_coord_in vec2 | alpha_intensity float, sampler2D tex_T0 |
| ocean_envmap.{vert,frag} | OCEAN_ENVMAP = 43 | OceanEnvmap::render_envmap (final radial envmap pass) | loc0 position_in vec2 | sampler2D tex_T1 |
| ocean_envmap_haze.{vert,frag} | OCEAN_ENVMAP_HAZE = 44 | OceanEnvmap::render_haze | loc0 ndc_in vec2; loc1 color_in vec4 | (none; color passed via varying) |

Notes for the deko3d port (facts, not opinions):
- `ocean_common.vert` uses 4 attribute locations; loc3 is an **integer** `uint fog_in` (`glVertexAttribIPointer` in CommonOceanRenderer.cpp:61-67). In deko3d that is a `DkVtxAttribType_Uint`/`Uint` attribute, not a float.
- `ocean_common.frag` selects behaviour with `uniform int bucket` (3 = ocean-texture pass, 4 = envmap pass per the flush code) and mixes `color_mult`/`alpha_mult`/`fog_color`. No loose uniforms exist in the .frag already (all are declared), so the UBO conversion is straight forward — but the deko3d pipeline forbids loose uniforms, so these must move into a std140 block.
- `ocean_texture.frag` has only `sampler2D tex_T0` — no other uniforms.
- No ocean shader declares a UBO yet; all uniforms are loose `uniform` declarations (the desktop GL path). deko3d requires `layout(binding = N)` + std140 — this is the main shader-porting delta.

## 5. VU1 emulation (the two *_PS2.cpp files)

Both are **pure CPU VU1 emulation** — no GL. They take VIF DMA data (plain floats / u16 packed) and produce vertex data via the in-memory `m_vu_data[1024]` VU1 buffer and the shared `Vu` register file (32 Vf + integer regs + scratch). The final output is pushed to the GS via `xgkick(addr)` -> `m_common_ocean_renderer.kick_from_mid / kick_from_near`. Input for each call comes from the VIF DMA stream decoded in the driver files (OceanMid.cpp / OceanNear.cpp / OceanTexture.cpp), via STCYCL 0x404 + UNPACK_V4_32/V4_8 memcpy into `m_vu_data`.

### OceanMid_PS2.cpp (mid ocean) — `void OceanMid::...`

| Function | Input | Produces | PS2 original |
|---|---|---|---|
| run_call0_vu2c | m_vu_data + Constants | sets up VU1 scratch / index_table via lq/sq on m_vu_data | L0: load constants, init loop counters, compute start positions |
| run_call41_vu2c / run_call43_vu2c | m_vu_data | small helper transforms on the buffer | sub-routines called between main VU1 stages |
| run_call46_vu2c | m_vu_data | vertex geometry stage | one of the mid-ocean wave/transform passes |
| run_call73_vu2c (+ _jak2) | m_vu_data + index_table | vertex geometry (envmapped ocean draw path; the code comment marks `xgkick` "draws envmapped ocean") | jak1/jak2 near-identical wave mesh |
| run_call107_vu2c (+ _jak2) | m_vu_data | geometry pass | longer transform pass (jak2 variant differs) |
| run_call275_vu2c (+ _jak2) | m_vu_data | geometry pass | |
| run_L26_vu2c | m_vu_data | wave displacement transform | |
| run_L32_vu2c (+ _jak2) | m_vu_data | wave displacement (jak2 differs) | |
| run_L38_vu2c (+ _jak2) | m_vu_data | wave displacement | |
| run_L43_vu2c / run_L45_vu2c | m_vu_data | final geometry | |

All functions: `lq_buffer`/`sq_buffer`/`ilw`/`isw` on `m_vu_data`, `vu.vfNN.{add,mul,sub,set_zero}` register math, then `xgkick(viNN)`. The helper free functions `fcand`/`fcor` (lines 31/40) do float-AND/OR on u16. UNCLEAR: exact per-function wave meaning (not documented in code).

### OceanNear_PS2.cpp (near ocean) — `void OceanNear::...`

| Function | Input | Produces | PS2 original |
|---|---|---|---|
| run_call0_vu2c (+ _jak2) | m_vu_data + Constants | near-ocean geometry init | L0: constants + counters |
| run_call39_vu2c (+ _jak2) | m_vu_data | geometry transform | |
| run_L15_vu2c (+ _jak2) | m_vu_data | wave/transform pass | |
| run_L21_vu2c (+ _jak2) | m_vu_data | wave/transform pass | |
| run_L23_vu2c | m_vu_data | wave/transform pass | |
| run_L25_vu2c (+ _jak2) | m_vu_data | wave/transform pass | |
| run_L30_vu2c / run_L32_vu2c | m_vu_data | final geometry + xgkick | |

Dispatch: `run_call0`/`run_call39` are reached from `OceanNear::render_jak1/jak2` via the MSCALF+STMOD VIF codes (immediate 0 and 39). The `run_L*` are called from within the `run_call*` functions (bal/jal). All produce vertices into shared buffers and end with `xgkick`.

### Where input comes from in the frame

- Mid path: `OceanMid::run/run_jak2` (OceanMid.cpp) decodes STCYCL 0x404 UNPACK_V4_32/V4_8 DMA into `m_vu_data` and calls the run_* functions. Constants struct (0x240 bytes) is DMA'd in at VU1 addr 0x2dd.
- Near path: `OceanNear::render_jak1/jak2` (OceanNear.cpp) decodes the same VIF codes into `m_vu_data`, then `run_call0/call39`. 32-byte setup DMA + BASE/OFFSET DMA prep it.
- Texture path: `OceanTexture::handle_ocean_texture_jak1/jak2` (OceanTexture.cpp) — VU1 program with `TexVu1Data` addresses (BUF0=384, BUF1=583, DEST0=782, DEST1=881, CONSTANTS=985) and `TexVu1Prog` (START=0, REST=2, DONE=4); the PC path (jak2/3) is `OceanTexture_PC.cpp`.

## 6. Loader / textures

The ocean has **no source-image textures loaded from disk**. Every ocean texture is generated at runtime OFFSCREEN (render-to-texture) and then uploaded to VRAM via the TexturePool. There are no `ocean` references in `game/graphics/opengl_renderer/loader/` (grep returned nothing). The data the ocean needs is: (a) the live sky color (for the envmap) and (b) VU1-processed vertex data (for the body). 

| Data | Source file | Format | Approx size / VRAM addr |
|---|---|---|---|
| Ocean surface texture (animated waves) | OceanTexture.cpp / OceanTexture_PC.cpp | RGBA8, 128x128, up to 8 mips (NUM_MIPS=8); generated OFFSCREEN via VU1/PC then uploaded | TBP JAK1=8160, JAK2/3=672 (OCEAN_TEX_TBP_JAK1/JAK2); 128*128*4 = 64 KB base level |
| Ocean envmap (sky reflection) | OceanEnvmap.cpp | RGBA8, 64x64 (ENVMAP_WIDTH/HEIGHT); 2 offscreen FBOs (first-pass + final envmap); single-level linear | 64*64*4 = 16 KB; ENVMAP_VRAM_ADDR = 0xf80 |
| Haze (radial gradient overlay on envmap) | OceanEnvmap.cpp `render_haze` | rendered into the envmap FBO via OCEAN_ENVMAP_HAZE shader | part of the 64x64 envmap |
| Envmap radial pass | OceanEnvmap.cpp `render_envmap` | samples `first_pass` FBO into envmap FBO via OCEAN_ENVMAP (sampler tex_T1) | 64x64 |
| Ocean geometry (near+mid body) | CommonOceanRenderer.cpp | no texture; indexed vertex mesh, 3 buckets (RGB_TEXTURE / ALPHA / ENV_MAP), streamed via glBufferData GL_STREAM_DRAW | up to 4096*10 vertices (preallocated) and 3 index arrays |

Constants / buffers fed in via DMA (not files):
- Mid ocean Constants struct (0x240 bytes, `OceanMid.h`): hmge_scale, inv_hmge_scale, hvdf_offset, fog, constants, constants2, GIF tags (drw_fan/env_fan/drw_textur*/env_*), index_table[8], pos0..pos3. DMA'd to VU1 addr 0x2dd.
- Texture constants (`OceanTexture.h`, 112 bytes): giftag, buffers, dests, start, offsets, constants, cam_nrm. VU1 addrs BUF0=384/BUF1=583/DEST0=782/DEST1=881/CONSTANTS=985.
- Sky color (ocean envmap): pulled live from the DMA stream by OceanEnvmap `find_sky_color` (reads RGBA of an untextured sky draw, `OceanEnvmap.cpp:64`).

## 7. Frame flow (ordered step lists from the 2 bucket render() methods)

### OceanMidAndFar::render_jak1 (OceanMidAndFar.cpp:52-93)

1. Jump to bucket (read 1 DMA tag, assert vif0/vif1/size = 0).
2. If next tag is a CALL (renderer did not run), drain 4 tags and return (early out).
3. `m_direct.reset_state()`.
4. **texture**: `m_texture_renderer.handle_ocean_texture_jak1` (OFFSCREEN animated ocean surface texture into a 128x128 FBO; if mipmaps, render to temp then `make_texture_with_mipmaps` GL draw; result moved to VRAM TBP 8160).
5. **far**: `handle_ocean_far` — read 160-byte init GIF + loop DIRECT GIF draws (`m_direct.render_gif`); patches TA0 to 0 in the init data. (Far/horizon ocean.)
6. `m_direct.flush_pending` + `set_mipmap(true)`.
7. **mid**: `handle_ocean_mid` -> `m_mid_renderer.run` (VU1) -> CommonOceanRenderer accumulates vertices, then `flush_mid` (2 glDrawElements; bucket 3 = ocean-texture, bucket 4 = envmap with reverse-index + blend trick).
8. Trailing: read final next-tag NOP, drain 4 tags, assert reached next_bucket.
9. `m_direct.flush_pending` + `set_mipmap(false)` (restore state).

### OceanMidAndFar::render_jak2 (OceanMidAndFar.cpp:95-152)

1. Jump to bucket; early-exit if already at next_bucket.
2. `m_direct.reset_state()`.
3. **envmap**: `m_envmap_renderer.handle_ocean_envmap_jak2` — pull live sky color, scan DMA for scissor/frame, render OFFSCREEN (first-pass FBO 64x64 -> haze pass via OCEAN_ENVMAP_HAZE -> radial envmap pass OCEAN_ENVMAP via tex_T1), then `move_existing_to_vram` to TBP 0xf80 (or page_tbp if registered).
4. **texture**: `m_texture_renderer.handle_ocean_texture_jak2` (PC path in OceanTexture_PC.cpp; 128x128, TBP 672).
5. **far**: `handle_ocean_far` + `m_direct.flush_pending` + `set_mipmap(true)`.
6. **mid**: `handle_ocean_mid` -> `m_mid_renderer.run_jak2` -> CommonOceanRenderer `flush_mid`.
7. Trailing NOP read, drain 2 tags, flush, `set_mipmap(false)`.

### OceanNear::render_jak1 / render_jak2 (OceanNear.cpp:52-219; near-identical)

1. Jump to bucket; if CALL (not run), drain 4 tags + return.
2. **texture**: `m_texture_renderer.handle_ocean_texture_jak1/jak2` (second surface-texture gen; jak1 vs jak2 call names). *Code comment asks why this repeats the previous ocean render — UNCLEAR.*
3. Assert `qwc == 2` (else error+drain).
4. `m_common_ocean_renderer.init_for_near()` + 32-byte setup DMA (NOP + DIRECT assert).
5. BASE/OFFSET DMA (VU1 base 0, offset 0x10).
6. Main loop until end tag (CNT + NOP + DIRECT): read each DMA; STCYCL 0x404 + UNPACK_V4_32 -> memcpy to m_vu_data[addr]; MSCALF + STMOD -> dispatch `run_call0`/`run_call39` (jak1) or `*_jak2` (these are the VU1 wave passes in OceanNear_PS2.cpp).
7. Drain to next_bucket; `m_common_ocean_renderer.flush_near` (2 glDrawElements, same bucket 3/4 + reverse/blend trick as mid).

### Cross-cutting facts for the deko3d port
- All texture generation is OFFSCREEN render-to-texture (FBO). In deko3d this maps to image render targets + blit, **not** CPU read-back (there is none). The 3 FBOs: m_result_texture/m_temp_texture (128x128, OCEAN_TEXTURE) and m_first_pass_fb/m_envmap_fb (64x64, envmap).
- The single shared "body" draw is `CommonOceanRenderer::flush_*` using OCEAN_COMMON with `bucket`=3 (ocean-texture, no blend, depth ALWAYS) then `bucket`=4 (envmap, reversed indices + blend DST_ALPHA/ONE to skip the low-poly overdraw). Texture filter: MIN=LINEAR_MIPMAP_LINEAR, MAG=LINEAR, WRAP=REPEAT.
- 4 attribute locations in OCEAN_COMMON; loc3 (fog) is integer (glVertexAttribIPointer).
- No GL read-back anywhere; no FBO attachment via glFramebuffer in these files (the FBOs are FramebufferTexturePair objects built elsewhere). UNCLEAR: where FramebufferTexturePair creates its framebuffer/glFramebuffer — not in the ocean/ folder.
