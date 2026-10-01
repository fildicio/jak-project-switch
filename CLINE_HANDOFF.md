# Handoff from Cline session (AI-assisted) — 2026-10-01 (late)

Repo: fildicio/jak-project-switch. **Current branch: `pbo-async-texture-upload`**
(FIX 69, unmerged by design). `main` = `slows-down-new-area` = FIX 68 (`2d0e713ec`
code commit), pushed. Full details: `SWITCH_PORT_SESSION_NOTES.md`, "FIX 68" and
"FIX 69" sections (FIX 69 ends with the hardware verdict).

## Session result — FIX 69 written, built, deployed, **hardware-REJECTED, parked**

The user chose "Option 2" (take texture upload off the frame) over "Option 1"
(optimize the ~25 ms renderer CPU) on ease/token/result. FIX 69 = PBO async
texture upload in `game/graphics/opengl_renderer/loader/LoaderStages.cpp`
(`add_texture` Switch branch): persistent GL_PIXEL_UNPACK_BUFFER, orphan +
`glBufferData` refill, same `glTexImage2D` from buffer offset 0. NOT the FIX 33
banding (that was client-memory banded glTexSubImage2D); stock buffer-object path.

**First hardware test REJECTED it** (jak2, player: "loads slower, loses more
fps"). Logs: `[loader] FIX 69 PBO upload enabled` at boot, then `slow setup`
texture submit up to **56.5 ms** (F67/F68 baseline 6–8.5 ms), 1013 occurrences.
Mechanism: nouveau has no orphaning for an in-use PBO — each refill blocks until
the GPU drained the previous upload (implicit sync). Strictly worse than the
client-memory copy on this driver.

Standing state after the verdict:
- **Card runs the F69 NRO with `sdmc:/switch/gk_nopbo.txt` present** = pure
  FIX 68 atomic texture path, same binary. Do not delete that file while the
  F69 NRO is live. **F68 itself is still unvalidated** — the next boot IS the
  clean F68 test (plan below).
- Branch has the default flipped to opt-in (`sdmc:/gk_pbo.txt` enables,
  `gk_nopbo.txt` forces off even then); compiles clean; NOT deployed (the
  flag already covers the card — rebuilding the live binary mid-test adds
  risk for zero behavior change). Branch stays unmerged; main = F68.
- Option 2 via PBO is closed on nouveau. Revisit only as Option 2b
  (shared-context upload thread) or per-texture PBOs + fence/defer pattern.
  FIX 33a's rule stands: atomic `glTexImage2D` is the only shipped path.

## Card state — LIVE: F69 NRO + gk_nopbo.txt (= FIX 68 behaviour)
| game | live md5 | rollback (F68) |
|---|---|---|
| jak1 | `25321474fd0af3edb6ec9e36d4c45eaa` | `Jak 1.f68.bak` (`83dfd6cc`) |
| jak2 | `59041473b711d9bbefb4d971bc437fa4` | `Jak 2.f68.bak` (`6050a3f1`) |
| jak3 | `ec4d0b188cf5e53a62a2370af7011a94` | `Jak 3.f68.bak` (`9e6e3b1d`) |

Boot log must say `[loader] FIX 69 PBO upload DISABLED (sdmc:/gk_nopbo.txt
present)` — that line confirms the atomic path is active.
Desktop copies: `~/Desktop/jak bakcups/Jak {1,2,3}.f69.nro` (+ `.f68.nro`).

Note: `gk_stdout.txt` lags real time up to ~2 s (FIX 67 buffering) — expected.
The Switch clock runs ~1 day ahead of the Mac; trust the card's own dates.
The reader connection dropped twice this session — if the card vanishes
mid-deploy, the cp chain aborts before any write; check `ls /Volumes/` first.
Card flag files: `/switch/gk_nopbo.txt` MUST stay while the F69 NRO is live;
`/switch/gk_no_vag.txt` (iso.cpp) is unrelated.

## Hardware test plan — NEXT BOOT IS THE CLEAN F68 TEST
The F69 test conflated F68+PBO; the flag un-conflates it. Boot jak2, stream
into new areas (atoll / city sections) and jak1 beach/jungle:
1. Expect the DISABLED line above in gk_stdout.
2. F68 checks: `budget ms=7.0 tex_kb=2048 mode=catchup-pace`; ready times
   ~2.5–6 s (atoll ~7 s) vs F67's 3.8–9.97 s; post-ready shimmer ~2–4 s;
   `slow setup` back to the ~7 ms pace baseline (was 5 at F67's pace, 8 at
   catchup; NOT 20–56 ms — that means the flag file is missing).
3. F68 regression check: gpu_scale (FIX 52) firing often (`tex_kb` halving
   within a mode) → pull pace back toward 5–6 ms in Loader.cpp on main.

## Next queue
1. User's clean F68 test (above). Good → main already F68, nothing to merge,
   F69 branch stays parked. Bad → tune pace on `slows-down-new-area`/main.
2. fake_iso read-ahead (card burst reads during streaming).
3. tpage-link RpcSync busy-wait.
4. Option 2b (shared-context upload thread) — only if uploads-off-frame is
   ever worth the surgery on this driver.
5. Option 1 (renderer buckets CPU) only behind a cheap gate: enable the
   `[phase]` diagnostic split first; skip if no single bucket dominates.

## Operational reminders
- Build serially, never concurrently: docker devkita64, JOBS=2, per-game build
  dirs (jak1-f61, jak2-f60, jak3-f61). Incremental ≈ 30 s/game when only one
  TU changes. Target name is `gk_nro` (ninja), not `gk.nro`.
- Deploy: rotate card NRO to `.fN.bak` FIRST, then cp, then `sync`, then md5
  compare against the build dir NRO.
