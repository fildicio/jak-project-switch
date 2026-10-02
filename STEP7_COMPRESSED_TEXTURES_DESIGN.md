# STEP 7 DESIGN NOTE — Pre-compressed (BCn) level textures (AI-assisted)

Status: **IMPLEMENTED 2026-10-02 (FIX 74, branch `compression-ecc`).**
The design below is as originally planned; see the "As built" section at the
end for what shipped and the measured numbers. Desktop boot validated; Switch
hardware test pending.

## 1. The problem this fixes (and nothing else does)

User's #1 complaint: entering/rendering a new area goes into **slow motion**
while geometry + textures stream in. The logs have localized this completely:

| Evidence (hardware logs) | Number |
|---|---|
| Texture submit cost per texture (F67) | ~1.6 ms, growing to 2.43 ms later in session |
| One jak2 city level texture stage | 1222 textures, upload **2132 ms** |
| Runtime mipgen per city level | **1060 ms** (`mipgen 1059.7ms`) |
| Slow-setup lines during streams | 14–37 ms frames (`mip rate=8 did=8`) |
| Streaming hitches per session, EVERY build F62..F70b | ~250–275 |
| F71 hardware note | "Haven City loads are **texture-upload bound**" |

Why the already-tried levers can't finish the job:

- **FIX 9** chunked geometry uploads (256 KB chunks) — done, geometry is paced now.
- **FIX 33** row-band texture uploads — crashed on nouveau (partial-upload staging,
  esr 0x92000007). Banding from client memory is a dead end on this driver.
- **FIX 42** defers mipgen to 8/frame — still costs 14–37 ms "slow setup" frames,
  and every streamed texture still pays it during gameplay.
- **FIX 48** made the upload a straight memcpy (format fix) — no more headroom.
- **FIX 68** tuned budgets — took the rate from ~2 to ~5–7 textures/frame; ceiling.
- **FIX 69 PBO async upload — HARDWARE-REJECTED**: nouveau has no orphaning for
  in-use PBOs; every refill blocks on the GPU (56.5 ms stalls, 1013 occurrences).
  "Uploads off the frame" is **closed on this driver**.

Every remaining path points at one conclusion: the only lever left is the
**number of bytes**. RGBA8888 is 32 bits/pixel; BCn is 4–8 bits/pixel.
Pre-compressed textures also make mipgen a load-time non-event (mips come
from the file), and cut VRAM 4–8x (directly reduces the FIX 63 GPU-OOM risk).

## 2. What stays out of scope (first cut)

- **Only fr3 level textures** (tfrag / tie / shrub + per-level + `common` fr3).
  This is the streaming path = the slow-motion path.
- GOAL runtime textures (TextureConverter / merc / sky / TextureAnimator)
  stay RGBA exactly as today. No GOAL recompile, no `.gc` changes, no CGO/DGO changes.
- Desktop keeps the identical RGBA path (BCn is Switch-motivated; desktop
  doesn't need it and must not regress).

## 3. Format choice

- **BC7** (GL_COMPRESSED_RGBA_BPTC_UNORM, 8 bpp) as the default: high quality,
  proper 8-bit alpha (PS2 alpha semantics already survive the RGBA conversion).
- **BC1** (GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 4 bpp) for large **opaque** textures
  (>64x64, alpha unused) — the biggest 8x byte win where quality allows.
- **Not ASTC**: Tegra X1 decodes it, but nouveau/Mesa exposure is the risky part;
  S3TC is confirmed present in our Mesa build (`s3tc_compatible_internal_formats`
  symbol, `glad_glCompressedTexImage2D` loaded). BPTC must still be probed at boot
  (see §5.4) — if absent, alpha textures fall back to BC3, opaque to BC1; the
  encoder picks per-texture so the file carries only one format per texture.

Encoder: bundle **bc7enc + rgbcx** (richgel999, MIT, small, header-ish C++) in
`third-party/` for the **extractor only**. The Switch NRO never encodes
anything — it only feeds pre-made bytes to `glCompressedTexImage2D`, so the
console binary does not grow or slow down.

## 4. Changes by component

### 4.1 `common/custom_data/Tfrag3Data.h` (shared by extractor + game)
- `tfrag3::Texture`: add `u8 format` (0 = legacy RGBA, 1 = BC1, 3 = BC3, 7 = BC7)
  and per-mip data: `std::vector<u8> bcn_data` + `std::vector<u32> mip_offsets`
  (byte offset + length of every mip level, including level 0). The legacy
  `std::vector<u32> data` stays untouched for the RGBA path.
- `tfrag3::Level::serialize(Serializer&)`: write a **file header with magic +
  version** before everything else. Today there is none (verified:
  `Serializer(decomp_data...)` → `Level::serialize`, Loader.cpp ~647).
  - Old files (no magic) → read as v0 RGBA (backward compatible loader).
  - New files start with magic+version; a too-new version fails with a clear
    `[loader] fr3 version mismatch` log instead of a garbage read.

### 4.2 `common/texture/` — new `bcn_encode.{h,cpp}`
- Encode RGBA words → BC7/BC1 blocks; build full mip chain with a box filter that
  matches `glGenerateMipmap` closely enough visually (straight alpha, no premultiply).
- Used **only** by the decompiler/extractor build target. Size each mip level as
  `((w+3)/4) * ((h+3)/4) * bytes_per_block` (8 for BC1, 16 for BC3/BC7).

### 4.3 `decompiler/level_extractor/` (`extract_level.cpp`, `extract_tfrag.cpp`)
- After the existing RGBA conversion (texture_conversion.h), encode each texture
  + mips per §3 rules; set `format` + `mip_offsets`. RGBA data is dropped for
  encoded textures (that's the point — the fr3 gets smaller).
- New extractor flag `--no-bcn` to regenerate legacy v0 fr3 files (rollback path
  that needs no code changes on the console).

### 4.4 `LoaderStages.cpp` `add_texture` (the FIX 33a/42/48/69 battleground)
- Branch on `format`:
  - legacy RGBA → exactly today's path (GL_UNSIGNED_BYTE whole-texture
    `glTexImage2D`, FIX 42 mip queue, unchanged).
  - BCn → `glCompressedTexImage2D` **per mip level** from `mip_offsets` —
    a 4–8x smaller upload that fits inside the per-frame byte caps, and
    **no `glGenerateMipmap` ever** (texture skips the FIX 42 queue, already
    complete).
- Keep: FIX 34 `g_tex_upload_ms` instrumentation, FIX 52 gpu_scale byte
  accounting (bytes just get smaller), FIX 68 budgets, one-shot `[texfmt]` log.
- Kill-switch, FIX 69-style: `sdmc:/gk_nobcn.txt` present → log once and skip
  the level load with a clear message (we cannot decode BCn on the console
  cheaply — the file IS the data; the real fallback is re-extract `--no-bcn`).

### 4.5 `TexturePool`
- No format knowledge needed: the loader hands over ready GL texture names.
- `GL_TEXTURE_MAX_LEVEL` set to the uploaded mip count (the FIX 42a
  completeness-check logic already manipulates this — reuse it).

## 5. Risks & mitigations

1. **nouveau BPTC/BC7 exposure unproven** → §4.4 boot probe logs
   `[texfmt] s3tc=1 bptc=?` on first texture; BC3/BC1 fallback chosen at
   *extract* time per build; one hardware run decides.
2. **BC1 artifacts** (gradients/sky) → BC1 only for large opaque textures;
   everything else BC7. Visual check at 720p on hardware before calling it done.
3. **Version skew on the SD card** (old NRO + new fr3) → file magic makes the new
   NRO accept both; an old NRO meeting new data fails at a clear log line.
   Deploy order: NRO first, then fr3 files, same session.
4. **First time we touch game data on the SD card.** Only NROs were ever replaced.
   fr3 files are regenerable on the Mac (`--no-bcn`), so no `.bak` copies on the
   card (space); the Mac keeps the old `out/jakN` tree until hardware sign-off.
5. **Extractor cost**: BC7 encode is slowish (~1–3 ms per texture × 1000+
   textures/game) — expect extract to run some minutes longer. One-time cost.
6. **fr3 on-disk size**: today zstd over RGBA compresses well; BCn barely
   compresses. Net file size expected similar-or-smaller (raw is 4–8x smaller);
   measured on jak1 before the full rollout; SD free space checked first.
7. **Mip semantics**: offline mips replace `glGenerateMipmap` — slight filter
   differences acceptable; alpha handling mirrors the runtime path.

## 6. Rollout order (one game first, as always)

1. Implement §4.1–4.3; extract **jak1 only** on the Mac; verify fr3 sizes.
2. Implement §4.4–4.5; **desktop** build runs jak1 with new fr3 (visual sanity:
   no black/purple textures — the FIX 33a/47 lessons).
3. Switch build jak1; deploy NRO + jak1 fr3 (md5-verify; old fr3 regenerable).
4. User hardware test, fixed 10-min route incl. 2 area transitions; compare:
   - hitch count / max-in-15 s (plan's step-2+ metric rule)
   - `tex stage: upload X ms / mipgen Y ms` (expect mipgen ≈ 0, upload 4–8x lower)
   - `ready in N s` per level, `slow setup` line frequency
5. Numbers good → jak2, jak3 same pipeline. Numbers bad → `--no-bcn` re-extract
   restores today's behavior with zero console changes.

**Acceptance target:** ≥50% fewer streaming hitches on the fixed route,
mipgen gone from logs, user-visible: new-area slow motion largely gone.

## 7. Cost estimate

- Extractor + serializer + encoder bundling: ~1 session.
- Loader + pool + probe: ~1 session (incl. desktop visual pass).
- Extract ×3 + hardware iterations: user-paced.
This is the plan's "several days" item — but it is the **only** remaining fix
for the streaming slow-motion; everything cheaper has been tried and measured.


## 8. As built (2026-10-02, FIX 74, branch `compression-ecc`)

Deviations from the plan above, all deliberate:

1. **S3TC only (BC1/BC3), no BC7/BPTC.** BPTC support on our nouveau/Mesa build
   is unproven; S3TC is a Tegra X1 fixed function. And in practice **every jak1
   texture has at least one non-opaque pixel** (PS2 t_blend), so everything took
   BC3 (8 bpp) anyway - BC1 would only apply to fully opaque textures. Result is
   the same "fewer bytes" lever, with formats the driver definitely supports.
2. **Hand-written encoder**, `decompiler/level_extractor/bcn_encode.{h,cpp}`
   (decompiler-only - zero bytes added to the NRO). No third-party bc7enc/rgbcx:
   BC1/BC3 endpoint fitting (bbox + diagonal-projection extremes, both alpha
   modes for BC3) is small and good enough for PS2-era art at 720p.
3. **No new file magic needed.** `tfrag3::Level` already had a version check;
   `TFRAG3_VERSION` 43 → 44 rejects old fr3 with the existing "did you forget to
   re-decompile?" assert, and old NROs refuse new fr3 the same way. NRO and data
   must deploy together per game (as planned).
4. **Rollback switch is the decompiler config key `bcn_textures`** (default
   true), mergeable via `--config-override '{"bcn_textures": false}'` on the
   extract command - same effect as the planned `--no-bcn` flag. Plus the full
   pre-FIX74 backup of all three games' SD data at
   `~/Desktop/jak bakcups/2026-10-01-pre-FIX74-full/` (verified: 0 differing
   files). Kill-switch file `sdmc:/gk_nobcn.txt` was NOT implemented: the
   compressed path is data-format-level, not runtime-optional (an NRO built for
   v44 fr3 still loads RGBA textures fine - the upload path handles both).
5. **Sky textures stay RGBA** (name/tpage contains "sky", case-insensitive):
   SkyBlendCPU reads sky pixels on the CPU via `TexturePool::get_data_ptr`.
   Compressed textures hand the pool `src_data = nullptr`; every CPU reader
   already null-checks. jak1 citadel: 8 sky textures excluded.
6. **Mips ship in the file** (2x2 box filter, full chain to 1x1) and upload via
   one `glCompressedTexImage2D` per level; `GL_TEXTURE_MAX_LEVEL = n-1`. The
   FIX 42 mipq / `glGenerateMipmap` path is never taken for compressed textures,
   so the ~1s/level deferred-mipgen backlog disappears entirely.

Measured (jak1, host extract):

- Texture bytes shipped (all mips included): **33.5–34.7% of the old level-0
  RGBA cost** (~3x fewer bytes uploaded AND ~3x less VRAM). citadel: 1207 BC3 /
  9 RGBA(sky). GAME.CGO common: 346 BC3.
- fr3 file sizes (zstd on top): 66–85% of old (geometry dominates most levels).
- Desktop boot (`gk -boot -fakeiso`, macOS GL): `[texfmt] FIX 74 BCn compressed
  texture path active (first texture format 3)`, version check passes, levels
  stream. First run aborted on the EyeRenderer scissor assert - **this was a FIX
  74 regression, not pre-existing** (see the EyeRenderer note below); after the
  fix, a 15-minute soak on the title/village1 backdrop (1070 draws/frame, merc +
  sprite + sky-blend buckets active) ran clean with zero asserts.
- Switch budget integration: the loader's per-frame byte accounting now counts
  `bcn_data.size()`, so the FIX 46c/66 dispatch caps scale with the actually
  uploaded bytes (a level's texture stage finishes in ~3x fewer dispatches).

### Post-deploy fix: EyeRenderer pupil DMA desync (f74b)

The first Switch hardware test crashed exactly at the intro cutscene (Jak &
Daxter closeups), matching the desktop abort. Root cause: BCn textures give the
TexturePool `src_data = nullptr`, and `EyeRenderer::get_draws` gated its pupil
`read_eye_draw(dma)` calls (which **consume DMA transfers**) behind
`tex1->get_data_ptr()`. GOAL always sends the pupil transfers, so skipping the
read desynced the DMA walker and the next `decode_scissor` tripped its
`SCISSOR_1` assert. The renderer never reads pupil pixels on the CPU - it only
needs the GL handle, which is valid for compressed textures - so the fix is to
always consume the pupil DMA and gate only the draw on `tex1 != nullptr`
(same as the iris/lid paths, which were already unconditional). Runtime-only
change (`EyeRenderer.cpp`); fr3 data is unaffected - no re-extract needed.

### Post-deploy fix 2: area loads slower than f73c (f74c, immutable storage)

f74b hardware results: in-level FPS **improved** (Sandover), but loads into new
areas got visibly **slower**. The SD run log (`gk_stdout.txt`) showed why - the
texture stage's measured upload CPU:

- f74b (BCn, one `glCompressedTexImage2D` per mip): 640-737 textures,
  **3855-8576 ms per level** (~9.6 ms/texture at worst).
- f73c-era baseline (RGBA, one `glTexImage2D` level-0 + deferred mipgen):
  1.2-1.4 ms/texture (e.g. 1222 textures / 1416 ms).

The bytes shrank 4-8x but the **call count grew ~11x**, and every
`glCompressedTexImage2D` makes mesa/nouveau re-derive the texture's storage;
the fixed per-call driver cost dominates on the A57. FIX 74c switches
`upload_bcn_texture` to one `glTexStorage2D` for the whole mip chain (single
immutable miptree allocation) + `glCompressedTexSubImage2D` per level (a pure
copy into existing storage). The upload level count is clamped to
`floor(log2(max(w,h)))+1` (GL's legal maximum - a full 1x1 chain satisfies it
exactly), `w/h > 0` joins the validation, and the storage path is probed once
on the first texture (`glGetError` after a clear) with a permanent fallback to
the old per-mip `glCompressedTexImage2D` loop if a driver rejects it.
Verification markers on hardware: `[texfmt] FIX 74c glTexStorage2D accepted`
and the `[loader] tex stage:` upload times dropping toward the ~1 ms/texture
range. Dispatch caps (FIX 46c/66) are untouched - they were not the binding
constraint (stage wall time tracked upload CPU 1:1).

