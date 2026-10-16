# T10 — Stop presenting while the game is out of focus

**Goal:** plan P1.2b focus/suspend. When the HOME menu or sleep takes focus, stop
submitting GPU work and drain the queue once, so nothing is in flight while suspended.

**Edit (3 files, small):**

1. `dk_device.h` (namespace `dkdev`):
   ```cpp
   //! true when this applet has focus (not in HOME menu / overlay / sleep).
   bool in_focus();
   //! Wait until the GPU queue has finished all submitted work.
   void wait_idle();
   ```
2. `dk_device.cpp`:
   ```cpp
   bool in_focus() { return appletGetFocusState() == AppletFocusState_InFocus; }
   void wait_idle() { if (s_up) dkQueueWaitIdle(s_queue); }
   ```
3. `dk_renderer.cpp`, `DkDisplay`:
   - member `bool m_focused = true;`
   - in `render()`, find the single call to `dkdev::present_frame(...)`. Just before it:
     ```cpp
     const bool focused = dkdev::in_focus();
     if (focused != m_focused) {
       switch_run_logf("[dk] focus %s", focused ? "regained" : "lost: pausing present");
       if (!focused) dkdev::wait_idle();
       m_focused = focused;
     }
     ```
     and only call `present_frame` when `focused`. **Keep the engine handshake
     (frame index, `sync_cv` notify, chain acknowledge) running exactly as now**, even
     when not focused — the game must not deadlock waiting for a frame. Read `render()`
     fully first and explain in your report where the handshake is and why your change
     doesn't skip it.
   - Today the line is `const dkdev::FrameTiming ft = dkdev::present_frame(show_splash);`
     and `ft` feeds the `[dk]` stats below it. Make it `dkdev::FrameTiming ft{};` +
     `if (focused) ft = dkdev::present_frame(show_splash);` and skip adding to the stats
     when not focused.
   - When not presenting, sleep ~16 ms (`std::this_thread::sleep_for`, add `#include <thread>`
     and `<chrono>` if missing) so the loop doesn't spin.

**Done-check:** `scripts/ai/check-switch.sh jak1` → `PASS`. Paste the edited part of `render()`.
