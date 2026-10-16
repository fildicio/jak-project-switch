# T21 — BufferPool region: dedicated 8 MB memblock in dk_device (P2.3)

Goal: give `dkpool::BufferPool` (T17) a real GPU region so P3 level-data staging can
pool chunks in deko3d memory instead of GL buffer churn (FIX 33). **Inert**: nothing
acquires yet (first consumer is P3); this task only creates the region and the accessor.

## Rules (read first)
- Only `dk_*.cpp` files include `<deko3d.h>`/`<switch.h>`; headers stay plain C++.
- Read `game/graphics/deko3d_renderer/dk_buffer_pool.h` BEFORE writing — use its API
  as-is (`init(base, gpu_base, size)`, telemetry accessors). Do not edit that file.
- Never edit `game/graphics/opengl_renderer/` or any desktop path.
- Log format: `[dk]` prefix, one line.
- STOP rule: if any command fails, append ONLY its last 5 lines to your report, do NOT
  attempt fixes, do NOT paste long logs — report STOP instead. Long failed logs kill
  this session.

## Edits (2 files, nothing else)

### `game/graphics/pipelines/dk_device.cpp`
1. Include `game/graphics/deko3d_renderer/dk_buffer_pool.h` (plain header, fine here).
2. Constant near the other pool constants:
   `constexpr uint32_t kPoolBytes = 8 * 1024 * 1024;  // T21: 8 MB = 32 x 256 KB size classes`
   (already 0x1000-aligned = DK_MEMBLOCK_ALIGNMENT multiple; do NOT add rounding math).
3. Module state next to `s_uboRing`:
   `static DkMemBlock s_poolMem = nullptr;` and `static dkpool::BufferPool s_bufferPool;`
4. In `init()`, right after the UBO ring block: create the memblock with the SAME maker
   pattern/flags as `s_dataMem` (`DkMemBlockMaker` → `CpuUncached | GpuCached`, size
   `kPoolBytes`), then `s_bufferPool.init(cpu, gpu, kPoolBytes)` with the cpu/gpu bases
   from `dkMemBlockGetCpuAddr/GetGpuAddr`. Log success
   `[dk] buffer pool 8 MB ready (32 chunks x 256 KB)` or failure
   `[dk] buffer pool UNUSABLE (memblock failed) — continuing without pool` (never fatal,
   same style as the UBO ring log). Include the block in `s_pool_bytes` accounting.
5. In `shutdown()`: `dkMemBlockDestroy(s_poolMem)` following the existing teardown order
   for `s_dataMem`, and null the handle. The pool object itself needs no teardown call.
6. Accessor next to `ubo_ring()`:
   `dkpool::BufferPool* buffer_pool() { return is_up() ? &s_bufferPool : nullptr; }`

### `game/graphics/pipelines/dk_device.h`
- Include `game/graphics/deko3d_renderer/dk_buffer_pool.h` (plain, keeps file libnx-clean)
  and declare, with a P2.3 doc comment (nullptr when device down; callers must skip
  uploads when null — FIX 33 caller contract):
  `dkpool::BufferPool* buffer_pool();`

## Done-check (background wrappers ONLY, never foreground)
```
scripts/ai/check-bg.sh jak1     # starts the Docker build in the background
scripts/ai/check-tail.sh        # poll until DONE-PASS / FAIL
```
Then verify inertness: `grep -rn 'buffer_pool()' game/ --include='*.cpp'` must show the
dk_device.cpp definition only (no callers). Append to QWEN_REPORT.md a `## T21` section:
files changed, the last 5 lines of the check log, the md5 line, and the grep result.
Flip your README row to 🟡. Do NOT commit, do NOT deploy.
