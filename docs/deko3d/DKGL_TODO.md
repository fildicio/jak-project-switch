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
| 14 | jak2: reuse the native deko3d renderers where they already work, run the remaining GL bucket renderers through dkgl, add the missing dkgl programs (`shadow2`, `tex_anim`, ...) and TextureAnimator support — full list in §7. **tex_anim + TextureAnimator landed 2026-11-06 (T125, AI-assisted): the UNCHANGED GL animator runs through dkgl** (details in §7 item 3). **Glow landed 2026-11-06 (T126, AI-assisted):** the native T124 glow port (wrong strip order, no occlusion = flares through walls) was removed; the UNCHANGED GL `GlowRenderer` (new_mode: depth copy, probe grid, downsample, additive draw) runs through dkgl from the SpriteDk jak2 glow section into a `kGlowList` side list, called right after sprite bucket 313. dkgl gained depth textures (alias of the frame depth image, no copy), Z24S8 renderbuffers, depth attachments, vertex-stage sampling; side lists no longer replay twice from the world loop. A/B: `gk_dkgl_noglow.txt` | 🟡 (HW test pending, NRO md5 438aacef) |
| 15 | jak3: same as jak2, plus PCRTC brightness | ⏳ |

**jak3 slow motion below 60 fps — ✅ DONE, HW verified 2026-11-04 (GAME.CGO `efb5481f`, stable 30 fps).** Original note: `pc-maybe-vsync` in
`goal_src/jak3/engine/draw/drawable.gc` (~line 2248) still returns a fixed `1.0`, so every frame
advances game time by 1/60 s and any drop below 60 fps plays in slow motion. Port the jak2 fix
(commits `c07efb959` + `5149655da`): measure the frame with `__read-ee-timer`, divide by
300 MHz / target-fps, add it to a carried time debt, step 1.0 while the debt is < 1.15 and
`fmin 4.0 debt` otherwise, then clamp the leftover debt to [-0.5, 1.0]. Rebuild with
`goalc --game jak3 --instruction-set arm64 --cmd '(make-group "iso")'` and copy
`out/jak3/iso/GAME.CGO` to the SD. jak1 has the same carry in `display-frame-start`.

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
| particles (`Sprite3`) | `SpriteDk` native | ✅ code 2026-11 (AI-assisted): walks GL `render_jak2` layout (no CALL head, `SpriteFrameData`, matrix −1 glow sprites skipped), jak2 `sprite3_vert_jak2` dksh, recorded at bucket 313 in `record_jak2_dkgl`; near-fade off on jak2 (GL look). Opt-out `gk_nosprite.txt`. Glow (`GlowRenderer`) + distort NOT ported. **HW 2026-11-04 (NRO `bf66f569`): particles render, but three effects look wrong ⏳ TODO:** scatter-gun (red gun) shot = big hard-edged flat yellow/red wedge; Metal Head skull gems = flat orange discs; dark eco orbs = flat purple blobs. Log: `misses=0`, `no_tex` 0–7, so textures load. Shaders match GL. Step 1: compare `SpriteDk`/`dktf::fill_draw_state` blend + alpha test with GL `Sprite3::flush_sprites`/`update_mode_from_alpha1` for jak2. Step 2: port `GlowRenderer` (glow halos) and `Sprite3_Distort` (heat haze). Both in one HW trip |
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
  The jak2 `EyeRenderer` is wired through `SharedRenderState::eye_renderer` like GL. Was inert
  by design: shadow 195/314 (shadow2) landed §7.2 phase 2 (2026-10-31), and the menu/HUD tail
  (progress 320, debug2 324, debug-no-zbuf2 325, debug3 326) landed the same phase — menus,
  loading bars, pause minimap and HUD text now render through dkgl (`gk_dkgl_nomenu.txt` A/B).
  Still inert: warp 317. Particles 313: native `SpriteDk` (2026-11, HW: renders; scatter gun / skull gems / dark eco look wrong ⏳; glow/distort not ported).
  **HW 2026-10-31 (build e70b2c9d):** menus/main UI text ✅ show. Three regressions reported
  and under diagnosis: (a) pause minimap blank, (b) yellow background at title→Start, (c)
  shadows inside the hideout "weirdly white". Host-side the whole Shadow2 final-pass emission
  (stencil NOTEQUAL + RevSub/Add + ONE/ONE/ONE/ZERO + per-channel color mask + mid-frame
  stencil clear) is now golden-tested (`Shadow2LightenPassEmission`/`Shadow2DarkenPassUsesRevSub`,
  DkglState 34/34) — the state layer is provably correct, so (c) is runtime data (the
  time-of-day shadow color picks the *lighten* path indoors) or GPU-side. Diagnostics in the
  next build: `[shadow2] color r g b a -> pass` lines (one per distinct color, stdout),
  `[progress] fbp N` transitions, and a `notex` counter in the `[dkgl]` stats line (draws
  whose every sampler is unbound → stale texture, the likely shape of (b)).
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
  **HW 2026-10-31 (NRO `6e819bd6`, AI-assisted):** Haven City renders, much faster than GL, no
  fatal. Three bugs:
  (a) *wrong aspect* — same as jak1 #20: the SD `pc-settings.gc` had `aspect4x3 … #t` +
  `letterbox #t`; set to `(aspect-state aspect16x9 16 9 #f)` / `(letterbox #f)` (old file in
  `hw-logs/jak2-pc-settings.gc.before-aspect-fix`). Settings fix, not code.
  (b) *Jak's head missing* — dkgl Merc2 got `s_no_anim_slots` (all 0); jak2 slots 0–13
  (`texture_slots.cpp`) are Jak's face/hair/eyebrow/eyelid/finger, made by TextureAnimator
  `ClutBlender`. Stopgap: `fill_jak2_clut_slots()` in `dkgl_frame.cpp` builds each slot's
  default (`-norm`/`-start`) CLUT texture on the CPU from GAME.fr3 `index_textures` and uploads
  it with `dktexgpu::upload_rgba8_mipped` (`dkgl::set_common_level` from `dk_loader_start`).
  No dark-Jak morph until #3. Log: `[dkgl] jak2 anim slots: N/34 static clut textures`.
  (c) *violet corners* — user (photos IMG_0161/0165): missing shadows (shadow2, phase 2 #2).
  **HW 2026-10-31 #2 (NRO `de153a64`):** head OK (`anim slots: 12/32`). Game ran at 30 fps
  because the SD `pc-settings.gc` had `(fps 30)` (swap interval already 60): set to `(fps 60)`
  like jak1 (old file `hw-logs/jak2-pc-settings.gc.before-fps60`). Eyes missing on everyone
  (`eye draws 0`): jak2 `eye.gc` sends eye DMA through `vu1-bucket-map (merc-mode eye)` to the
  level's `tex-*` buckets, which dkgl did not run. Fix (NRO `4f48a65c`): `init_jak2` merges
  the 40 eye-capable tex buckets into the dkgl table as `PrisTexEyes` lists (`s_eye_list`);
  the eye barrier is only recorded when a list drew. Log: `[dkgl] jak2 eyes: 40 tex-bucket
  eye lists`, then `eye draws N` > 0 in the 10 s line.
  *Picture stretched/zoomed vertically* (user, gameplay too; jak1 fine): every dk/dkgl vertex
  shader baked the jak1 y-scale `(512/448) * 1.0`; GL uses `(512/416) * 0.5` for jak2
  (`Shader.cpp:67-81`), so jak2 y was ~1.86× too big. Fix (NRO `a92fa1df`): `gen-dksh.py
  --game jak2` rewrites the constants (fails if a `448` is left), `game/CMakeLists.txt` builds
  `*_vert_jak2_dksh` for tfrag3/shrub/etie/etie_base/merc2 + dkgl ocean_common/generic/shadow/
  direct_basic(_textured)/emerc/merc2, picked by `g_game_version` (`dk_device.cpp`,
  `use_jak2_verts()` in `dkgl_frame.cpp`). Jak1 `.dksh` bytes unchanged.
  **HW 2026-10-31 #3 (NRO `a92fa1df`): user — aspect, eyes, head all correct, nothing missing.**
  Log: `jak2 eyes: 40`, eye draws ~16/frame, fps 45.7–60 (steady 60 at 300–400 s, dips to
  46–50 in busy Haven City). Slow frames are `cpu` (render thread `build` ~6–8 ms + dkgl
  `capture` ~4–5 ms), GPU fence ~5 ms: next perf work is CPU-side (#8).
- **Phase 2 (days):** #2 `shadow2` — **landed 2026-10-31 (AI-assisted, HW pending)**; details in
  item 2 below (generated program, both bucket instances, dkgl's first mid-frame stencil clear).
  **Same session, bonus: the menus** — progress 320 (ProgressRenderer: loading bars + the
  pause-menu minimap off-screen FBO → VRAM 4032) and debug2 324 / debug-no-zbuf2 325 /
  debug3 326 (DirectRenderers, the menu/HUD-text buckets = jak1 debug 67 equivalents) wired
  through dkgl like sky-draw/screen-filter; the native DirectDk walk stays off jak2 (no CALL
  head). Upload-only neighbours (318/319/322) already ride the native sprite walk.
  A/B `gk_dkgl_nomenu.txt`. Remaining: #5 tie categories, #4 tfrag WATER.
- **Phase 3 (only multi-day subsystem, last):** #3 TextureAnimator + #6 shrub slots (rides on
  #3's plumbing). Deferring it does not block a playable-on-dk milestone — Haven City ads just
  render as static first-frame textures.
  **#3 landed 2026-11-06 (T125, AI-assisted; hardware test pending):** the UNCHANGED GL
  `TextureAnimator` now runs through dkgl on the Switch build, per the wrapper rule — no
  CPU-composite port (a first attempt, `TextureAnimDk.h`, was deleted for exactly that
  reason). Wiring: `init_jak2` constructs it from `s_srs->shaders` + the GAME.fr3 common
  level (`gk_dkgl_notexanim.txt` A/B); the SpriteDk tex-bucket walk dispatches PC_PORT
  imm-12 to `dk_texture_anim` (dk_renderer), which brackets
  `handle_texture_anim_data` in `dkgl::tex_anim_begin/end` — the animator's FBO draws
  record into a dedicated `kTexAnimList` (blackout-style pseudo bucket, called by
  `record_tex_anim()` right before the native sprite bucket 313 draws). Publish path is
  GL's own: `update_gl_texture` + `move_existing_to_vram`, with the pool seam resolving the
  dkgl texture id to a dktexgpu id, so the native sprite lookup finds the gems. Shaders:
  `tex_anim` added to the T103 generator (now supports uniform ARRAYS at std140 stride 16
  and ivec2/3/4) — `positions[4]`/`uvs[4]` spread in `uniform2fv/3fv`, `channel_scramble`
  via `uniform4i`. Still census stubs (logged, benign for skull gems):
  `glGenerateMipmap` (no mip content — sprites sample LINEAR), aniso `glTexParameterf`,
  debug-only `glGetTexImage`/`glGetTexLevelParameteriv`/`glGetFloatv`.
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
2. **dkgl program `shadow2` — LANDED 2026-10-31 (AI-assisted, HW pending).** jak2 shadows are
   `Shadow2` (buckets SHADOW 195 and SHADOW2 314 — GL registers exactly two instances, "shadow"
   and "shadow2"; the `shadow3` registration is jak3-only), program `shadow2`, not an alias of
   jak1's `shadow`. Shipped: `shadow2` added to `dkgl_gen_shaders.py PROGRAMS` (generated
   `shaders/dkgl/shadow2.{vert,frag}.glsl` from the GL sources + uniform-table entry; vert block
   7 slots/112 B, frag 1/16 B); `_DKGL_SHADERS` + `_JAK2_SHADERS` (jak2 y-scale variant,
   `dkgl_shadow2_vert_jak2_dksh`); `s_programs`/`kJak2Verts` entries; `SHADOW`/`SHADOW2` in
   `kJak2Buckets` at their GL dispatch positions; two `Shadow2` instances in `init_jak2`
   (A/B flag `gk_dkgl_noshadow2.txt`). **New dkgl capability — mid-frame stencil clear:**
   Shadow2's second instance resets the stencil buffer after the first drew its volumes
   (`glClearStencil(0)` + `glClear(GL_STENCIL_BUFFER_BIT)`, gated by `stencil_dirty` which
   `capture_begin` now resets per frame like GL). dkgl previously dropped the stencil bit of
   `glClear`. Added: `STENCIL_BUFFER_BIT` in `dkgl_gl_enums.h`, `clear_stencil` in
   `ShadowState` + `dkgl_glClearStencil` glad override, `Emitter::clear` now carries
   `stencil/mask/value`, and the Switch emitter does a stencil-only
   `dkCmdBufClearDepthStencil(cb, false, 0.f, mask, value)` with full-target scissors on the
   frame target — safe mid-frame (natives never write stencil, `clearDepth=false` leaves the
   world depth alone; the frame-begin clear still zeroes stencil at frame start). Everything
   else Shadow2 uses was already dkgl from T111/T113: stencil func/op/mask, colour mask,
   `FUNC_REVERSE_SUBTRACT`, depth `ALWAYS`/`GEQUAL`, separate alpha blend (works — repo
   deko3d is T101-patched), TRIANGLE_STRIP + primitive restart, per-frame buffer renames
   (ctor size-only `glBufferData` reserves nothing, per-frame uploads are the used counts).
   Host `Dkgl*` 36/36.
3. **TextureAnimator on the dk path** — ✅ code done 2026-11-06 (T125, AI-assisted; hardware
   test pending, see Phase 3 above for the full wiring). The dk upload replay now dispatches
   PC_PORT imm-12 to the UNCHANGED GL animator through dkgl (`tex_anim` program, kTexAnimList
   segments recorded at capture, called before sprite bucket 313). Not covered yet: shrub
   anim slots (#6) and a `glGenerateMipmap` implementation (census stub, mips stay empty —
   LINEAR sampling hides it for sprites).
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
   **Measured 2026-10-31 (NRO `a92fa1df`, handheld, STOCK clocks — no overclock):** playable
   now — avg ~55 fps, long locked-60 stretches, worst 10 s window 45.7 fps (busy Haven City),
   frame max mostly ≤ 22 ms. CPU-bound: slow frames are `cpu`, render thread `build` 6–8 ms +
   dkgl `capture` 4–5 ms, GPU fence only ~5 ms. Goal: locked 60 at stock handheld clocks (do
   not rely on overclock; docked raises only the GPU). Ideas: cache per-frame draw data that
   did not change, pin the render thread (it hops cores 0/1/2 — `gk_pin.txt` test first),
   split `build`/`capture` work across cores. Fallback: a well-paced 30 fps cap.
   **Correction 2026-11-01 (NRO `90c0eaf5` logs):** in heavy areas the frame time tracks
   `gpu world` almost exactly (22.1 vs 22.5 ms, 21.2 vs 21.5 ms) -- jak2 is GPU-bound there,
   not CPU-bound (the `slow` classifier sums every CPU part, so it always says `cpu`). Pinning
   was already tried (FIX 70b, regressed). Next: opt-in per-bucket GPU profiler
   (`switch/jak2/gk_gpuprof.txt` -> `[dk] gpu top ms/frame` line, NRO `fee755cd`), then
   optimise the top buckets.
   **Fix try 1 (NRO `1359571c`, 960x540): REVERTED** — user rejects any resolution drop below
   720p handheld. Checked and ruled out at 720p: fb HwCompression already on; TexturePool
   textures sample with GL_LINEAR (no mips on GL either); per-frame barriers (eye, tod copies)
   and the shadow2 full-screen quads are each well under 1 ms. Next fix needs the per-bucket
   `[dk] gpu top` data from the 720p profiler build `fee755cd`.
   **Profiler result (2026-11-01, NRO `fee755cd`, Haven + water station, 46 fps):** gpu world
   18-22 ms. Per unit ms/frame: sky-draw b5 **5.8-8.7** (biggest; 1.9 on the title screen),
   native tie 3-6 (one level at a time, e.g. tie2/tie6), native shrub 3-7 (sh1/sh3 ~3.5 each;
   shrub has no culling on GL either, 245k tris/frame), merc pris b201/b205 1-2, tfrag only
   ~0.5. Next: find why the sky costs ~25 full-screen layers (expected ~1-2 ms), then shrub.
   Title "yellow": sky-tng menu-mode tints the blit-displays capture (tbp #x4c0/#x3300) sepia;
   dkgl has no framebuffer copy there, so only the tint shows.
   **Sky fix (HW-confirmed 2026-11-01, NRO `2ce828bc`):** the jak2 sky double-draw (two
   alpha-test discard passes) is folded into one draw on Switch (`DirectRenderer.cpp`, only
   for `sky-direct`) and dkgl binds the discard-free `direct_basic_textured_fast` shader, so
   early-Z works: sky b5 6-9 -> ~3 ms, Haven 46 -> 53-60 fps. Remaining top: tie 3-5, shrub 2-4.
   TieDk/TFragmentDk now honour `lod-force-tie/tfrag` from pc-settings (default 0 = full
   detail; card left at 0 -- lower LOD is a visual trade-off for the user to choose).
   **Slow-mo fix (2026-11-01, GAME.CGO `f8765700`, HW pending):** `pc-maybe-vsync` in
   `goal_src/jak2/engine/draw/drawable.gc` now returns the measured frame time as the dog-ratio
   (jitter < 1.15 -> 1.0, max 4.0), like the PS2 display-sync, so drops below 60 no longer slow
   the game down.
   **TODO — next perf step: make tie and shrub cheaper at 720p, full detail (no LOD/res drop).**
   Profiler after the sky fix: tie 3-5 ms (tie2/tie4/tie6), shrub 2-4 ms (sh1/sh2/sh3),
   sky ~3 ms; target total < 16.7 ms with room for particles/warp/texture animator.
   Ideas to measure one at a time with `gk_gpuprof.txt`: (a) shrub has no frustum culling
   (~245k tris/frame) — add per-instance/per-group culling like TieDk; (b) check tie/shrub
   shaders for `discard` (alpha test) on opaque draws, which disables early-Z (same cause as the
   sky); (c) draw order front-to-back / depth pre-pass for the heaviest tie trees; (d) vertex
   format/size and fragment shader cost (fog, tod blend done per pixel vs per vertex).
   **Step 1 (2026-11-03, NRO `b33c1147`, HW pending):** TieDk read and discarded the jak2
   proto-vis mask and ShrubDk never read it, so both drew every proto the game had hidden (GL
   skips them). Both now apply it (`dk_proto_vis.h`, host test `DkProtoVis`). Measure tie/shrub
   ms with `gk_gpuprof.txt` in Haven City before trying (a)-(d).
9. **Census sweep.** Run jak2 on the dk build and close every `dk_gl_census` unimplemented-call
   hit the same way #19 did; `glClearBufferfv` is a known miss already.
10. **Data prerequisite (not renderer work).** jak2 fr3 extraction (v43 RGBA) +
    `jak2_texture_remap` per JAK2_JAK3_ROLLOUT_PLAN §6.1 — nothing above can be tested without it.

### 7.3 Checked and NOT jak2 gaps

`depth_cue` (jak1-only registration), `SkyRenderer`/`SkyBlendHandler` (jak1-only in this fork —
jak2's sky-draw is a plain DirectRenderer), `hfrag` (jak3), `glow_*` (no GlowRenderer is
registered anywhere), `direct2`/`DirectRenderer2` (registered nowhere), movie playback (no
MoviePlayer in this fork at all), PCRTC (jak3-only), `CollideMeshRenderer` (not registered).
