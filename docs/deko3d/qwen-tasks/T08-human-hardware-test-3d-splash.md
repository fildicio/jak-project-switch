# T08 — (human) Hardware test of the 3D splash

Not a Qwen task. Do this after Gate P1 has passed on hardware.

1. Reviewer sets `kSplash3D = true` in `dk_device.cpp`, runs
   `scripts/ai/check-switch.sh jak1`, notes the md5.
2. Deploy as usual (`sdmc:/switch/jak1/jak1.nro`, old one rotated to `.bak`),
   `sdmc:/switch/jak1/gk_dk3d.txt` present.
3. Expect: splash looks the same as the 2D blit (aspect-correct, centered, sharp),
   then the blue pulse. `gk_run_log.txt` has `[dk] shaders: ...` and no `[dk] dk error`.
4. If the screen is black or garbled: copy `gk_run_log.txt` / `gk_fatal.txt`, set
   `kSplash3D = false` again.
5. Pass → keep `true`, update plan §0 row P1.4 with "3D splash ✅", commit `(AI-assisted)`.
