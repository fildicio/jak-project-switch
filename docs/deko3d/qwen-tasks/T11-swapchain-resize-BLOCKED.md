# T11 — Swapchain resize on dock/undock — ⏸ DEFERRED (reviewer decision 2026-10-17)

**Decision: Option C for P1/P2** — the swapchain keeps its boot size on dock/undock.
No Qwen work on this until it is revived (see below).

**Problem:** the framebuffer memblock is sized at boot (`fbSize` × 2 for the boot
resolution). Docked wants 1920×1080, handheld 1280×720.

**Options for the reviewer:**
- A. At boot, size the fb memblock for 1920×1080 always (+~8 MB, heap is tight: FIX 34c,
  24 MB headroom rule) and re-make images + swapchain in place on mode change.
- B. Destroy + re-create the fb memblock on mode change (fragmentation risk on a heap
  with ~4 MB spare).
- C. Keep the boot size, scale with the GPU (no realloc) — same as GL FIX 73c today.

**Why C (measured on hardware 2026-10-17):**
- Parity: the GL path behaves the same (FIX 73c — size fixed at boot).
- No benefit yet: the engine pins output at 1280x720 (`pc_get_active_display_size`
  override, Gate P1 run log); a 1080p swapchain would only add ~8 MB and upscale anyway.
- Risk: resize = queue drain + fences + memblock/image re-init — our most fragile
  lifecycle code, exercised on exactly the dock path that also carries T10 focus changes.
- Heap is no longer the constraint (2937 MB free at steady state, P0.6) — when we do
  resize, **Option A** (size fb for 1920x1080 at boot) is affordable.

**Revival:** as a design item inside the P3 render-target work (pick internal-res vs
output-res once, properly). Then a Qwen task: `dkdev::resize(int w, int h)` = `wait_idle` →
`dkSwapchainDestroy` → re-init images → `dkSwapchainCreate`, called from the T09 mode-change branch.
