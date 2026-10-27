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
| 16 | Jak's shadow looks weird | ⏳ | Reported 2026-10-27 (NRO `de27847a`). Needs a photo + A/B with `gk_dkgl_noshadow.txt` and `gk_opengl.txt` |
| 17 | Geyser Rock: water/horizon wrong + fps drop | 🟡 | Log `20261027-1931`: 52 fps in one 10 s window with ~11.7 ms of GPU in the "sprite" timer, but that timer covered every dkgl pass after merc (emerc, generic water/alpha, shadow, ocean-near, depth-cue) plus the sprites. The timer is now split (`pass ms` line shows `glmerc`, `alpha`, `oceann`, `dcue`, `sprite`), so the next Geyser log names the culprit. The `direct_basic_textured_multi_unit ... (inert)` line is harmless: it is printed once while dkgl links the whole shader library, no game draw uses it |
| 18 | Choosing a language doesn't change the dialogs | ⏳ | Not rendering. The game code matches upstream OpenGOAL, which splits language in 3 options (Options → Game): **Language** = voices (`VAGWAD.<lang>`), **Subtitle language** = cutscene subtitles (`<N>SUBTIT.TXT`), **Text language** = menus/in-game text (`<N>COMMON.TXT`); Italian = 4. Every HW log shows only the boot `Language set to 0`, and a missing file is logged only in the `lg` file log, which `sd-sync.sh` did not pull. `sd-sync.sh` now pulls `log/*.log` and writes `lang_files.txt` (which language files are on the SD). Next: set all 3 options to Italiano, watch a cutscene, sync |
| 19 | Characters have no eyes / look weird | 🟡 | Cause: the native merc (MercDk) drew models with face blend shapes (blerc) in the rest pose and had the fade pass off. Fix: characters now run through dkgl with GL `Merc2` (same as the PC port: blerc, fade, eyes from the native eye pass). Opt-out for an A/B: `gk_dkgl_nativemerc.txt` (old native characters). Needs a hardware look at faces |

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
