# T22c — dk_texture_gpu: BCn direct upload (P2.3)

Third slice of the old T22 (needs T22b ✅). Adds `upload_bcn` — BCn payloads upload
DIRECT from the card bytes, zero CPU decode (fr3 v44, FIX 74).

## API cheat-sheet — REVIEWER-VERIFIED 2026-10-16 (docs/deko3d-ref/include/deko3d.h)
- formats: `DkImageFormat_RGB_BC1` L440, `DkImageFormat_RGBA_BC3` L443,
  `DkImageFormat_RGBA_BC7_Unorm` L452. BC2: grep `BC2` yourself for the exact name
  (expected `DkImageFormat_RGBA_BC2`); if it does not exist, map `Fmt::Bc2` → fail with
  a `[dk] tex bcn fmt unsupported (bc2)` log (never a crash).
- Everything else (layout maker, descriptor, CopyBufferToImage, barrier) is already in
  place from T22b — reuse its helpers exactly.
- For the compressed-source `DkCopyBuf` layout, FIRST read the vendored textures
  example `docs/deko3d-ref/examples/deko_examples/source/` (the Example0x that uploads
  textures) and mirror its rowLength/imageHeight pattern for compressed data; note in
  your report which example and what values it uses.

## Rules
- Only dk_*.cpp includes <deko3d.h>. Log `[dk]`, one line. STOP rule as always.

## Edits
1. `game/graphics/deko3d_renderer/dk_texture_gpu.h`:
   `uint32_t upload_bcn(dktex::Fmt fmt, uint32_t w, uint32_t h, const void* src, uint32_t size);`
2. `game/graphics/deko3d_renderer/dk_texture_gpu.cpp`: same flow as upload_rgba8 but:
   - fmt map Bc1→RGB_BC1, Bc2→(see above), Bc3→RGBA_BC3, Bc7→RGBA_BC7_Unorm;
     Rgba8→reject (use upload_rgba8).
   - payload sanity (caller's `size` wins; only WARN-log if it differs):
     block bytes BC1=8, BC2/BC3/BC7=16; blocks = ceil(w/4)*ceil(h/4).
   - `dktex::Entry` fmt field set from the argument; layout maker format = mapped enum.
   - CopyBuf/rect per the example you mirrored; staging path identical (payload ≤ 2 MB).

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
Inert grep unchanged (no new callers). QWEN_REPORT `## T22c` (files + the example/values
you mirrored + last 5 log lines + md5). README row → 🟡. No commit, no deploy.
