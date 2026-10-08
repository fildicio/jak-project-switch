# Jak 2 → Jak 3 carry-over: compression, loading and fps work (AI-assisted)

Written 2026-10-08, after FIX 93 was confirmed on hardware. The user reported: "way better". This document covers everything done on jak2 from FIX 84 to FIX 93, what the Jak 3 rollout needs, and the traps hit along the way. The detailed per-fix notes are in `CLINE_HANDOFF.md`. The older jak1 → jak2/3 plan is in `JAK2_JAK3_ROLLOUT_PLAN.md`.

## 0. TL;DR for Jak 3

1. **Almost all of this is shared C++.** It is gated on `__SWITCH__`, not on the game version, so a jak3 NRO built from HEAD inherits it all automatically. The only jak2-specific piece is the prefetch seed table (`kJak2LevelAdjacency`). Jak3 stays learned-graph-only.
2. **The hard step is data, not code.**
   - HEAD has `TFRAG3_VERSION` 44 (BCn fr3). The jak3 card still holds v43 RGBA fr3 together with a pre-FIX 85 NRO.
   - Re-extract jak3 at v44.
   - Rebuild GOAL with **`--instruction-set arm64`**. Without that flag the kernel crashes at boot (FIX 85).
   - Deploy the NRO and **all** fr3 files together. A v44 NRO asserts on v43 fr3, and the reverse also fails.
   - Back up the v43 fr3 first. For jak2 no v43 backup was kept, so rollback was impossible.
3. **Expected final state, matching jak2 f93:**
   - The fr3 files on the SD stay BCn, which means smaller reads.
   - The loader thread decodes them to RGBA (FIX 91).
   - The render thread uploads level 0 only with one `glTexImage2D` call (FIX 90).
   - Mip chains are built later by the mip queue (FIX 42).
   - Then add the CPU floor (FIX 92) and the render trims (FIX 93).
4. **Build:** `docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest env SWITCH_GAME=jak3 BUILD_DIR=/work/build-switch-jak3 bash scripts/build-switch.sh`

## 1. The key lesson: on Switch (nouveau), upload CALLS cost time, not bytes

| Build | Texture path | ms / texture (1222-tex lwidea) | Verdict |
|---|---|---|---|
| f73c | RGBA, one `glTexImage2D` + deferred mips | ~1.2 | baseline |
| f85 | BCn, `glTexStorage2D` + one `glCompressedTexSubImage2D` per mip | 8–13 | **much worse**: the intro, saves and areas were all slower |
| f88 | BCn + 16-slot PBO ring | 8–13 | no gain → default OFF |
| f89 | forced CPU decode, but **every mip** uploaded as RGBA | 8.4 (identical) | proved the cost is the per-call ~1 ms, not the format |
| f90 | decode + **level 0 only** + mipq | ~2.3 | 3.5–4× faster |
| f91 | decode moved to the **loader thread** | ~1.4–2.3 | render thread back on the f73c path |

Takeaways:
- Never upload more than one `glTex*Image` call per texture on the render thread.
- BCn on the card is still worth it (fr3 633 → 501 MB, faster SD reads). It just must not reach the GPU as BCn.
- `GL_TEXTURE_COMPRESSED` reported "keeps S3TC". Mesa answers that query from the logical format, so the probe cannot be trusted. Only timing A/B tests settle questions like this.
- Decoding on the loader thread is cheap: 43 ms per 1222 textures and 167 ms per 848 textures.
- VRAM use goes back to RGBA levels, the same as f73c. That was fine on jak2. Watch jak3 for OOM, because its levels are bigger.

## 2. Fix-by-fix list (all in shared code unless noted)

| Fix | What | Where | Jak 3 action |
|---|---|---|---|
| 84 | DirectRenderer fenced segment ring, never re-specified; fixes GPU MMU fault 2520 / NULL bo crashes | `DirectRenderer.cpp/.h` | already in jak3 f84 NRO; keep |
| 85 | BCn v44 fr3 extraction (BC1/BC3 + file mips); GOAL joint-exploder / sparticle fixes for jak2 | `extract_level.cpp`, goal_src/jak2 | **re-extract jak3 v44; check whether jak3 needs the same GOAL fixes** |
| 86/87 | area prefetch: memory gate (32 MB free) **and** time gate (frame EMA ≤ 25 ms); printf-style `[pf]` logs | `Loader.cpp` | inherits; jak3 has no seed table (learned graph only) |
| 88 | BCn PBO ring: **default OFF** (no gain) | `LoaderStages.cpp` | nothing (`gk_pbo.txt` opts in) |
| 89 | S3TC probe + bit-exact CPU BC1/BC3 decoder | `LoaderStages.cpp` | inherits |
| 90 | decode default; level-0-only upload; mips deferred to the mipq | `LoaderStages.cpp` `upload_bcn_texture` | inherits |
| 91 | `decode_level_bcn_to_rgba()` on the loader thread after unpack and in `load_common` | `Loader.cpp`, `LoaderStages.cpp/.h` | inherits; check the `[texfmt] FIX 91 loader-thread decode` line |
| 92a | CPU floor of 1785 MHz via clkrst, re-checked every 2 s; GPU untouched; never lowers a higher clock | `game/switch/platform.cpp` `switch_clock_tick` | inherits; kill switch `gk_noclk.txt` |
| 92b | `kTodRecomputeBudget` 3 → 1 | `GfxDrawStats.h` | inherits |
| 93a | BlitDisplays no longer clears fb 0 at frame start; the clear is deferred to the final blit (the jak2 version of FIX 13) | `BlitDisplays.cpp`, `BucketRenderer.h`, `OpenGLRenderer.cpp` | inherits (jak3 also uses BlitDisplays) |
| 93b | clouds (`handle_clouds_and_fog`) rebuilt every other frame | `TextureAnimator.cpp` | inherits if jak3 sends CLOUDS anims |
| 93c | ocean envmap texture rebuilt every other frame; the DMA is still consumed | `ocean/OceanTexture.cpp/.h` | inherits (jak3 uses `render_jak2`) |

## 3. Override files on the SD root (`sdmc:/`)

| File | Effect |
|---|---|
| `gk_nodecode.txt` | keep BCn upload (turns off FIX 90/91; slow, for A/B only) |
| `gk_pbo.txt` / `gk_nopbo.txt` | FIX 88 PBO ring on / hard off |
| `gk_noclk.txt` | turn off the FIX 92 CPU floor |
| `switch/jakN/gk_no_pf.txt` | turn off area prefetch |
| `gk_decode.txt` | obsolete since FIX 90 |

**Trap (FIX 88):** a leftover kill-switch file silently disabled a fix for a whole test session. Always check the SD root for `gk_*.txt` before judging a build, and grep the log for the fix's "active" line.

## 4. Diagnostics checklist for the first Jak 3 session

- `gk_stdout.txt`: look for `[texfmt] FIX 90 ...` and `[texfmt] FIX 91 loader-thread decode: N BCn textures -> RGBA in X ms`. The `[loader] tex stage: N textures, upload X ms` line should show about 1–2.5 ms per texture.
- `gk_run_log.txt`: `[clk] FIX 92 CPU a -> 1785 MHz`. If that line repeats **every 2 s**, sys-clk is fighting the floor (seen on jak2 f93). Set the sys-clk profile to at least 1785, or remove it.
- Hold **R3+Minus** in a heavy area to get `[fps]` (wait_dma / render / swap), `[buckets]` and `[tod]`. jak2 was render-thread bound at 32 ms before FIX 92/93.
- For silent session ends, check `atmosphere/erpt_reports` (GPU faults) as well as `gk_fatal.txt`.

## 5. Things that were tried and must NOT be repeated

- Burst loader budgets during gameplay cause slow motion (FIX 75, 46b, 68). Starving the loader means areas never finish (FIX 38). Keep the existing tier table.
- Making the loader never exceed the frame's spare time was **rejected by the user**, because areas would fill in later.
- Using FastLoad CPU boost during gameplay is wrong: it clamps the GPU to minimum. Use it only in blackouts (FIX 71).
- A GOAL rebuild without `--instruction-set arm64` produces x86 code, and the kernel crashes at boot.
- Never deploy a v44 NRO with v43 fr3 files, or the reverse.

## 6. Open ideas (not done yet, as of FIX 93)

See the end of `CLINE_HANDOFF.md` for the current load-speed proposals. These cover the blackout budget, time-capping the mip drain, recycling GL textures, and a shared GL context on the loader thread.
