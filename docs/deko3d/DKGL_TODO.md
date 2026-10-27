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
| merc (characters, faces, fade) + emerc shine (GL `Merc2`, 8 buckets) | dkgl (#19, hardware test pending) |
| screen fade to black (pmode_alp) + loader "finish before fade-in" | dkgl + loader (confirmed on hardware 2026-10-27) |

## 1. Open bugs (jak1)

| # | Bug | Status | Next step |
|---|---|---|---|
| 1 | Black line at the sea horizon | ✅ | Fixed by running the far ocean through dkgl (NRO `319f35f7`, confirmed on hardware 2026-10-27) |
| 2 | Jak / objects disappear in busy scenes | ✅ | Merc limit raised 256 → 1024 (GL value), confirmed on hardware 2026-10-27 |
| 3 | Black boxes / things disappearing while a level loads | ✅ | `dk_set_pmode_alp` was a stub (no fade-to-black, no "finish loads before fade-in"). Fixed in NRO `e73e23e2`, confirmed on hardware 2026-10-27 |
| 4 | Dark vertical stripe in the sky (IMG_0108) | ✅ | Same cause as #3 (photo taken during a load). Fixed in NRO `e73e23e2`, confirmed on hardware 2026-10-27 |
| 16 | Jak's shadow looks weird; dark shade on top of Jak and Daxter (IMG_0122) | ⏳ | Reported again 2026-10-27, also on the old build. Suspect: the dkgl shadow (stencil) volume. A/B: boot once with `gk_dkgl_noshadow.txt` on the SD; if the shade goes away the culprit is the shadow pass |
| 17 | Geyser Rock fps drop when Jak smashes the wooden training target | 🟡 | Log `20261027-2121`: during the smash the `sprite` pass averages 5.5 ms of GPU (worst frame 31.9 ms) with a normal sprite count (~1.5k, 21 draws). Sprite vertex+fragment shaders, sizes and alpha test match the PC renderer exactly, textures are block-linear, the game does not touch clocks: it is pure overdraw (fill rate) of big smash particles. Fix 1 (NRO `a08dd132`): sprites that do not write depth use `sprite3_ez.frag` (`layout(early_fragment_tests)`), so particle pixels hidden behind the world are rejected before shading/blending; identical picture. Log line `sprite frame ... early_z=N`. Off switch: `gk_dk_spritelatez.txt`. Next option if still slow: colour compression (`DkImageFlags_HwCompression`) on the frame target (needs the 3D splash + frame-grab check) |
| 18 | Language: voices stay English | ✅ (user setting) | Log `20261027-2121`: `pc-settings.gc` still has `game-language 0`, so the option was never confirmed. In Options → Game → Language pick Italiano with left/right and **press A**; X/Y (square/triangle) cancel and restore the old value. Voices follow this option (`VAGWAD.ITA` is present) |
| 19 | Characters: eyes white/green, mouth not drawn/moving | 🟡 | Mouth: fixed by GL `Merc2` through dkgl (NRO `9f1eb189`). Eyes (IMG_0121/0122: white or green blobs): the native eye pass rendered fine (bad=0) but the hand-off to Merc2 was wrong. Now the GL `EyeRenderer` (bucket 54) runs through dkgl too, Merc2 samples its textures, and dkgl puts a fragment barrier after it (NRO `0b04922c`). Opt-out for an A/B: `gk_dkgl_nativeeyes.txt`. Eyes then rendered BLACK on `a08dd132`: the EyeRenderer needs 40 textures + 40 FBOs, and the dkgl object tables held only 64 each, so the later eyes got id 0. Tables raised to 256 (NRO `41bbc824`). Needs a hardware look at faces |
| 20 | Intro "Sony presents" screen: the dark fade covers only the 4:3 centre, the sides stay bright (IMG_0115) | ⏳ | The game itself is 16:9 (`aspect-ratio-auto? #t`, the window decides). Checked and ruled out: GOAL `blackout`/`letterbox` (full 512x224 sprites), the dkgl pmode fade (full-screen quad, full viewport) and the native direct renderer scissor (same maths as GL). Remaining suspect: the 2D/HUD sprite pass, which the PC port keeps at 4:3 on purpose (may be normal PC behaviour). A/B: boot once with `gk_opengl.txt` (full GL), once with `gk_nosprite.txt` |

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
| 12 | deko3d on by default: jak1 boots on deko3d with no flag files; `gk_opengl.txt` opts out (replaces `gk_dk3d`, `gk_dkload`, `gk_dktfrag`, `gk_dkshrub`, `gk_dktie`) | 🟡 needs a boot test with no flag files |
| 13 | Long soak test (1 h, several levels) + GL fallback still works | ⏳ |

## 5. jak2 and jak3 (all via dkgl)

| # | Item | Status |
|---|---|---|
| 14 | jak2: run its GL bucket renderers through dkgl; add the jak2 shader programs (tfrag/tie/shrub/merc/generic/sprite/... as needed) | ⏳ |
| 15 | jak3: same as jak2, plus PCRTC brightness | ⏳ |

## 6. When is the migration finished?

The deko3d migration is done when all of these are true:

1. jak1: rows 3–13 above are ✅ (no open bugs, all features, deko3d on by default, soak test passed).
2. jak2 and jak3: rows 14–15 are ✅ and each game boots and plays one full level on hardware with deko3d.
3. Performance is the same as or better than GL (fps and frame ms in the `[dk]` log), measured per game.
4. GL stays only as the opt-out fallback (`gk_opengl.txt`); no game needs it to look right.
