# T22c — dk_texture_gpu: BCn direct upload (P2.3)

Third slice of the old T22 (needs T22b ✅). Adds `upload_bcn` — BCn payloads upload
DIRECT from the card bytes, zero CPU decode (fr3 v44, FIX 74).

## API cheat-sheet — REVIEWER-VERIFIED 2026-10-16/17 (docs/deko3d-ref/include/deko3d.h)
- format map (BC2 verified 2026-10-17 — all four EXIST, no grep needed):
  Bc1→`DkImageFormat_RGB_BC1` L440, Bc2→`DkImageFormat_RGBA_BC2` L442,
  Bc3→`DkImageFormat_RGBA_BC3` L443, Bc7→`DkImageFormat_RGBA_BC7_Unorm` L452;
  Rgba8→reject with a `[dk] tex bcn fmt unsupported (rgba8)` log (use upload_rgba8).
- Everything else (layout maker, descriptor, memblock/bump cursor, staging, barrier) is
  already in place from T22b — reuse its helpers exactly.
- `DkCopyBuf` for compressed sources (REVIEWER-VERIFIED 2026-10-17 — do NOT go looking
  for an example): the only BCn upload in the vendored examples is
  `deko_examples/source/SampleFramework/CExternalImage.cpp` (Example04 loads
  cat-256x256.bc1 through it); it aggregate-initializes DkCopyBuf with ONLY the address —
  rowLength = 0, imageHeight = 0 — which deko3d reads as "tightly packed, derive the
  pitch from the image layout". Use exactly:
  `const DkCopyBuf cp = { dkdev::staging_gpu(), 0, 0 };`
  `const DkImageRect rc = { 0, 0, 0, w, h, 1 };`  // w,h in PIXELS
  Do NOT compute blocks_x * block_bytes row pitches yourself.

## Rules
- Only dk_*.cpp includes <deko3d.h>. Log `[dk]`, one line. STOP rule as always.

## Edits
1. `game/graphics/deko3d_renderer/dk_texture_gpu.h`:
   `uint32_t upload_bcn(dktex::Fmt fmt, uint32_t w, uint32_t h, const void* src, uint32_t size);`
2. `game/graphics/deko3d_renderer/dk_texture_gpu.cpp`: same flow as upload_rgba8 but:
   - fmt map: exactly the cheat-sheet map (Bc1→RGB_BC1, Bc2→RGBA_BC2, Bc3→RGBA_BC3,
     Bc7→RGBA_BC7_Unorm); Rgba8→reject (use upload_rgba8).
   - payload sanity (caller's `size` wins; only WARN-log if it differs):
     block bytes BC1=8, BC2/BC3/BC7=16; blocks = ceil(w/4)*ceil(h/4).
   - `dktex::Entry` fmt field set from the argument; layout maker format = mapped enum.
   - CopyBuf/rect: exactly the `{ dkdev::staging_gpu(), 0, 0 }` / `{ 0, 0, 0, w, h, 1 }`
     pair from the cheat-sheet (tight packing); staging path identical (payload ≤
     512*1024*4 bytes, same guard as upload_rgba8 step d).

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
Inert grep unchanged (no new callers). QWEN_REPORT `## T22c` (files + last 5 log lines +
md5). README row → 🟡. No commit, no deploy.
