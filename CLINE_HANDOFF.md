# Handoff from Cline session (AI-assisted) — 2026-10-03 (late)

Repo: fildicio/jak-project-switch. Branch `slows-down-new-area` = main = `2d0e713ec`
(FIX 68), pushed. Full details: `SWITCH_PORT_SESSION_NOTES.md`, "FIX 68" section.

## Session result — FIX 68 shipped, awaiting hardware test
The card was still in the reader with the F67 test logs; they isolated the
user-reported residual ("assets delayed, worst in jak2; fps dips while a new
area loads") to two numbers, both fixed:

1. **catchup-pace was the single binding constraint.** Loads served by
   `catchup` (8 ms/2 MB) finish in 0.44–1.81 s; every stream-in load lands in
   `catchup-pace` (5 ms/1 MB) and took 3.8–9.97 s (atoll: 848 tex × ~1.6 ms ÷
   3 tex/frame ≈ the measured 9.97 s). Fix: pace payload → **7 ms / 2 MB /
   2048 KB**. Guards kept: FIX 52 gpu_scale + per-frame timer hard stop.
2. **Mip drain token rates re-introduced FIX 42a's "ten seconds unfiltered"**
   (atoll: 426 chains at rate 1–2 = 7–14 s of shimmer AFTER "ready"). Fix:
   backlog-aware rates while busy — backlog >256: rate 4/6/8 by EMA band
   (>38/30–38/<30); small backlogs 2/3/3.

Both in `game/graphics/opengl_renderer/loader/Loader.cpp` (update_frame_budget
pace branch + mip ladder in the FIX 42 block). Built ×3 serial docker
devkita64 (Loader.cpp TU only, 0 errors), deployed with rotation + md5 verify,
committed and merged to main.

## FIX 68 expectations to verify on hardware
- jak2 stream loads 4–10 s → ~2.5–6 s (atoll ~7 s); post-ready shimmer 7–14 s →
  ~2–4 s. jak1 beach/jungle 7–9 s → ~5–6.5 s.
- Frames during a stream may run ~2–3 ms longer (28 vs 30 fps) but finish
  sooner; `budget ms=7.0 tex_kb=2048 mode=catchup-pace` should appear in
  gk_stdout.
- If it regresses: gpu_scale should start halving tex_kb within a mode (FIX 52)
  — if that fires often, pull pace back toward 5–6 ms.

## Card state — CURRENTLY LIVE, F68
| game | live md5 | rollback (F67) |
|---|---|---|
| jak1 | `83dfd6cc88484fb7aee8a8465b0a44cd` | `Jak 1.f67.bak` (`f4bf79ce`) |
| jak2 | `6050a3f18e1a7c0c430f2a674025637c` | `Jak 2.f67.bak` (`fe2b104f`) |
| jak3 | `9e6e3b1d0de6f6b5bd706adb482470ab` | `Jak 3.f67.bak` (`a3384258`) |

Desk copies: `~/Desktop/jak bakcups/new25-builds/Jak {1,2,3}.f68.nro` (md5s
identical to card). Older rotations still on card (f6x.bak chain).

Note: `gk_stdout.txt` lags real time up to ~2 s (FIX 67 buffering) — expected.
The Switch clock runs ~1 day ahead of the Mac; trust the card's own dates.

## Next queue (if FIX 68 confirms)
1. fake_iso read-ahead (raw burst reads hammer the card during streaming) —
   next FIX 69 candidate.
2. tpage-link RpcSync busy-wait.
3. If asset delay persists after both, revisit per-texture submit cost
   (~1.6 ms; the atomic add_texture path is load-bearing for nouveau — do not
   band it, see FIX 33a).

## Operational reminders
- Build serially, never concurrently: docker devkita64, JOBS=2, per-game build
  dirs (jak1-f61, jak2-f60, jak3-f61). Incremental ≈ 30 s/game when only
  Loader.cpp changes.
- Deploy: rotate card NRO to `.fN.bak` FIRST, then cp, then `sync`, then md5
  compare against the build dir NRO.
