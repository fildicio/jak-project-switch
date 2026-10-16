# T20 — UBO ring: real per-frame uniform memory (P2.4)

**Goal:** P2.4. Grow the reserved-but-tiny UBO area in the data memblock into a real
per-frame ring and expose it. `dkring::FrameRing` (T06, already reviewed + gtested)
does the logic; this task wires it into `dk_device`. Nothing allocates from it yet
(P3 renderers will) — but it must be live at boot: allocated, advanced every frame,
telemetry logged.

**Read first:** `game/graphics/deko3d_renderer/dk_ring.h` (the whole file — note the
invariant: `begin_frame` may only be called once the frame's fence has been waited
on), and in `game/graphics/pipelines/dk_device.cpp`: the constants block
(`kOffUbo`, `kUboBytes` — currently only 0x100 bytes for one SplashParams),
`present_frame` (find the slice fence wait and the frame counter it uses), and `init`.

**Edit (2 files):**

1. `dk_device.cpp`:
   - `kUboBytes`: `0x100` → `0x80000` (512 KB total = 2 frames × 256 KB segments).
     The existing `static_assert(kOffUbo % DK_UNIFORM_BUF_ALIGNMENT == 0)` must still
     hold (it does — the offset doesn't move). `kDataPool` grows accordingly; that is
     expected (heap budget: +0.5 MB of ~3 GB free — fine, note it in the log line).
   - Add `#include "game/graphics/deko3d_renderer/dk_ring.h"` (plain header — allowed)
     and `static dkring::FrameRing s_uboRing;`.
   - In `init`, after `s_dataMem` is confirmed non-null:
     `s_uboRing.init(cpu_base + kOffUbo, gpu_base + kOffUbo, kUboBytes, kFbNum);`
     then one log line either way:
     `[dk] ubo ring %u KB x %u frames, seg %u KB` or on an unusable ring
     `[dk] ubo ring UNUSABLE (init failed) — continuing without UBO ring`
     (never fatal; the game still boots and draws the splash).
   - In `present_frame`, immediately AFTER the existing per-slice
     `dkFenceWait(&s_sliceFence[slice], -1)` (the comment says "this slice's previous
     commands are done"), call `s_uboRing.begin_frame(<the same frame counter the
     slices use>)`. Grep for how the counter is derived and reuse it — do not invent
     a second counter.

2. `dk_device.h`: add `#include "game/graphics/deko3d_renderer/dk_ring.h"` (it is a
   plain, deko3d-free header — the file stays libnx-clean) and inside `namespace dkdev`:
```cpp
//! P2.4: the per-frame UBO ring carved from the data memblock. nullptr when the
//! device is down or the ring is unusable. Call begin_frame() happens inside
//! present_frame; renderers alloc() with align = DK_UNIFORM_BUF_ALIGNMENT (0x100).
dkring::FrameRing* ubo_ring();
```
   Implement it in dk_device.cpp (`return is_up() ? &s_uboRing : nullptr;`).

**Why a fence-gated ring and not one big buffer:** uniform data of frame N must stay
valid until frame N's GPU work finishes; segments = in-flight frames, and
`begin_frame` is only safe after the slice fence wait (the same guarantee the frame
slices already rely on). This is the dk_ring.h contract — do not weaken it.

**Done-check** (Docker build — wrappers only):
```sh
scripts/ai/check-bg.sh jak1
# repeat until DONE-PASS / DONE-FAIL:
scripts/ai/check-tail.sh
```
Expected: `DONE-PASS`. Hardware check happens in the next play session: the boot log
must contain `[dk] ubo ring 512 KB x 2 frames, seg 256 KB`.

**If the done-check is not DONE-PASS: STOP.** Append to your QWEN_REPORT.md section
the last 5 lines of `check-tail.sh` output and what you had changed — do not attempt
further fixes and do not read the whole build log into your session (long logs kill
the session).

**When done:** flip your row in `docs/deko3d/qwen-tasks/README.md` to 🟡 and append a
`## T20` section to `docs/deko3d/qwen-tasks/QWEN_REPORT.md` (what you did + the
done-check last lines).
