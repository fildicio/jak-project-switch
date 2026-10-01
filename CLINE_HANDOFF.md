# Handoff from Cline session (AI-assisted) — 2026-10-01 (late)

Repo: fildicio/jak-project-switch. **Current branch: `pbo-async-texture-upload`**
(FIX 69, unmerged by design). `main` = `slows-down-new-area` = FIX 68 (`2d0e713ec`
code commit), pushed. Full details: `SWITCH_PORT_SESSION_NOTES.md`, "FIX 68" and
"FIX 69" sections.

## Session result — FIX 69 written+built+staged, deploy PENDING

The user chose "Option 2" (take texture upload off the frame) over "Option 1"
(optimize the ~25 ms renderer CPU) on ease/token/result. FIX 69 = PBO async
texture upload in `game/graphics/opengl_renderer/loader/LoaderStages.cpp`
(`add_texture` Switch branch): bytes staged in a persistent
GL_PIXEL_UNPACK_BUFFER (orphan+refill), same `glTexImage2D` issued from buffer
offset 0. NOT the FIX 33 banding (that was client-memory banded
glTexSubImage2D); this is the stock buffer-object path.

- **Kill-switch:** `sdmc:/gk_nopbo.txt` on the card → atomic FIX 68 path, same
  build. A/B by moving one file. Logged at first texture:
  `[loader] FIX 69 PBO upload enabled` / `... DISABLED`.
- Built ×3 (jak1 `25321474`, jak2 `59041473`, jak3 `ec4d0b18`), 0 errors.
  Branch pushed; main left on F68 until hardware confirms.
- **The card was NOT deployed** — the reader dropped the card mid-deploy. The
  first cp failed cleanly (card untouched). WHEN THE CARD IS BACK:
  1. rotate live NRO → `.f68.bak`, 2. cp the three build NROs (paths below),
  3. md5 compare against `25321474`/`59041473`/`ec4d0b18`.
  Desktop copies: `~/Desktop/jak bakcups/Jak {1,2,3}.f69.nro` (md5-identical).

## Card state — CURRENTLY LIVE, F68 (F69 not yet deployed)
| game | live md5 | rollback (F67) |
|---|---|---|
| jak1 | `83dfd6cc88484fb7aee8a8465b0a44cd` | `Jak 1.f67.bak` (`f4bf79ce`) |
| jak2 | `6050a3f18e1a7c0c430f2a674025637c` | `Jak 2.f67.bak` (`fe2b104f`) |
| jak3 | `9e6e3b1d0de6f6b5bd706adb482470ab` | `Jak 3.f67.bak` (`a3384258`) |

After F69 deploy, rollbacks become `.f68.bak` (the md5s in this table).
Desk copies of F68: `~/Desktop/jak bakcups/Jak {1,2,3}.f68.nro` + `new25-builds/`.

Note: `gk_stdout.txt` lags real time up to ~2 s (FIX 67 buffering) — expected.
The Switch clock runs ~1 day ahead of the Mac; trust the card's own dates.
The reader connection dropped twice this session — if the card vanishes
mid-deploy, the cp chain aborts before any write; check `ls /Volumes/` first.

## Hardware test plan (F68+F69 in one boot)
1. Boot jak2, expect `[loader] FIX 69 PBO upload enabled`, then stream into a
   new area (atoll / city sections) and jak1 beach/jungle.
2. F68 checks still apply: `budget ms=7.0 tex_kb=2048 mode=catchup-pace`,
   ready times ~2.5–6 s (atoll ~7 s), post-ready shimmer ~2–4 s.
3. F69 check: `slow setup` texture-stage ms should drop well below the F67
   6–8.5 ms baseline (per-texture ~1.6 ms → <1 ms) at the same budgets.
   If unchanged → nouveau fell back to sync copy; park `gk_nopbo.txt` on the
   card, record numbers, escalate to shared-context upload thread (Option 2b).
4. Crash at first texture (esr data abort, FIX 33a style) → create
   `/switch/gk_nopbo.txt` via reader, boot again, no rebuild needed.

## Next queue
1. Deploy F69 when the card is back (see above), then hardware test.
2. If F69 confirms: fake_iso read-ahead (card burst reads during streaming).
3. tpage-link RpcSync busy-wait.
4. Option 2b: shared-context upload thread (only if PBO falls back to sync).
5. Option 1 (renderer buckets CPU) only behind a cheap gate: enable the
   `[phase]` diagnostic split first; skip if no single bucket dominates.

## Operational reminders
- Build serially, never concurrently: docker devkita64, JOBS=2, per-game build
  dirs (jak1-f61, jak2-f60, jak3-f61). Incremental ≈ 30 s/game when only one
  TU changes. Target name is `gk_nro` (ninja), not `gk.nro`.
- Deploy: rotate card NRO to `.fN.bak` FIRST, then cp, then `sync`, then md5
  compare against the build dir NRO.
