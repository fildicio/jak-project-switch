# jak2 + jak3 rollout plan — bring the jak1 fixes over (AI-assisted)

Written 2026-10-02 from:
- the Cline session `1790876325372_guxng`. Its "write to do list left for jak 2 and 3" request was never answered: the session hit its usage limit.
- the Copilot sessions that did FIX 76f–78.

The goal is the user's standing rule: **a perfect, locked 30 fps at 540p upscaled.** New areas should already be loaded when you get there, with no visible stream-in, no fps dips, and no slow motion.

Read first: `CLINE_HANDOFF.md` (FIX 74 → 78 sections), `STEP7_COMPRESSED_TEXTURES_DESIGN.md` §8, `AGENTS.md`.

---

## 1. Where things stand

| | jak1 | jak2 | jak3 |
|---|---|---|---|
| Live NRO | FIX 77 (`59dbea88`) | F73c (`3de12059…`) | F73c (`eacc6306…`) |
| fr3 format | v44 BCn (BC1/BC3 + file mips) | v43 RGBA | v43 RGBA |
| FSR 540→720 / 720→1080 (F73c) | yes | yes | yes |
| CPU boost on blackout loads (F71b) | yes | yes | yes |
| Area prefetch (FIX 76…76f) | yes | **no** | **no** |
| BCn upload path (FIX 74/74b/74c) | yes | **no** | **no** |
| `[goal]` profiler (FIX 77) | yes | **no** (NRO-only, free) | **no** |
| GOAL fixes (76f exploder, 78 particles) | yes | n/a / to port | n/a / to port |

**The hard coupling:** `TFRAG3_VERSION` is now 44.
- A jak2/jak3 NRO built from HEAD **refuses the v43 fr3 files** already on the card, and the old NROs refuse v44 files.
- So for each game, the **NRO and all its fr3 files must be deployed together.** No NRO-only update of jak2/jak3 is possible any more.

## 2. What the user learned the hard way (do not repeat)

- **Never trade fps for load speed during play.** FIX 75 (a burst budget during playable streaming) was rejected as "10 fps on every bridge". All extra load work must fit inside the frame's vsync slack, or happen during a black screen.
- **Prefetch must never cost a frame.** This needed f76 → f76e:
  - cache protection from the recycler
  - a start guard based on `failed_allocations()`
  - a purge that keeps caches
  - a dispatch cap of 1
  - the pf-lean 2 ms budget
  - miss backoff and big-texture deferral
  
  All of that is C++ and comes to jak2/3 for free with the NRO. Only the prediction table is per-game.
- **Never create a runtime `std::thread`** (F71: `_exit(1)` at boot).
- **PBO (F69) and core pinning (F70) are rejected or neutral.** Don't retry them.
- **Warps and teleports already load everything behind the black screen.** `update_blocking` sweeps every desired level before the fade lifts. The terrible Geyser Rock fps was the prefetch plus the particle log spam, not level streaming.
- **Any GOAL `format 0` that fires per frame or per particle is a frame killer on Switch.** It goes to `data/log/jakN.*.log` with an fflush per line (FIX 78: 13k lines → 40–130 ms frames). Always grep that log, not only `gk_stdout.txt`.
- Build the games **one at a time**. Deploy with `.bak` rotation, md5-verify, `sync`, and delete `._*` inside the game folder.
- Commits end with `(AI-assisted)` plus the Copilot co-author trailer. Never open PRs or issues.

## 3. Fix inventory → what jak2/jak3 need

| Fix | What | Where | jak2/jak3 action |
|---|---|---|---|
| FIX 74 | BC1/BC3 fr3 textures, mips in file | extractor + `LoaderStages.cpp` | **Re-extract fr3** (step B). ⚠ See the TextureAnimator blocker below. |
| FIX 74b | Eye renderer always consumes pupil DMA | `EyeRenderer.cpp` | Free with NRO. Jak2/3 eyes use the same renderer, so test cutscene close-ups. |
| FIX 74c | `glTexStorage2D` + `CompressedTexSubImage2D` | `LoaderStages.cpp` | Free with NRO. |
| FIX 76–76e | Area prefetch, hardened | `Loader.cpp` | Free with NRO, but **auto-enabled** (learned graph) and has **no static table** for jak2/3. Decide in step D. |
| FIX 76f (loader) | No prefetch on jak1 islands | `Loader.cpp` | Write the jak2/3 equivalent: places only reached by warp, elevator or cutscene. Step D. |
| FIX 76f (goalc) | `make_texture(..., true, false)` | `goalc/build_level/jak2,jak3` | Already done for all games. |
| FIX 76f (GOAL) | joint-exploder recursion cap | `joint-exploder.gc` | jak2/3 use `adjust-bbox-for-limits` with a `probeless?` stop. Likely safe, but add the same depth cap defensively (step C). |
| FIX 77 | `[goal]` per-process profiler | `kmachine.cpp` (common) | Free with NRO: jak2/3 `with-profiler` and `pc-prof` events already exist. Use it for every "this area is slow" report. |
| FIX 78 | Silent, bigger particle launch queue | `sparticle-launcher.gc` | jak2/3 queue is already 256, but the overflow still does `format 0`. Make it silent (step C). |

### ⚠ Blocker found while writing this: TextureAnimator needs RGBA pixels from GAME.fr3

- jak1 has no TextureAnimator. jak2/jak3 do (`OpenGLRenderer.cpp:465`).
- At startup it reads **CPU pixels** of common-level textures: `TextureAnimator.cpp:685–712` (`dtex->data.data()`, `stex->data.data()` via `tex_by_name(m_common_level, …)`).
- `apply_bcn_compression` (`decompiler/level_extractor/extract_level.cpp`) clears `data` for every non-sky texture.
- So extracting jak2/jak3 as-is would hand the animator empty buffers: a crash, or garbage textures, at boot.

**Fix before extracting:** keep those textures RGBA. Options, from simplest:
1. Skip BCn for the **common level (GAME.fr3)** on jak2/jak3. It is loaded once at boot, so it doesn't matter for streaming.
2. Add a name list of every `tex_name` / layer `tex_name` referenced by the TextureAnimator definitions to the exclusion, next to `sky_texture()`.

Then grep the renderer again for other CPU readers of `tfrag3::Texture::data` / `get_data_ptr()` used only by jak2/3. Today these are SkyBlendCPU (already excluded via "sky"), Eye (fixed), and TextureAnimator.

## 4. Step-by-step (do jak2 fully, user-test it, then jak3)

### A. Backup
- Full card data is already at `~/Desktop/jak bakcups/2026-10-01-pre-FIX74-full/`. Verify it still matches the card (jak2/jak3 haven't changed since).
- Rotate `jakN.nro` → `Jak N.fNN.bak` when deploying.

### B. Re-extract fr3 with BCn (after the TextureAnimator fix)
```
cmake --build build-host --target decompiler -j 8
./build-host/decompiler/decompiler ./decompiler/config/jak2/jak2_config.jsonc ./iso_data ./decompiler_out \
  --version ntsc_v1 --config-override '{"decompile_code": false, "levels_extract": true, "allowed_objects": []}' \
  > /tmp/jak2_extract_bcn.log 2>&1
grep 'FIX 74 BCn' /tmp/jak2_extract_bcn.log
```
- jak2 iso is SCUS-97265 → `ntsc_v1`. jak3 is `ntsc_v1` too (only one version folder).
- Expect about 1/3 of the old texture bytes, sky and animator textures as RGBA, and **GAME.fr3 showing RGBA** if option 1 was used.
- Output goes to `out/jakN/fr3` (jak2 148 files, jak3 274).
- Desktop sanity check: `build-host/game/gk --game jak2 -boot -fakeiso`. Look for `[texfmt] FIX 74 BCn compressed texture path active`, then:
  - no black or purple textures
  - an animated-texture area: jak2 city/dark eco/skull gems; jak3 wasteland/sky
  - a cutscene with eye close-ups

### C. GOAL changes (then `(make-group "iso")` for that game, diff CGOs against the card)
1. `goal_src/jakN/engine/gfx/sprite/particles/sparticle-launcher.gc` `sp-queue-launch`: remove the `format 0 "ERROR: ... queue is full"` (keep the `return 0`).
2. Optional, defensive: a depth cap of 32 in `adjust-bbox-for-limits` in `goal_src/jakN/engine/anim/joint-exploder.gc`, like jak1's `*joint-exploder-split-depth*`.
3. Before deploying, grep the old `data/log/jakN.*.log` files on the card for any other repeated message: `grep -v '^\[' | sort | uniq -c | sort -rn | head`. Silence any per-frame spam the same way.
4. Deploy only the CGOs whose code changed (ENGINE/GAME). Same-size DGO diffs are rebuild noise.

### D. Prefetch for jak2/jak3 (C++, `Loader.cpp`)
- As of HEAD, prefetch **turns on automatically** in jak2/3 with the learned-transition graph only. Gating is `m_game_version == GameVersion::Jak1` for the static table and the island exclusion.
- It only starts when `loaded + 1 < min(max_live_levels(), 5)` and fewer than 2 caches exist. jak2/jak3 often have 3–4+ levels live in the city, so it may rarely fire there. That is fine.
- **Recommended phasing:**
  1. First jak2 release: BCn + NRO with prefetch **disabled** for jak2/jak3 (early return when not Jak1). This isolates the BCn result, which is the design's §6 rule: one change at a time.
  2. Second release: enable prefetch with a small static table for the known pain points the user reported:
     - jak2 leaving Haven City (city ↔ forest/sewer/palace edges)
     - jak3 Spargus ↔ desert, Haven sections
     
     Build the table from the `[loader]` "ready in" lines in the user's logs (which levels follow which).
  3. Add the jak2/jak3 equivalent of the FIX 76f island exclusion for areas only entered by elevator, warp or cutscene, where a prefetch can never be used.
- Keep the jak3 FIX 63 GPU-OOM history in mind. Prefetch only takes memory when `failed_allocations()==0`, and BCn cuts VRAM by about 3x, which helps here.

### E. Build and deploy (jak2 first)
```
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  env SWITCH_GAME=jak2 BUILD_DIR=/work/build-switch-jak2 JOBS=2 bash scripts/build-switch.sh > build-switch-jak2-fNN.log
```
- Deploy as `sdmc:/switch/jak2/jak2.nro` **together with** all `out/jak2/fr3/*.fr3` → `sdmc:/switch/jak2/data/out/jak2/fr3/`, plus any changed CGO.
- md5-verify every file. Copy the NRO to `~/Desktop/jak bakcups/jak2.fNN.nro`.

### F. Hardware test (user) — what success looks like in the logs
- `gk_stdout.txt`: `[texfmt] FIX 74 BCn compressed texture path active`, `FIX 74c glTexStorage2D` probe OK.
- `tex stage` upload ms per level about 3x lower than F73c; no mipgen; `ready in` seconds down.
- `gk_run_log.txt`: `[cam] HITCH` count and max-in-15 s, on the same route as the F73c baseline (city exit, forest, desert).
- `data/log/jakN.*.log`: no repeated ERROR spam.
- Boot and cutscenes: no crash at the first Jak/Daxter close-up (the FIX 74b lesson).
- For any slow spot: R3+Minus, then read `[goal]` (GOAL-bound) versus `[fps]` render / `[buckets]` (GPU-bound).

### G. Rollback
- Old NRO `Jak N.fNN.bak` → `jakN.nro`, **and** restore the v43 fr3 files from `2026-10-01-pre-FIX74-full/`.
- Or re-extract with `--config-override '{"bcn_textures": false}'` and rebuild.

### H. Then jak3, same steps
- jak3-specific notes:
  - Haven City is the most upload-heavy area, so it is the biggest expected win.
  - The prebot boss was only passable at 60 fps (FIX 63 era). Re-test it.
  - jak3 has 274 fr3 files; the extract and copy take longer.

## 5. Order of work, short version
1. Fix the TextureAnimator RGBA blocker in the extractor.
2. jak2: extract → desktop check → GOAL particle silence → NRO (prefetch off) → deploy NRO + fr3 → user test.
3. jak3: same.
4. jak2/jak3: enable prefetch with per-game tables and exclusions → user test.
5. Update `CLINE_HANDOFF.md` after every deploy (card table, md5s, rollbacks).
6. Logging fix (§6.1) can be done at any time. It is cheap and helps jak1 too.

## 6. Further improvements (recommended, all games)
In order of value. Each one is a separate change and a separate user test.

1. **Logging: stop the fflush on every line (C++, all 3 games).** GOAL `format 0` goes through `lg::print` to `sdmc:/switch/jakN/data/log/jakN.*.log` and fflushes each line. That was half of the Geyser Rock cost (13k lines per session). On Switch:
   - buffer the file writes (flush on a timer, on error or warn level, and at exit), and/or
   - drop identical repeated lines with a "repeated N times" counter.
   - Files: `common/log/log.cpp`, gated by `#ifdef __SWITCH__`. Make sure crash logs still get flushed.
2. **Audit the logs for other repeated messages.** Grep every `data/log/jak*.log` from real sessions for lines repeated more than about 100 times. Silence or fix them at the source, the way FIX 78 did. Geyser Rock was just the worst one found so far.
3. **The jak2/3 TextureAnimator is a CPU cost jak1 never had.** It runs every frame for animated textures (water, lava, Dark Eco, and so on). If jak2 is GOAL-light but its frame time is high, profile the animator first. Possible fixes are skipping off-screen animations or updating them at a lower rate.
4. **Lock jak2/3 to 540p and 30 fps from the start.** Their game logic is heavier than jak1's, so expect more `[goal]` tuning per area, using the FIX 77 profiler.

Not worth it (measured or rejected): deko3d rewrite, BC7/ASTC textures, extra runtime threads, core pinning, PBO uploads.
