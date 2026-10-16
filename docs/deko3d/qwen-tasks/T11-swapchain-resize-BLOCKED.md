# T11 — Swapchain resize on dock/undock — ⛔ BLOCKED (needs reviewer decision)

Do not start. The reviewer must first choose the memory approach, then rewrite this file.

**Problem:** the framebuffer memblock is sized at boot (`fbSize` × 2 for the boot
resolution). Docked wants 1920×1080, handheld 1280×720.

**Options for the reviewer:**
- A. At boot, size the fb memblock for 1920×1080 always (+~8 MB, heap is tight: FIX 34c,
  24 MB headroom rule) and re-make images + swapchain in place on mode change.
- B. Destroy + re-create the fb memblock on mode change (fragmentation risk on a heap
  with ~4 MB spare).
- C. Keep the boot size, scale with the GPU (no realloc) — same as GL FIX 73c today.

Then a Qwen task: `dkdev::resize(int w, int h)` = `wait_idle` → `dkSwapchainDestroy` →
re-init images → `dkSwapchainCreate`, called from the T09 mode-change branch.
