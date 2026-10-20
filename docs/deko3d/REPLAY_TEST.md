# Replay test: check deko3d CPU code on the Mac (AI-assisted)

**Why:** a deko3d renderer has a CPU half (walk the game's DMA chain, cull, build index lists and
draw states) and a GPU half (record deko3d commands). The CPU half is most of the code and is
plain C++. The replay test runs it on the Mac against one frame saved on the Switch, so the coder
model can change or port it and get PASS/FAIL in a minute, without an SD-card trip.

## 1. Make a dump (human, once per game/level)

1. SD card: `sdmc:/switch/jak1/` must have `gk_dk3d.txt`, `gk_dkload.txt`, `gk_dktfrag.txt`.
   **The dump only arms on the deko3d path.** These switch files must also exist:
   `gk_dk3d.txt` (deko3d on), `gk_dkload.txt` (dk loader), `gk_dktfrag.txt` (tfrag pass).
   Without them the game silently runs OpenGL and no dump is written. Never move
   `gk_*.txt` files in bulk: some of them are switches, not logs (logs are
   `gk_boot_log/gk_run_log/gk_stdout/gk_fatal`). Lesson from 2026-10-20.
2. Create `sdmc:/switch/jak1/gk_dkdump.txt`. Optional content: a number of frames to wait
   (default `300`, about 10 s after the world first shows).
3. Boot, wait ~20 s in the level, quit. `gk_run_log.txt` shows
   `[dk] replay dump: ... ok` and `[dk] replay golden: ... N draws ok`.
4. Copy `dk_replay.dma` and `dk_replay.golden.txt` from `sdmc:/switch/jak1/` to
   `test/deko3d/replay/jak1/`. Delete `gk_dkdump.txt` (otherwise every boot dumps again).

The golden is the draw list built by the **hardware-proven** code, so it is the reference.

## 2. Run the test (anyone, any time)

```
cmake --build build-host --target goalc-test -j8
./build-host/goalc-test --gtest_filter='DkTfragReplay.*'
```

- `dk-replay: N golden lines, 0 differ -> PASS` is the pass line.
- `SKIPPED` means no dump in `test/deko3d/replay/jak1/` (or no `out/jak1/fr3`).
- On a failure, the first 10 differing lines are printed, and the full host output is in
  `build-host/dk_replay.host.txt`. Diff it against the golden.
- `DK_REPLAY_DIR=<dir>` runs another dump folder.

## 3. What it checks / what it doesn't

| Checked | Not checked |
|---|---|
| bucket walk, level lookup, cameras (`make_new_cam_mat`) | anything on the GPU (shaders, pipelines, barriers) |
| frustum culling + index lists (FNV hash per tree) | time-of-day colours (only that a palette exists) |
| per-draw texture, index range, strips, depth/blend/alpha state | texture contents |

GPU bugs still need the hardware. CPU bugs (the bigger part) are caught here.

## 4. How it is wired

- `TFragmentDk.cpp` compiles on the host when `DK_HOST_REPLAY` is defined (the
  `goalc-test` target defines it). The test file defines the 5 Switch-only helpers (buffer pool,
  copy, two texture calls, loader-buffer lookup) and loads levels straight from `out/jak1/fr3`.
- `dktf::set_golden(&str)` makes `build_frame` append the golden text. The Switch build writes
  the same text on the dump frame, so the two outputs compare line for line.
- Golden format v1: `fog`, then per pass `pass/camera/pc_camera/hvdf_trans_fog`, per tree
  `tree ... hash`, per draw `draw ...`, and a final `sum` line. Only change the format together
  with a new dump.

## 5. Using it for the next renderer (tie, shrub, jak2/jak3)

Same three steps: add `gold(...)` lines to that renderer's CPU half, run it under
`DK_HOST_REPLAY`, and take one hardware dump once its first version is hardware-proven. After
that, every refactor/port of it is checked on the Mac.
