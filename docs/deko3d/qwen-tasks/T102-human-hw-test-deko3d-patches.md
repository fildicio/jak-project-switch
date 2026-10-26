# T102 — (human) Hardware test of the patched deko3d (T101)

Not a Qwen task. Do this after T101 is ✅ reviewed.

1. Reviewer notes the T101 md5, deploys as usual (`sdmc:/switch/jak1/jak1.nro`, old one
   rotated to `.bak`).
2. Play **≥ 20 minutes** in one session, including **Sentinel Beach** (ocean) and a few
   level loads. Copy `gk_run_log.txt` to `hw-logs/<date-time>/`.
3. **Leak fix (patch 2)** — compare the `heap free N MB` value in the `[dk]` perf lines
   at ~1 min and at the end of the session:
   - Expect: roughly flat (drops only at level loads, recovers afterwards).
   - Compare with an older long session in `hw-logs/` on the stock library (same
     game, similar length): stock should show a slow downward trend, patched shouldn't.
4. **Alpha fix (patch 1)** — look at:
   - **The reported Sentinel Beach symptom set** (2026-10-24 diagnosis, session
     notes): a **black spot** in the water + a **blue line across the player** at
     the waterline, both gone when walking away from the beach. Diagnosis: the
     near-ocean b1/b2 alpha bookkeeping was corrupted by the stock-lib
     `FuncAlphaDst` bug — the near band's framebuffer alpha clamped to 1.0 instead
     of the ~0.15 wave pattern, so the `DstAlpha,ONE` envmap pass fired ~6.5× too
     strong, and the mid envmap's stamp-α-to-0 overlap trick never happened. On the
     patched lib expect: blue line across the player **gone**, black spot **gone**,
     near-water envmap shine visibly **weaker / closer to GL** — a big change here
     is the fix working, not a regression.
   - Ocean envmap shine at Sentinel Beach (reads framebuffer alpha via `DstAlpha`):
     describe it vs the previous build (stronger / weaker / same) and vs PC/GL if you can.
   - Transparent effects (water, glass, particles, HUD): anything that changed.
   Changes here are expected — alpha now matches what the code asks for (GL parity).
   Describe in words; don't attach images (text-only models).
5. Any `[dk] dk error`, black/garbled screen, or crash: copy `gk_run_log.txt` /
   `gk_fatal.txt`, redeploy the `.bak`, and send the logs to the reviewer.
6. Pass → reviewer marks T101/T102 ✅, adds a plan §0 line, commits `(AI-assisted)`.
   If a visual regression is traced to patch 1 alone, the reviewer can revert just that
   one line in `third-party/deko3d/source/maxwell/gpu_3d_state.cpp` and keep patch 2.
