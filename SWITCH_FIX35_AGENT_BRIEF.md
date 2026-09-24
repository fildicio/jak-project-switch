# FIX 35 — agent brief: jak2 on Switch, 720p @ 30 fps (AI-assisted)

Hand this whole file to the agent. It is written to be executed top-to-bottom.
Repo: `jak-project-switch-original` (branch `develop`). Read
`SWITCH_PORT_SESSION_NOTES.md` (bottom ~400 lines) before touching anything.

---

## 0. Current state (measured, do not re-derive)

From `sdmc:/switch/jak2/gk_run_log.txt`, steady state in the city / on the zoomer:

```
[fps] 13.3 avg (49.79ms) | wait_dma 18.15/39.69 render 29.70/32.12 swap 0.86/3.42
[phase] setup 0.01 | loader 0.01 (max 0.02) | buckets 28.94 | blit 0.00 | pcrtc 0.05
```

- **render ≈ 29.5 ms, of which `buckets` ≈ 29 ms** — the bucket renderers.
- **`wait_dma` ≈ 18 ms** — render thread idle, waiting for the GOAL simulation.
  These add up: ~48 ms = ~13 fps, and because the GOAL clock advances per frame
  the game goes into **slow motion** rather than just getting choppy.
- `loader` ≈ 0.01 ms/frame — FIX 33 solved level-streaming cost; do not "fix" it again.
- **Already at 720p.** `pc-settings.gc` on the card has `(game-size 1280 720)`,
  `(msaa 1)`, `(fps 30)`, `(lod-force-tfrag 2)`, `(lod-force-tie 2)`,
  and `data/log/jak2.*.log` shows `FBO Setup: requested 1280x720, msaa 1`.
  The `create_window 1920x1080` line is the presentation swapchain only
  (`game/graphics/pipelines/opengl.cpp:277`); FIX 7f deliberately stopped
  resizing it — **do not** try to resize the swapchain again.
- `buckets` stays ~29 ms no matter what is on screen → suspect draw-call /
  GL-state / submission overhead, not shading. Prove it (task 1 + 2).

Symptoms reported by the user, in priority order:
1. **Tutorial hints (e.g. stadium jetboard) → ~10 fps, slow motion.**
2. **Zoomer riding → heavy frame drops.**
3. **New areas in the city struggle to load.**

---

## 1. HARD CONSTRAINTS — violating these has already broken the game 3 times today

1. **Never create a thread after boot.** `std::thread` on devkitA64 does not
   throw on failure; it takes the process out via `_exit(1)`. Proof in
   `gk_run_log.txt`: `[MC] starting async memcard worker...` → 10 ms later
   `[exit] _exit(1)`. try/catch does NOT help (FIX 34b/34c). The process holds a
   fixed 3.2 GB reservation with ~4 MB free (`mem_used=3261548KB/3265536KB`,
   constant from t=0.008), so there is no room for a thread stack. If background
   work is unavoidable, use libnx `threadCreate()` with a **statically
   preallocated, page-aligned stack** (it returns a `Result` instead of killing
   the process) + libnx `Mutex`/`CondVar`, or push the job onto an
   **already-running** thread (the overlord/ISO thread has a message queue).
2. **Never change the texture upload path.** Band/chunked `glTexSubImage2D`
   crashed nouveau (FIX 33), `GL_UNSIGNED_BYTE` instead of
   `GL_UNSIGNED_INT_8_8_8_8_REV` hung the GPU hard with no crash log (FIX 34a).
   Atomic `glTexImage2D` + `GL_UNSIGNED_INT_8_8_8_8_REV` is the only sanctioned
   path on Switch. Same applies to any "faster" upload idea for buffers.
3. **One change per hardware test**, and every build must contain a **new log
   marker or a new md5** you verify with `strings`/`md5` *after* copying to the
   card. A previous session silently redeployed an identical NRO and wasted a test.
4. **`cmake --build … --target gk` does NOT produce the NRO** — the target is
   `gk_nro`. Use the build commands in §6 which do it correctly.
5. Keep all Switch-specific behaviour behind `#ifdef __SWITCH__`; desktop
   behaviour must not regress (host build is the fast correctness check).
6. Commit messages and any docs must carry `(AI-assisted)`. Never open issues/PRs.

---

## 2. TASK 1 (do first, ~30 min) — make jak2 report per-bucket cost

**The instrumentation already exists and jak2 simply never runs it.**
`game/graphics/opengl_renderer/OpenGLRenderer.cpp`:
- `g_bucket_prof` (line ~78), `SwitchBucketProf`, `g_bucket_prof_started` (~81)
- per-bucket timing + the 2-second `[buckets]` report live **inside
  `dispatch_buckets_jak1()`** (lines ~1522–1640)
- **`dispatch_buckets_jak2()` starts at line ~1641 and has none of it** — that
  is why `gk_run_log.txt` contains zero `[buckets]` lines even though the
  format strings are present in the NRO.

**Do:** factor the FIX 12 instrumentation out of `dispatch_buckets_jak1` into
small helpers (e.g. `switch_bucket_prof_begin()` / `_end(bucket_id, name, ms)` /
`switch_bucket_prof_report()`) and call them from **jak2 and jak3** too. Keep the
existing output format:

```
[buckets] N frames, all buckets XX.XXms/frame -- worst:
[buckets]   <name>   avg  X.XXms  max  X.XXms  ( XX.X%)
```

Also add, in the same report (cheap counters incremented in the renderers'
draw paths, Switch-only): **draw calls per frame** and **triangles per frame**,
globally and for the top buckets. Without draw counts you cannot tell
"expensive pixels" from "too many draws".

**Acceptance:** after one boot, `gk_run_log.txt` contains `[buckets]` lines
every 2 s while playing jak2.

---

## 3. TASK 2 (same build as Task 1) — settle pixel-bound vs draw-call-bound

Add a Switch-only, automatic resolution A/B so the user does not have to fiddle
with menus:

- every 15 s, alternate `Gfx::g_global_settings.game_res_w/h` between
  `1280x720` and `960x540` (44 % of the pixels). The FBO path already rebuilds
  on size change — see `OpenGLRenderer.cpp:1400` (`m_fbo_state.render_fbo->matches(...)`)
  and the `FBO Setup: requested WxH` log line.
- log `[resab] res=WxH render=XX.XXms buckets=XX.XXms draws=N tris=N` each time
  a phase ends.
- Gate it behind a compile-time flag (e.g. `SWITCH_RES_AB`) so it can be removed
  in the shipping build.

**Interpretation, decided by the numbers, not by taste:**
- render time drops roughly with pixel count → **fill/pixel bound** → implement
  **dynamic resolution scaling** (shrink `game_res` when the 8-frame average
  frame time exceeds ~30 ms, restore when it recovers; hysteresis, min 960x540)
  and this alone likely delivers 30 fps.
- render time barely moves → **draw-call / submission bound** → go to Task 4.

---

## 4. TASK 3 (independent of the above; fixes the reported #1 symptom) — tutorial-hint freeze, without threads

Jak 2 auto-saves on **every** tutorial hint. Each save is a synchronous
`open → 1 KiB header → 128 KiB payload → 1 KiB footer → fsync → close` on the
GOAL thread, measured at **125–276 ms** in `mc-trace.txt` — that is the "slow
motion while the jetboard tutorial text is on screen".

File: `game/kernel/common/kmemcard.cpp`. Today, after FIX 34c, `MC_run()`
dispatches to `mc_dispatch_save_async()` / `mc_dispatch_load_async()`, which on
Switch fall through to running `mc_worker_save()` / `mc_worker_load()` **inline**
and then call `mc_apply_async_result()`. The request/result structs
(`McSaveRequest`, `McLoadRequest`, `McAsyncResult`) and the raw-buffer checksum
(`mc_checksum_bytes`) already exist and are exactly what you need.

Implement **both** of the following:

**(a) Skip redundant saves.** Before writing, compute
`mc_checksum_bytes(req.bank_data.data(), BANK_SIZE[g_game_version])` and compare
with the checksum of the last successfully written save for that file index. If
identical, report `McStatusCode::OK` immediately without touching the SD card
(still update the in-memory `mc_files[...]` preview/save-count bookkeeping so
GOAL sees a normal, successful save). Tutorial hints fire repeated near-identical
saves; this removes most of them outright. Log `[MC] save skipped (unchanged)`.

**(b) Frame-sliced writes.** Convert the save into a state machine driven by
`MC_run()` (which GOAL calls once per frame):
`OPEN → WRITE_HEADER → WRITE_PAYLOAD (N KiB per call) → WRITE_FOOTER → FSYNC →
CLOSE → APPLY`. Between steps, leave `op.operation` as-is and `op.result` as
BUSY and simply `return` — the GOAL save code already waits on BUSY for seconds
(that is how a real PS2 memcard behaves). Start with **16 KiB per frame**
(≈8 frames, ~1–2 ms each) and make the chunk a named constant so it can be tuned.

Invariants that must not change (compare against `git show cacbc33f3:game/kernel/common/kmemcard.cpp`):
- bank alternation (`last_saved_bank ^ 1`), save-count increment, the
  "reserve 0, use 1" rule for a first save;
- header and footer are identical `McHeader`s, checksum over the payload only;
- every FS operation stays inside `SWITCH_FS_LOCK()` (FIX 7u — the overlord ISO
  thread races us inside newlib's non-thread-safe fsdev layer);
- the existing 3-attempt retry with a 100 ms backoff must still work; on a mid-
  transaction failure, close the FILE*, reset the state machine and restart the
  attempt from OPEN;
- on failure after all attempts → `op.result = McStatusCode::INTERNAL_ERROR`;
- loads keep the bank verification logic in `mc_worker_load()` untouched
  (header/footer save counts, magic, checksum, freshest-bank pick, `NEW_GAME`
  and `READ_ERROR` results, `mc_last_file` latching on OK **and** NEW_GAME).

**Host verification before any console test** (this is why this task is cheap):
run the host build against `out/jak2`, save/load in game, and confirm the
produced `bank0.bin` / `bank1.bin` are **byte-identical** to files produced by
the pre-change build for the same save, and that loading them works.

**Acceptance on console:** `mc-trace.txt` shows the sliced steps and a per-step
cost of a few ms; no frame in `[fps]` worst-case exceeds ~40 ms during a
tutorial hint sequence.

---

## 5. TASK 4 (only after Task 1+2 data) — the ~29 ms of buckets

Pick work strictly by what the `[buckets]` report indicts. Likely candidates in
the jak2 city, with the specific angle for each:

- **`use-vis? #f` is set in the card's `pc-settings.gc`.** Visibility data off
  means dramatically more geometry submitted every frame. Find out *why* it is
  off for this port (search the notes and `goal_src` for `use-vis`), and whether
  it can be re-enabled — this is potentially the single biggest win and costs
  nothing to test.
- **Merc2** (`foreground/Merc2*.cpp`): jak2 city is character-heavy. Look for
  per-draw uniform uploads and per-effect buffer binds that can be hoisted or
  batched; check `Merc2::flush_draw_buckets`-style paths for one-draw-per-effect
  patterns.
- **Tie3 / TFragment / Shrub**: per-tree draws and per-draw texture/state
  changes; consider merging draws that share a texture+mode, and skipping
  trees that contribute nothing after culling.
- **State changes**: a draw-mode cache was tried before (commit `896c045fd`) and
  measured ~0 gain — do not repeat it blind; only revisit with the counters
  showing redundant state calls.
- **`lod-force-tfrag/tie 2`** are already forcing lower LODs; check what else
  the PC settings expose (`ps2-lod-dist?`, `force-envmap?` — envmap is ON in the
  card's settings and is expensive; measure it).

Each change: measure with the `[buckets]` report on the **same** save spot,
before and after, and record the numbers in `SWITCH_PORT_SESSION_NOTES.md`.

---

## 6. TASK 5 — the other half: `wait_dma` ≈ 18 ms (GOAL simulation)

Even with a free GPU, 18 ms of simulation caps the game at ~35 fps and is what
makes tutorials feel like slow motion. Profile the GOAL side (the engine has
per-process timing; there is also the `*profile*`/`profile-bar` machinery in
`goal_src/jak2/engine/debug/`). Capture in three situations: stadium tutorial
hint, zoomer ride, quiet city street. Suspects: hint/text processing, traffic and
ambient spawning, and the auto-save above (Task 3 removes that one).

Then consider a **dynamic quality system** on the GOAL side (jak 2 already has
`*frame-rate-options*` plumbing from FIX 30): when the frame time average misses
budget, shed particles/sprites, ocean detail, shadow resolution and the depth-cue
/ slow-time post effects; restore them when it recovers. Goal: degrade visuals,
never the clock.

---

## 7. Build / deploy / verify (exact commands)

Host (fast correctness check, macOS arm64 — `build-host` is stale, use this one):
```bash
cmake --build build-host-fix33 --target gk -j 8
```

Switch NROs (docker, devkitA64) — **this is the only correct way**:
```bash
docker run --rm -v "$PWD:/work" -w /work \
  -e BUILD_DIR=/work/build-switch-jak2 -e SWITCH_GAME=jak2 \
  devkitpro/devkita64:latest bash scripts/build-switch.sh
docker run --rm -v "$PWD:/work" -w /work \
  -e BUILD_DIR=/work/build-switch -e SWITCH_GAME=jak1 \
  devkitpro/devkita64:latest bash scripts/build-switch.sh
```

Deploy + verify (SD mounts as `/Volumes/SWITCH SD`):
```bash
mkdir -p backups/pre-fix35 && cp "/Volumes/SWITCH SD/switch/jak2/Jak 2.nro" backups/pre-fix35/
cp build-switch-jak2/game/gk.nro "/Volumes/SWITCH SD/switch/jak2/Jak 2.nro"
cp build-switch/game/gk.nro      "/Volumes/SWITCH SD/switch/jak1/Jak 1.nro"
sync
md5 -q build-switch-jak2/game/gk.nro "/Volumes/SWITCH SD/switch/jak2/Jak 2.nro"   # must match
strings -a "/Volumes/SWITCH SD/switch/jak2/Jak 2.nro" | grep -c "<your new marker>"  # must be > 0
```

Known-good rollback NROs: `backups/pre-fix34b/jak2-gk.nro`, `backups/pre-fix34/`,
`backups/pre-fix33/` (each with md5s recorded in the notes).

## 8. Logs to read after each console run

On the card, `sdmc:/switch/jak2/`:
- `gk_run_log.txt` — `[fps]`, `[phase]`, `[buckets]`, `[resab]`, `[gfx] alive`,
  `[exit]`, `[FATAL]`. **Everything stopping at once with no `[FATAL]` = GPU
  hang or `_exit`, not a CPU crash.**
- `gk_stdout.txt` — loader lines (`[loader] live=…`, `tex stage`), blackout loads.
  Truncated on every boot.
- `mc-trace.txt` — append-only, unbuffered `write()`, survives crashes. The
  memory-card ground truth.
- `gk_fatal.txt` — CPU exception contexts; check whether the last entry is
  actually **new** (it is append-only and full of old entries) before blaming it.
- `data/log/jak2.*.log` — `FBO Setup: requested WxH`, engine-side logging.

## 9. Definition of done

- Stadium jetboard tutorial: no sub-20 fps stretch, no slow motion; `mc-trace.txt`
  shows either a skipped save or sliced writes of a few ms each.
- Zoomer through the city, 3+ area transitions: ≥ 25 fps sustained, no crash,
  `[loader] live=` not climbing.
- jak1 still boots and plays (both NROs are built from the same source).
- `SWITCH_PORT_SESSION_NOTES.md` updated with measurements before/after for every
  change, and every commit tagged `(AI-assisted)`.
