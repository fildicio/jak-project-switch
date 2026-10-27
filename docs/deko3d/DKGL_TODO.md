# What is left to convert to deko3d (AI-assisted)

Rule: everything below is done through the **dkgl wrapper** (see `AI_ROLES.md`,
"Wrapper rule"). Status: ⏳ todo · 🟡 code done, hardware test pending · ✅ done.

## Already on deko3d (jak1)

| Part | How |
|---|---|
| tfrag (+ alpha, ice, dirt), tie (+ wind, envmap), shrub, sky, sky-blend | native |
| merc (characters, without shine), eyes, sprite, direct (HUD, text, debug) | native |
| texture uploads, level loader | native |
| ocean texture + mid + near | dkgl |
| generic (distant mountains, 10 buckets), shadow, depth-cue | dkgl |

## 1. Open bugs (jak1)

| # | Bug | Status | Next step |
|---|---|---|---|
| 1 | Black line at the sea horizon | ✅ | Fixed by running the far ocean through dkgl (NRO `319f35f7`, confirmed on hardware 2026-10-27) |
| 2 | Jak / objects disappear in busy scenes | ✅ | Merc limit raised 256 → 1024 (GL value), confirmed on hardware 2026-10-27 |
| 3 | Black boxes near Samos's hut while a level loads | ⏳ | Check `tex relocate ... source missing` in the logs; video of the moment |
| 4 | Dark vertical stripe in the sky (IMG_0108) | ⏳ | Find which bucket draws it (disable buckets one by one with flags) |

## 2. Missing features (jak1, via dkgl)

| # | Feature | What the GL side uses | Status |
|---|---|---|---|
| 5 | Shine on orbs / metal (merc envmap, "emerc") | GL `Merc2` envmap draws: add the merc2 + emerc programs to dkgl and run those draws through it | ⏳ |
| 6 | Screen fade to black (cutscenes, loads) | GL `FullScreenDraw` with the `solid_color` program (today logged as the only `inert` dkgl draw) | ⏳ |
| 7 | Brightness / contrast (PCRTC) | jak1: no-op (GOAL never calls it). Do it with jak3 | — |
| 8 | Dock / undock resize | swapchain size is fixed at boot (same as GL FIX 73c) | ⏳ |

## 3. Close the hardware checks

| # | Item | Status |
|---|---|---|
| 9 | T102 / T110: ocean hardware test (Sentinel Beach, 20 min, heap flat, fps unchanged) | ⏳ |
| 10 | T112 / T113: depth-cue + shadow hardware test | 🟡 (seen working, close the rows) |

## 4. Release (jak1)

| # | Item | Status |
|---|---|---|
| 11 | Commit the pending work (fog fix, T112/T113, merc limit, far ocean via dkgl) | ⏳ ask the user |
| 12 | deko3d on by default: replace the opt-in flags (`gk_dk3d`, `gk_dkload`, `gk_dktfrag`, `gk_dkshrub`, `gk_dktie`) with one opt-out `gk_opengl.txt`, so users only replace the NRO | ⏳ |
| 13 | Long soak test (1 h, several levels) + GL fallback still works | ⏳ |

## 5. jak2 and jak3 (all via dkgl)

| # | Item | Status |
|---|---|---|
| 14 | jak2: run its GL bucket renderers through dkgl; add the jak2 shader programs (tfrag/tie/shrub/merc/generic/sprite/... as needed) | ⏳ |
| 15 | jak3: same as jak2, plus PCRTC brightness | ⏳ |
