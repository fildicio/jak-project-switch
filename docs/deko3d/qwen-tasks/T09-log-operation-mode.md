# T09 — Log docked / handheld changes

**Goal:** first step of plan P1.2b. Detect docked ↔ handheld while the game runs and log
it. No resize yet (that's T11) — just make the event visible in `gk_run_log.txt`.

**Edit (3 small changes in 3 files):**

1. `game/graphics/pipelines/dk_device.h` — inside `namespace dkdev`, add:
   ```cpp
   //! 1 = docked (TV), 0 = handheld. Polled once per frame (cheap, no IPC wait).
   int operation_mode();
   ```
2. `game/graphics/pipelines/dk_device.cpp` — implement it (this file may use libnx):
   ```cpp
   int operation_mode() {
     return appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0;
   }
   ```
3. `game/graphics/pipelines/dk_renderer.cpp` — in `DkDisplay`:
   - add member `int m_op_mode = -1;`
   - at the end of `process_events()`:
     ```cpp
     const int mode = dkdev::operation_mode();
     if (mode != m_op_mode) {
       switch_run_logf("[dk] operation mode: %s (swapchain stays at boot size until P1.2b resize)",
                       mode ? "docked" : "handheld");
       m_op_mode = mode;
     }
     ```
   `dk_renderer.cpp` must **not** include `<switch.h>`; it only calls `dkdev::`.

**Done-check:** `scripts/ai/check-switch.sh jak1` → `PASS`.
