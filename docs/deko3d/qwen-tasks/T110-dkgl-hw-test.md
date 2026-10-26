# T110 — (human) HW test of the dkgl ocean (bundle with T102's SD trip)

**Depends on:** T107, T109, and the same physical SD trip as T102. **Category:** human.
**Size:** ~20 min on the console. Text-only report (describe; do not commit screenshots).

## Goal

First-hardware validation of the dkgl hybrid ocean (brief item 5). Run it on the SAME SD
trip as T102 (Sentinel Beach soak) — one deploy, two protocols.

## Setup

1. Build jak1 NRO (T109's last md5). Deploy NRO + **the synced shader folder** (T107
   Edit 3 — `sdmc:/switch/jak1/game/graphics/opengl_renderer/shaders/`, ~45 files;
   verify one file exists on the SD before booting).
2. Boot jak1 with the normal `gk` log capture.

## Checklist (in order; each item = one log line or one observation)

1. **Init**: `[dkgl] init ok: programs=5 buckets=…` present; `[dkgl] glad overrides…`
   line present; no `[dkgl]` failure lines. If the init-skip line appears
   (`GL shader folder missing on SD`) → fix the sync, re-run, that's a setup failure
   not a code failure.
2. **Ocean visible**: Sentinel Beach (and the jungle beach) — water surface, wake lines,
   envmap shine, mipped water at distance. No black slab over the water, no missing
   water plane, no z-fighting bands.
3. **No crash over 20 min** incl. Sentinel Beach + jungle + a menu round-trip; watch
   `heap free` stability (T101 regression guard — dkgl adds per-frame ring slices, the
   ring must recycle).
4. **fps**: same measurement points as T102 (60.x fps ocean areas) — dkgl replaces the
   native ocean; a drop > ~1 fps sustained → report numbers, don't debug on console.
5. **Census silence**: grep the log for the census first-use lines — the ocean's GL calls
   are dkgl-intercepted BEFORE the census, so census must NOT log any
   `glXxx first call from ocean…`. Any census line naming an ocean file = a GL call the
   dkgl layer doesn't cover → quote it verbatim in the report (it's a bug list).
6. **Fallback flags**: copy `gk_dkocean_native.txt` → ocean falls back to the native
   dkocean path (log shows which path won; visuals = the known T100-era look). Remove it.
   Copy `gk_noocean.txt` → no ocean at all. Remove.
7. **Upside-down check**: if the water texture appears vertically flipped (reflection
   running the wrong way), note it — the reviewer then decides on a T103 `--flip-y`
   regeneration (origin is LowerLeft, so this is NOT expected; only note if seen).
8. **Depth check**: swim/ride into the water — the near-water surface must sort against
   Jak and the beach like the native ocean did (T100/FIX 114 history: mid plane
   `GL_ALWAYS`+write, far band culling).

## Report (text only)

For each checklist item: pass/fail + the exact log lines. If anything fails, the console
log around the failure is the deliverable — no code changes on the console trip.
