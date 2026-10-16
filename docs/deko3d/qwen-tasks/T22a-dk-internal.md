# T22a — dk_device_internal.h: dk-TU internals pass-throughs (P2.3)

Small first slice of the old T22 (split 2026-10-16: one big session timed out at model
level; slices are T17-sized). T22b/T22c build on this file.

## Rules
- Only dk_*.cpp includes <deko3d.h>; this new header is dk-INTERNAL (also allowed to,
  it is never included outside #if defined(__SWITCH__) code).
- STOP rule: any command failure → append only its last 5 lines, no self-fixing, STOP.

## Edits
1. NEW `game/graphics/pipelines/dk_device_internal.h`:
   `#pragma once`, `#if defined(__SWITCH__)`, `#include <deko3d.h>`, in `namespace dkdev`:
   `DkDevice device_handle(); DkQueue queue_handle(); DkCmdBuf cmd_handle();`
   `uint8_t* staging_cpu(); DkGpuAddr staging_gpu(); uint8_t* data_cpu();`
   `uint32_t upload_cmd_off(); uint32_t upload_cmd_size();`
   `void submit_upload_cmds(DkCmdList list);  // submit + SignalFence(s_uploadFence,true) + dkFenceWait(-1)`
2. `game/graphics/pipelines/dk_device.cpp`: include the internal header; implement all
   of the above as one-liners over existing module state (`s_dev, s_queue, s_cmd,
   s_dataMem + kOffStaging / kOffUpload / kCmdUpload, s_uploadFence`).
   `submit_upload_cmds` is exactly the T18 tail:
   `dkQueueSubmitCommands(s_queue, list); dkQueueSignalFence(s_queue, &s_uploadFence, true); dkFenceWait(&s_uploadFence, -1);`

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` then `scripts/ai/check-tail.sh` → DONE-PASS.
Inert: no callers of these yet. QWEN_REPORT `## T22a` (files + last 5 log lines + md5),
README row → 🟡. No commit, no deploy.
