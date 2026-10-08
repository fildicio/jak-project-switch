# Jak 2 → Jak 3 carry-over: compression, loading and fps work (AI-assisted)

Written 2026-10-08. Updated after **FIX 95**, which the user confirmed on hardware: "jak 2 runs almost perfectly". The jak2 reference build is **f95** (NRO md5 `b44c85e8eeaadb622eecc4c13f26919d`, commit `ee2018953`). This document covers everything done on jak2 from FIX 84 to FIX 95, what the Jak 3 rollout needs, and the traps hit along the way. The detailed per-fix notes are in `CLINE_HANDOFF.md`. The older jak1 → jak2/3 plan is in `JAK2_JAK3_ROLLOUT_PLAN.md`.

> **EXECUTED 2026-10-08 (AI-assisted): rollout complete.** Both GOAL ports were needed and landed; jak3 re-extracted at v44
> (GAME.fr3 RGBA, 876→701 MB); GOAL rebuilt with the arm64 flag (only GAME.CGO + TSZ.DGO changed); NRO `ec1526ba179fb7d10a7c158863edc8cd`
> deployed with all 274 fr3 + the two CGOs, everything md5/cmp-verified. Rollback exists this time: card `Jak 3.f83.bak` +
> `~/Desktop/jak bakcups/jak3-v43-fr3/`. Details: `CLINE_HANDOFF.md` "Jak 3 rolled out to FIX 95". Awaiting the user's hardware test (§4 checklist).

## 0. TL;DR for Jak 3

1. **Almost all of this is shared C++.** It is gated on `__SWITCH__`, not on the game version, so a jak3 NRO built from HEAD inherits it all automatically. The only jak2-specific piece is the prefetch seed table (`kJak2LevelAdjacency`). Jak3 stayed learned-graph-only **until FIX 96** (2026-10-08), which added `kJak3LevelAdjacency` built from the player's observed transitions plus jak3's own prefetch EMA line.
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
   - The render trims (FIX 93) apply.
   - Frozen loads build mips while the game is frozen (FIX 94b).
   - Gameplay streaming is sized from each frame's measured free time (FIX 95).
   - **The game never changes clocks** (FIX 95). The user's horizon-oc profile owns them.
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
| 92a | CPU floor of 1785 MHz via clkrst. **Disabled by FIX 95** (`switch_clock_tick` is a no-op): it fought horizon-oc, which re-lowered the CPU every ~300 ms | `game/switch/platform.cpp` | nothing; do NOT re-enable |
| 92b | `kTodRecomputeBudget` 3 → 1 | `GfxDrawStats.h` | inherits |
| 93a | BlitDisplays no longer clears fb 0 at frame start; the clear is deferred to the final blit (the jak2 version of FIX 13) | `BlitDisplays.cpp`, `BucketRenderer.h`, `OpenGLRenderer.cpp` | inherits (jak3 also uses BlitDisplays) |
| 93b | clouds (`handle_clouds_and_fog`) rebuilt every other frame | `TextureAnimator.cpp` | inherits if jak3 sends CLOUDS anims |
| 93c | ocean envmap texture rebuilt every other frame; the DMA is still consumed | `ocean/OceanTexture.cpp/.h` | inherits (jak3 uses `render_jak2`) |
| 94 | `blocking` tier for the frozen `update_blocking` sweep (40 ms, 32 MB, 64 dispatches); `mipq_process(rate, max_ms)` time cap; idle mip drain capped at 2/4 ms; stage clamp follows `dispatch_cap` | `Loader.cpp`, `LoaderStages.cpp/.h` | inherits |
| 94b | partial revert of 94: the sweep **builds mips** (rate 16, 10 ms cap) instead of deferring them. Deferring left a ~1460-chain backlog that cost 6–10 ms per gameplay frame. Blackout tier back to 12 ms / 4 MB; no blackout burst | `Loader.cpp` | inherits |
| 95a | **frame-free-time streaming budget.** `OpenGLRenderer` publishes `g_switch_frame_free_ms = ph_loader + ph_pcrtc` (pcrtc is mostly the swapchain-acquire wait). The loader takes the min over 4 frames minus 4 ms and uses it to clamp the gameplay streaming line (1 ms to the tier value). The mip drain gets what is left; with less than 0.8 ms left it does 1 chain every 4th frame. The stage still dispatches at least 1 texture per frame, so loading never stalls. **This is what removed the city slow motion** | `OpenGLRenderer.cpp`, `Loader.cpp` | inherits; check the `[loader] FIX 95 free` lines |
| 95b | the game never changes clocks: FIX 71 FastLoad boost and the FIX 92 floor are both no-ops | `game/switch/platform.cpp` | inherits |

## 3. Override files on the SD root (`sdmc:/`)

| File | Effect |
|---|---|
| `gk_nodecode.txt` | keep BCn upload (turns off FIX 90/91; slow, for A/B only) |
| `gk_pbo.txt` / `gk_nopbo.txt` | FIX 88 PBO ring on / hard off |
| `gk_noclk.txt` | obsolete since FIX 95 (the game no longer touches clocks) |
| `switch/jakN/gk_no_pf.txt` | turn off area prefetch |
| `gk_decode.txt` | obsolete since FIX 90 |

**Trap (FIX 88):** a leftover kill-switch file silently disabled a fix for a whole test session. Always check the SD root for `gk_*.txt` before judging a build, and grep the log for the fix's "active" line.

## 4. Diagnostics checklist for the first Jak 3 session

- `gk_stdout.txt`: look for `[texfmt] FIX 90 ...` and `[texfmt] FIX 91 loader-thread decode: N BCn textures -> RGBA in X ms`. The `[loader] tex stage: N textures, upload X ms` line should show about 1–2.5 ms per texture.
- `gk_stdout.txt` while streaming in gameplay: `[loader] FIX 95 free X ms -> stream budget Y ms (tier ...)`. X should be about 8–11 ms in a busy area. If X stays below 5, the frame has no slack: lower the resolution or raise the GPU clock in the user's profile.
- `Loader::update slow setup: N ms` during gameplay should now stay within the free time, not 10–15 ms as in f94b.
- No `[clk]` or `[boost]` lines are expected any more (FIX 95).
- Hold **R3+Minus** in a heavy area to get `[fps]` (wait_dma / render / swap), `[buckets]` and `[tod]`. jak2 was render-thread bound at 32 ms before FIX 92/93.
- For silent session ends, check `atmosphere/erpt_reports` (GPU faults) as well as `gk_fatal.txt`.

## 5. Things that were tried and must NOT be repeated

- Burst loader budgets during gameplay cause slow motion (FIX 75, 46b, 68). Starving the loader means areas never finish (FIX 38). Keep the existing tier table.
- The first "loader never exceeds spare time" proposal was rejected by the user because areas would fill in later. FIX 95 is the accepted version: the budget is the **measured** free time (often about 7 ms), with a 1-texture floor per frame. Loads stayed fast and the slowdown went away. Do not go back to fixed 7–8 ms gameplay tiers.
- Do not defer **all** mips out of a frozen load (FIX 94): the backlog lands in gameplay.
- Do not raise the blackout tier above 12 ms (FIX 94): fade frames grew to 150–250 ms and the load was no shorter.
- Do not set clocks from the game (FIX 92/95): it fights horizon-oc or sys-clk. The user picks the clocks; only advise them.
- Using FastLoad CPU boost during gameplay is wrong: it clamps the GPU to minimum. Use it only in blackouts (FIX 71).
- A GOAL rebuild without `--instruction-set arm64` produces x86 code, and the kernel crashes at boot.
- Never deploy a v44 NRO with v43 fr3 files, or the reverse.

## 5b. FIX 97-98 (jak3-first, apply to jak1/jak2 at next build)
- FIX 97: per-load soundbank accumulator (jak3/jakx overlord only).
- FIX 98: heap-headroom guard in `Loader::heap_guard()` + bounded crash-handler stack scan.
  nouveau never fails `glBufferData`, so retired levels must be recycled on real heap headroom
  (`heap_headroom_bytes()`), not on failed allocations. Shared code: jak1/jak2 pick it up on their
  next rebuild; watch the `| heap NNNMB` telemetry.

## 6. Recommended user settings (jak2 f95)

- Handheld: **432p internal with FSR upscale**. Try 540p if the city holds 30 fps.
- Docked: 720p or higher.
- Clocks are the user's choice in their horizon-oc profile. The jak2 profile appears to be `056600FFF1156000` (CPU 1428 / GPU 460 / RAM 2000). The log showed GPU 460 → 307. GPU 614 is the most useful raise if the user wants one. Never edit it for them.

## 7. Open ideas (as of FIX 95)

- Do NOT try a second-thread GL context: devkitPro Mesa is 20.1, and nouveau is not thread-safe there.

- Do NOT recycle GL textures: FIX 88 showed the ~1 ms per-call cost stays even with storage preallocated.

## 8. Jak 3 rollout order

1. Back up the jak3 v43 fr3 files and the current jak3 NRO (`Jak 3.fNN.bak`).
2. Re-extract jak3 at v44 (BCn). Rebuild GOAL with `--instruction-set arm64`, and check whether the jak2 FIX 85 GOAL fixes are needed.
3. Build the jak3 NRO from HEAD. Deploy the NRO and all fr3 files together, with md5 checks.
4. Clean the SD root of `gk_*.txt` kill switches.
5. First session: check the section 4 log lines (FIX 90/91 decode, FIX 95 free), and look for OOM. Jak 3 levels are bigger and use RGBA VRAM.
6. Jak 3 specific: it holds up to 11 levels (FIX 46). If the FIX 95 free time is lower than jak2's, start at 432p and tune from there.

