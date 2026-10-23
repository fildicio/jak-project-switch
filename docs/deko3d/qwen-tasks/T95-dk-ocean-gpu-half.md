# T95 — dk ocean GPU half: drivers, flush recorder, texture gen, wiring (AI-assisted)

Status: ✅ code done 2026-10-22 (reviewer-executed — Qwen unavailable, same as T93/T94); HW test pending

## Goal
Draw the jak1 ocean on the deko3d path: bucket 4 (ocean-mid-far) and bucket 63
(ocean-near), using the T93/T94 CPU decode (`dkocean::CommonRecorder`,
`dkocean::Near`, `dkocean::Mid`).

## Design (one page)
- `dk_ocean_texture.{h,cpp}` — verbatim port of the OceanTexture jak1 CPU half
  (OceanTexture_PC.cpp run_L1/L2/L3/L5_PC + xgkick_PC + helpers; OceanTexture.cpp
  `handle_ocean_texture_jak1` walk lives in the driver). Output = static
  positions + dynamic (s,t,rgba) + u32 restart index list + envmap tbp. No GL.
- `dk_ocean_driver.cpp` — plain C++: bucket walks for 63 (near) and 4
  (mid-far), ported verbatim from OceanNear.cpp / OceanMidAndFar.cpp /
  OceanMid.cpp (DMA reads + asserts); far GIF chunks are fed to the new
  `dkdirect::render_ocean_far_gif()`; texture-gen walk fills the TextureGen.
  Holds the static `dkocean::Near`/`Mid` instances + stats.
- `dk_ocean_flush.cpp` (Switch-only) — the GL flush_near/flush_mid port:
  uploads vertices/indices to rings, records ocean_common draws with the GL
  blend/depth states (near: GEQUAL+StdAlpha, ZERO/ONE alpha-only, DST_ALPHA/ONE
  + reversed indices for envmap; mid: ALWAYS write, then DST_ALPHA/ONE), and
  the ocean-texture generation draw into a 128x128 dktexgpu render target
  (registered in the pool at OCEAN_TEX_TBP_JAK1=8160, T82 eye pattern).
- Shaders: `ocean_common.{vert,frag}.glsl` + `ocean_texture.{vert,frag}.glsl`
  in deko3d_renderer/shaders (std140 UBOs, tokens per gen-dksh.py), CMake
  dk_ocn target, dk_device load + accessors.
- dk_renderer wiring: `gk_noocean.txt` opt-out flag `s_dk_ocean` (jak1 only),
  capture after sprite (texture uploads), build after tfrag, record in GL
  bucket order (mid-far 4 after sky 3 before tfrag 6; near 63 between sprite 66
  and direct 67). Telemetry line every 600 frames.

## Known limitations (documented, first pass)
- No mipmaps on the ocean texture (dktexgpu is single-level; GL's mip alpha
  fade chain is dropped). Sampler = LINEAR/REPEAT.
- ocean_envmap_haze / ocean_envmap / ocean_texture_mipmap shaders NOT ported
  (jak2 envmap path; jak1 doesn't run it).
- jak2/3 ocean: early-out with one log line (this task is jak1 scope).

## Acceptance
- goalc-test builds green with new host tests; `check-dk.py` PASS;
  `check-switch.sh jak1` PASS; verbatim diff of the copied bodies vs _PS2/GL
  sources (only documented deltas).
