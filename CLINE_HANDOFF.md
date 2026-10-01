# Handoff from Cline session (AI-assisted) — 2026-10-03 (evening)

Repo: fildicio/jak-project-switch. Branch `slows-down-new-area` = main + FIX 65/66/67.
`main` fast-forwarded to the same commit and pushed. Full details:
`SWITCH_PORT_SESSION_NOTES.md`, section "2026-10-03 (later)".

## User-verified on hardware (2026-10-03)
- **FIX 66 (byte-budget texture dispatch): confirmed.** jak1 village1: 667 textures
  in 1.38 s (~480/s vs old ~120/s). Count cap no longer binding.
- **FIX 67 (async buffered safe-stdout): confirmed.** jak1 loads visibly faster
  (60-78 ms hitch trains gone), jak2 faster too.
- **Residual (next target)**: assets — especially in jak2 — still delayed; fps
  dips when a new area starts loading. Ranked suspects:
  1. jak2 per-texture upload cost (~1.6 ms/texture) gated by the 5 ms
     catchup-pace tier → only ~3.5 textures/frame (give the texture stage its
     own budget line so the frame-gap EMA does not throttle it);
  2. fake_iso read-ahead absent (FIX 68 candidate — raw burst reads on the card
     while streaming);
  3. tpage-link RpcSync busy-wait.

## Card state (2026-10-03 evening) — CURRENTLY LIVE, F67
| game | live md5 | rollback |
|---|---|---|
| jak1 | `f4bf79ce026ec98c88067ad5ceee755f` | `Jak 1.f66.bak` (`5cc6c07c`) |
| jak2 | `fe2b104fe774ae2845d20140b097d941` | `Jak 2.f66.bak` (`2f07d622`) |
| jak3 | `a3384258b6850e39b459920317f3d24f` | `Jak 3.f66.bak` (`ab0638f3`) |

Desk copies of the same F67 builds: `~/Desktop/jak bakcups/new25-builds/Jak {1,2,3}.nro`
(md5s identical to the card).

Note: `gk_stdout.txt` now lags real time by up to ~2 s (FIX 67 buffering) — that is
expected, not a wedged card. `[stdout] N lines dropped` lines only appear under
extreme card pressure.
