# What is left to convert to deko3d (AI-assisted)

Rule: everything below is done through the **dkgl wrapper** (see `AI_ROLES.md`,
"Wrapper rule"). Status: ⏳ todo · 🟡 code done, hardware test pending · ✅ done.

## Already on deko3d (jak1)

| Part | How |
|---|---|
| tfrag (+ alpha, ice, dirt), tie (+ wind, envmap), shrub, sky, sky-blend | native |
| eyes, sprite, direct (HUD, text, debug) | native |
| texture uploads, level loader | native |
| ocean texture + mid + near + far | dkgl |
| generic (distant mountains, 10 buckets), shadow, depth-cue | dkgl |
| merc (characters, faces, fade) + emerc shine (GL `Merc2`, 8 buckets) | dkgl (#19: eyes still black on HW 2026-10-30, gated probe added, NRO rebuild pending) |
| screen fade to black (pmode_alp) + loader "finish before fade-in" | dkgl + loader (confirmed on hardware 2026-10-27) |

## 1. Open bugs (jak1)

| # | Bug | Status | Next step |
|---|---|---|---|
| 1 | Black line at the sea horizon | ✅ | Fixed by running the far ocean through dkgl (NRO `319f35f7`, confirmed on hardware 2026-10-27) |
| 2 | Jak / objects disappear in busy scenes | ✅ | Merc limit raised 256 → 1024 (GL value), confirmed on hardware 2026-10-27 |
| 3 | Black boxes / things disappearing while a level loads | ✅ | `dk_set_pmode_alp` was a stub (no fade-to-black, no "finish loads before fade-in"). Fixed in NRO `e73e23e2`, confirmed on hardware 2026-10-27 |
| 4 | Dark vertical stripe in the sky (IMG_0108) | ✅ | Same cause as #3 (photo taken during a load). Fixed in NRO `e73e23e2`, confirmed on hardware 2026-10-27 |
| 16 | Jak's shadow looks weird; dark shade on top of Jak and Daxter (IMG_0122) | 🟡 | Cause found: draw order. GL draws the shadow (bucket 47) BEFORE the pris/water merc buckets 49 52 55 58 61 (Jak and Daxter are 55), so the shadow's darkening quad never covers them. dk recorded all 8 merc buckets before 47. Now 49..62 are recorded after the shadow in GL order (`dk_renderer.cpp` record_tfrag_and_shrub, NRO `ce1d93b3`). The native merc path (`gk_dkgl_nativemerc.txt`) still draws all merc first. Needs a hardware look |
| 17 | Geyser Rock fps drop when Jak smashes the wooden training target | 🟡 | Log `20261027-2121`: during the smash the `sprite` pass averages 5.5 ms of GPU (worst frame 31.9 ms) with a normal sprite count (~1.5k, 21 draws). Sprite vertex+fragment shaders, sizes and alpha test match the PC renderer exactly, textures are block-linear, the game does not touch clocks: it is pure overdraw (fill rate) of big smash particles. Fix 1 (NRO `a08dd132`): sprites that do not write depth use `sprite3_ez.frag` (`layout(early_fragment_tests)`), so particle pixels hidden behind the world are rejected before shading/blending; identical picture. Log line `sprite frame ... early_z=N`. Off switch: `gk_dk_spritelatez.txt`. Next option if still slow: colour compression (`DkImageFlags_HwCompression`) on the frame target (needs the 3D splash + frame-grab check) Early-Z (`a08dd132`) did not help (log `20261027-2143`: sprite 4.86 ms avg, worst 38.7 ms). Real fix: colour compression (`HwCompression`) on the frame target, which cuts the read+write bandwidth of every blended pixel (NRO `2f0eef2f`). Opt-out: `gk_dk_nofbcomp.txt` Compression did NOT help (log `20261027-2159`: sprite still 4.6 ms avg), so it is blend overdraw, not bandwidth: the smash stacks screen-sized dust sprites. GL has the same scale limits; desktop GPUs just fill faster. Fix: near-camera fade in `sprite3.vert.glsl` -- 2D world sprites fade as their screen box passes ~1/3 of the screen and are skipped once it covers the whole screen (NRO `560278b0`). Opt-out: `gk_dk_nospritefade.txt`. Check: `sprite` GPU ms during the smash |
| 18 | Language: voices stay English | ✅ (user setting) | Log `20261027-2121`: `pc-settings.gc` still has `game-language 0`, so the option was never confirmed. In Options → Game → Language pick Italiano with left/right and **press A**; X/Y (square/triangle) cancel and restore the old value. Voices follow this option (`VAGWAD.ITA` is present) |
| 19 | Characters: eyes white/green, mouth not drawn/moving | ✅ (HW 2026-10-30, NRO `875ef956`: NPC eyes drawn) | Mouth: fixed by GL `Merc2` through dkgl (NRO `9f1eb189`). Eyes (IMG_0121/0122: white or green blobs): the native eye pass rendered fine (bad=0) but the hand-off to Merc2 was wrong. Now the GL `EyeRenderer` (bucket 54) runs through dkgl too, Merc2 samples its textures, and dkgl puts a fragment barrier after it (NRO `0b04922c`). Opt-out for an A/B: `gk_dkgl_nativeeyes.txt`. Eyes then rendered BLACK on `a08dd132`: the EyeRenderer needs 40 textures + 40 FBOs, and the dkgl object tables held only 64 each, so the later eyes got id 0. Tables raised to 256 (NRO `41bbc824`). Still black on `2f0eef2f`. Host test `ScriptedEyeRendererTwoEyes` shows dkgl emits the eye draws correctly, so the cause is the texture lookup: the Switch replays ALL of a frame's texture uploads before dkgl runs, so the eye bucket saw end-of-frame VRAM (pages 57/60/65 replace the iris/pupil/lid). First fix (NRO `c290f3f7`): a VRAM copy for bucket 54 only. Replaced (NRO `ce1d93b3`) by running every dkgl bucket inside the upload replay (`dkgl::capture_begin/capture_until/capture_end`, called from `dk_upload_bucket_hook`), so eyes, Merc2 and generic look textures up in VRAM exactly as GL does at their bucket. Log: `[dkgl] ... eye draws N | interleaved M` (N > 0 = eyes drew, M = frames captured that way). HW 2026-10-30 (NRO `ce1d93b3` confirmed on the SD, md5 + `interleaved` label): eyes still BLACK; session healthy (13.5 eye draws/frame replayed, 0 skipped, dma mismatch 0, no fatal). Full static audit of the chain (pool seam id → dktexgpu render target, FBO target switch + 128×128 viewport at draw time, scissor reset, post-54 barrier, Merc2 `lookup_eye_texture_hash` binding the same id) found no break; note `glClearBufferfv` is NOT implemented in dkgl, so the GL red debug clear never runs — a never-written eye image is black, not red. Gated probe added (AI-assisted): create `gk_dkgl_eyeprobe.txt` in `/switch/jak1/` on the SD → after bucket 54 the NRO copies eye slot 0 to CPU and logs `[dkgl] eye probe #N: avg rgba r,g,b,a | black n/16384 | r0/r32/r96 …` every ~5 s, 5 rounds. all-black ⇒ the strips never land in the eye image (eye-pass bug: RT bind / vertex stream); content present ⇒ Merc2 sampling is the suspect. NRO `e54a0396` built in docker (`devkitpro/devkita64`, `scripts/ai/check-switch.sh`), deployed to SD 2026-10-30 with the probe gate file `gk_dkgl_eyeprobe.txt` already created (sd-sync keeps it armed) — next SD trip reads the `[dkgl] eye probe #N` lines. **HW verdict 2026-10-30 (log `20261030-1722`)**: slot 0 = Jak HAS content (`avg rgba 162,165,173,233 | black 789/16384` from round 3 on; rounds 1-2 all-black = face off screen) ⇒ the eye pass, the 128×128 RT and the id→image seam all work. User: only Jak + Daxter (early slots) have eyes, all NPCs black. `gk_stdout.txt` has ZERO `lookup eye failed` lines ⇒ Merc2's `lookup_eye_texture_hash` finds and binds a texture for every character — NPC slots match a hash but are (presumably) pristine black because `run_gpu` skips quads when `lookup_gpu_texture` misses the iris/pupil/lid source (Jak's sources load, NPC ones don't). Probe v2 (AI-assisted): copies ALL 40 slots in one frame and logs a per-slot map `[dkgl] eye probe #N: ##.… | # n + n . n x n | slot0 avg …` (`#` content, `+` partial, `.` black, `x` no image), and `EyeRenderer::run_gpu` logs `[dkgl] eye src slot N: iris MISS (tbp N) …` for missing source textures during the probe window. NPC slots `.` + `eye src … MISS` ⇒ texture-pool miss for those tbps (VRAM page replay); NPC slots `#` ⇒ Merc2 binding seam. **Root cause found (AI-assisted, no HW trip needed)**: jak1 `eye.gc` puts each level's NPC eyes in that level's pris-tex bucket (48 / 51, right after the page with their iris/pupil/lid); only Jak/Daxter use bucket 54. GL's `TextureUploadHandler` runs `EyeRenderer::handle_eye_dma2` when it meets that DMA (qwc 8); the Switch replay (`SpriteDk walk_tex_bucket`) only did the uploads, so NPC eye slots were never drawn (black). Note: jak1 Merc2 picks eyes by slot (`lookup_eye_texture(eye_id)`), not by hash. Fix: dkgl lists 48 / 51 run `PrisTexEyes` (TextureUploadHandler minus the uploads) inside the replay, recorded with 54 before every merc, same barrier. NRO `875ef956` (check-switch PASS). Check: NPC eyes (e.g. Samos, the farmer) drawn; `eye draws` in the `[dkgl]` line goes up near NPCs |
| 20 | Intro "Sony presents" screen: the dark fade covers only the 4:3 centre, the sides stay bright (IMG_0115) | ⏳ | The game itself is 16:9 (`aspect-ratio-auto? #t`, the window decides). Checked and ruled out: GOAL `blackout`/`letterbox` (full 512x224 sprites), the dkgl pmode fade (full-screen quad, full viewport) and the native direct renderer scissor (same maths as GL). Remaining suspect: the 2D/HUD sprite pass, which the PC port keeps at 4:3 on purpose (may be normal PC behaviour). A/B: boot once with `gk_nosprite.txt` (the `gk_opengl.txt` GL opt-out was removed 2026-10-30). 2026-10-30 user report: intro looks like "a 4:3 box with bands around a widescreen image" (old bug). **User confirmed: pillarbox, left+right bands only.** Saved `pc-settings.gc` has `(aspect-state aspect4x3 4 3 #t)` + `(letterbox #t)`: GOAL keeps the view scissor 4:3, so the world draws in the 4:3 centre and the dk present-clear shows at the sides. The dk path reports a 16:9 window (`set_native_window` 1280x720/1920x1080), so the fix is the setting itself, no code: **Options → Aspect Ratio 16:9 + Letterbox OFF (press A to confirm)** — equivalent to editing pc-settings.gc to `(aspect-state aspect16x9 16 9 #f)` / `(letterbox #f)`; I can patch the file on the SD at the next sync if the menu route doesn't stick. If 16:9 still pillarboxes after that, then `aspect-ratio-auto?` handling misparses and the fallback is a GOAL-side force-16:9 branch in `update-from-os` (needs a goalc rebuild + data redeploy) |

## 2. Missing features (jak1, via dkgl)

| # | Feature | What the GL side uses | Status |
|---|---|---|---|
| 5 | Shine on orbs / metal (merc envmap, "emerc") | GL `Merc2` runs through dkgl on the 8 merc buckets; only the `emerc` program is in the dkgl table, so normal merc stays native (MercDk) and only the envmap draws reach the GPU, replayed right after native merc. Adds `glBindBufferRange(GL_UNIFORM_BUFFER)` (bones, binding 1), Loader buffer ids via `set_external_buffer_lookup`, and a null guard for the missing EyeRenderer in `Merc2.cpp`. Opt-out: `gk_dkgl_noemerc.txt`. Seen working on hardware 2026-10-27. Since #19 the normal merc draws (`merc2` program) also run through dkgl | ✅ |
| 6 | Screen fade to black (cutscenes, loads) | GL `FullScreenDraw` + `solid_color` through dkgl (`record_blackout`, after the HUD) | ✅ |
| 7 | Brightness / contrast (PCRTC) | jak1: no-op (GOAL never calls it). Do it with jak3 | — |
| 8 | Dock / undock resize | works like GL: the swapchain stays at the boot size (GL FIX 73c) and the Switch scales the image when docked. A native-resolution resize is deferred (risky, costs GPU time) | ✅ same as GL |

## 3. Close the hardware checks

| # | Item | Status |
|---|---|---|
| 9 | T102 / T110: ocean hardware test (Sentinel Beach, 20 min, heap flat, fps unchanged) | ⏳ |
| 10 | T112 / T113: depth-cue + shadow hardware test | ✅ seen working on HW 2026-10-27 |

## 4. Release (jak1)

| # | Item | Status |
|---|---|---|
| 11 | Commit the pending work (fog fix, T112/T113, merc limit, far ocean via dkgl) | ✅ `815d00d6e`, PR #4 merged |
| 12 | deko3d on by default: jak1 boots on deko3d with no flag files; no OpenGL opt-out or boot fallback since 2026-10-30 (replaces `gk_dk3d`, `gk_dkload`, `gk_dktfrag`, `gk_dkshrub`, `gk_dktie`) | 🟡 needs a boot test with no flag files |
| 13 | Long soak test (1 h, several levels) + GL fallback still works | ⏳ |

## 5. jak2 and jak3 (all via dkgl)

| # | Item | Status |
|---|---|---|
| 14 | jak2: reuse the native deko3d renderers where they already work, run the remaining GL bucket renderers through dkgl, add the missing dkgl programs (`shadow2`, `tex_anim`, ...) and TextureAnimator support — full list in §7 | ⏳ |
| 15 | jak3: same as jak2, plus PCRTC brightness | ⏳ |

## 6. When is the migration finished?

The deko3d migration is done when all of these are true:

1. jak1: rows 3–13 above are ✅ (no open bugs, all features, deko3d on by default, soak test passed).
2. jak2 and jak3: rows 14–15 are ✅ and each game boots and plays one full level on hardware with deko3d.
3. Performance is the same as or better than GL (fps and frame ms in the `[dk]` log), measured per game.
4. jak1 has no OpenGL fallback any more (`gk_opengl.txt` removed 2026-10-30); jak2/jak3 still fall back to GL until they render right.

## 7. Jak 2 bring-up — missing list (2026-10-31, AI-assisted)

Rule update (user, 2026-10-31): the 2026-10-27 "all new work via dkgl" rule is relaxed for jak2 —
**if reusing an existing native deko3d renderer is quicker, reuse it.** dkgl is for the GL bucket
renderers that have no native equivalent. Sources: `OpenGLRenderer::init_bucket_renderers_jak2()`
(the full jak2 bucket list), the dkgl program table (`dkgl_uniform_table.inc`, 14 programs),
`dk_renderer.cpp` (jak1 bucket wiring) and the native renderer sources.

### 7.1 Reusable as-is (native or dkgl, already validated on jak1 — jak2 shares the code)

| jak2 renderer (bucket) | Reuse | Note |
|---|---|---|
| tfrag NORMAL + TRANS trees (`tfrag-l*-tfrag`, `tfrag-t-l*-alpha`) | `TFragmentDk` native | TRANS/LOWRES_TRANS already exist; jak2 bucket IDs + WATER tree still missing (7.2 #4) |
| tie NORMAL + wind (`tie-l*-tfrag`) | `TieDk` native | wind done (T76), same DMA wind block layout |
| etie NORMAL_ENVMAP (`etie-l*-tfrag`) | `TieDk` native | etie/etie_base dksh from T67/T68 |
| shrub (`shrub-l*-shrub`) | `ShrubDk` native | anim slots missing (7.2 #6) |
| particles (`Sprite3`) | `SpriteDk` native | same class as jak1 bucket 66 |
| eyes (jak2 `EyeRenderer`, held off-bucket, called by merc2) | dkgl `eye` | capture/interleave machinery is bucket-generic |
| merc2 (all `merc-l*-*` + `merc-lcom-*` buckets) + emerc | dkgl `merc2` / `emerc` | limits already at GL values |
| generic2 — every `gmerc-*` bucket, `effects` (Mode::LIGHTNING), `warp` (draws through Generic2, no own program) | dkgl `generic` | jak2 uses it far more than jak1 (perf check in 7.2 #8) |
| sky-draw (jak2 registers a plain `DirectRenderer`, NOT `SkyRenderer` — no SkyBlend in this fork) | dkgl `direct_basic_textured` | textures arrive via `tex-lcom-sky-pre` uploads |
| HUD / subtitle / screen-filter / debug2 / debug3 / progress-hack | dkgl `direct_basic` / `direct_basic_textured` / `solid_color` | `ProgressRenderer` activates no own shader |
| ocean mid-far + near | `dkocean` native | validate jak2 VU frames on HW (7.2 #8) |
| texture uploads + level loader | dk pool + `LoaderStages` RGBA path | jak2 fr3 is RGBA (BCn path not needed); `kDkFr3LevelCount` already has jak2 |
| pmode fade + blackout | dkgl (`record_blackout`, `dk_set_pmode_alp`) | shared, game-agnostic |

### 7.2 Missing (new work)

**Fastest sequence** (refined 2026-10-31 after a "is this quickest?" review):

- **Step 0 — log-first inventory (≈free):** boot jak2 through the existing dkgl capture with every
  unknown program left inert. The `[dkgl] program 'X' not in table` + `dk_gl_census` logs hand you
  the runtime-verified gap list in minutes and confirm the static analysis below before any code.
  **Landed 2026-10-31 (AI-assisted):** jak2 now boots through dkgl capture — `dkgl_frame.cpp`
  has a jak2 bucket table (83 buckets: ocean 6/310, 40× merc2, 39× generic2 incl. effects 315,
  sky-draw 5, screen-filter 321, blackout) built from `buckets.h` enum symbols and constructed
  with GL's `GET_BUCKET_ID_FOR_LIST` stride loops; `SpriteDk` walks all 47 jak2 upload buckets
  (sprite read off); `dk_renderer.cpp` gates it on `gk_dkload.txt` + `gk_dktfrag.txt` in
  `sdmc:/switch/jak2/` and records via `record_jak2_dkgl` (dkgl buckets in GL order + blackout).
  The jak2 `EyeRenderer` is wired through `SharedRenderState::eye_renderer` like GL. Still inert
  by design: shadow 195/314 (shadow2), warp 317, particles 313, progress/debug (natives later).
  Tables verified against the GL registrations by script (walk ≡ upload set 47/47, merc 40/40,
  generic 39/39). **Note:** real jak2 bucket ids differ from the old manual estimates —
  `MAX_BUCKETS` is 327, `SHADOW`=195, `SHADOW2`=314, `EFFECTS`=315, `WARP`=317, `OCEAN_NEAR`=310;
  always take ids from the enum, not from prose.
- **Phase 1 (first picture):** #1 wiring + #7 flags. Write #1 **table-driven** — mirror GL's
  `GET_BUCKET_ID_FOR_LIST` loops (chain start + stride × 5 levels + `lcom` list), not ~150
  hand-enumerated buckets. Natives + existing dkgl programs then give world, characters, HUD,
  merc, generic, sky, eyes. Missing: shadows, water trees, alpha ties, animated textures.
  **dkgl half done (see Step 0); native halves landed 2026-10-31 (AI-assisted):**
  `TFragmentDk`/`TieDk`/`ShrubDk`/`MercDk`/`DirectDk` each got a jak2 table (stride-built
  from `buckets.h` symbols) + `set_game` pick; `SpriteDk` was already parameterized.
  `record_jak2_dkgl` now splits the native world passes in at their GL bucket positions
  (`kJak2NativeSplits`: per level TFRAG_L→TIE_L→ETIE_L before MERC_L tfrag, SHRUB_L before
  MERC_L shrub, TFRAG_T_L (TRANS) before MERC_L alpha), driven by `DrawCmd::range` +
  `record_pass` slices in `dk_tfrag_record.cpp`. jak2 deltas handled: no CALL chain head,
  fog from the vis bucket (`BUCKET_2` vif0, like `VisDataHandler`), tie proto-vis packet
  instead of WindWork (wind draws at rest, like GL's zero WindWork), envmap tint /128
  without the jak1 ×2. Debug off-switch: `gk_dkgl_noworld.txt` (dkgl-only world).
  Still inert on jak2: `MercDk`/`DirectDk` (dkgl merc2/direct own those buckets, §7.1),
  particles (phase 3), shadow2/warp/tfrag-WATER/alpha-ties (phase 2). HW test pending.
- **Phase 2 (days):** #2 `shadow2` (real port — it is NOT an alias of `shadow`: world-space
  perspective/fog/clear-mode path, 0.5× output vs 2×; verified by diff), #5 tie categories,
  #4 tfrag WATER.
- **Phase 3 (only multi-day subsystem, last):** #3 TextureAnimator + #6 shrub slots (rides on
  #3's plumbing). Deferring it does not block a playable-on-dk milestone — Haven City ads just
  render as static first-frame textures.
- #8/#9 are continuous (measure + read logs each phase), #10 runs in parallel (wiring, shadow2
  and the tex_anim shaders can be written and host-tested before extraction lands — dkgl
  compiles in the host tests).

Why not the alternatives (checked): **dkgl-everything** needs porting 7 world-geometry shader
pairs (`tfrag3`, `tfrag3_no_tex`, `shrub`, `etie`, `etie_base`, `tie_wind`, `sprite3_3d`) to
ES310+std140 with per-shader HW validation — the natives already render that same (unified
TFRAG3_VERSION) data correctly on jak1 hardware, so extending their tables/categories is much
less work. **Native replacements** for shadow2/merc2/generic would duplicate what dkgl already
does. Waiting for #10 before coding serializes work that can overlap.

1. **jak2 bucket wiring in `dk_renderer.cpp`.** `record_tfrag_and_shrub`, the sky/ocean hooks and
   `dk_upload_bucket_hook` use hardcoded jak1 bucket IDs. Need the jak2 lists (per-level
   tex→tfrag/tie/etie/merc/gmerc → shrub chain → alpha chain → pris/pris2 → water chain, plus the
   `lcom` buckets), in GL bucket order, calling the same natives + `dkgl::record_bucket` /
   `capture_until` (that machinery is bucket-generic). This is the first thing to write.
2. **dkgl program `shadow2`.** jak2 shadows are `Shadow2` (buckets SHADOW and SHADOW2), program
   `shadow2` — not in the dkgl table (jak1's dkgl `shadow` is `ShadowRenderer`, which jak2 does
   not use). New `shaders/dkgl/shadow2.*.glsl` port + `dkgl_gen_shaders.py` regen + table entry.
3. **TextureAnimator on the dk path** (biggest new subsystem). GL passes `m_texture_animator` to
   every `TextureUploadHandler`; the dk upload replay has **zero** animator wiring, and the
   programs it uses (`tex_anim` for FBO composition, `plain_texture`) are not in dkgl. Without
   it, jak2's animated textures (Haven City signs/ads, water effects) are wrong/static. Needs:
   two new dkgl shader pairs + FBO composition inside the upload replay (dkgl FBOs exist from
   the eyes work) + per-tex-bucket animator hooks.
4. **`TFragmentDk`: WATER tree kind** (`tfrag-w-l*-water` buckets) + the jak2 bucket table, and
   verify the recorder against the GL tfrag3-jak2 vertex format (wind / multi-draw deltas —
   the native tfrag shader has no wind today).
5. **`TieDk`: missing categories.** Only NORMAL / NORMAL_ENVMAP / NORMAL_ENVMAP_SECOND_DRAW are
   wired; jak2 also needs TRANS, TRANS_ENVMAP, WATER, WATER_ENVMAP (the etie dksh already cover
   the envmap variants).
6. **`ShrubDk`: anim slot support.** jak2 `Shrub` is constructed with `anim_slot_array()`; the
   native recorder has none (depends on #3's animator plumbing for the actual textures).
7. **Enable the path.** `gfx.cpp` hardcodes jak1 as the deko3d default; jak2 needs
   `gk_dk3d.txt` + `gk_dkload.txt` + per-pass `gk_dk*.txt` opt-ins (`dk_pass_on`) until it
   renders right, then flip the default like jak1 did.
8. **Scale validation (code expected to hold, must be measured).** Haven City is much heavier:
   VRAM/pool sizing (T57's 384 MB was sized on jak1), `dktexgpu` slot recycling under jak2
   texture churn (pitfall P8), merc2/generic2 draw volume, ocean on jak2 data, sprite fill
   (same overdraw class as bug #17).
9. **Census sweep.** Run jak2 on the dk build and close every `dk_gl_census` unimplemented-call
   hit the same way #19 did; `glClearBufferfv` is a known miss already.
10. **Data prerequisite (not renderer work).** jak2 fr3 extraction (v43 RGBA) +
    `jak2_texture_remap` per JAK2_JAK3_ROLLOUT_PLAN §6.1 — nothing above can be tested without it.

### 7.3 Checked and NOT jak2 gaps

`depth_cue` (jak1-only registration), `SkyRenderer`/`SkyBlendHandler` (jak1-only in this fork —
jak2's sky-draw is a plain DirectRenderer), `hfrag` (jak3), `glow_*` (no GlowRenderer is
registered anywhere), `direct2`/`DirectRenderer2` (registered nowhere), movie playback (no
MoviePlayer in this fork at all), PCRTC (jak3-only), `CollideMeshRenderer` (not registered).
