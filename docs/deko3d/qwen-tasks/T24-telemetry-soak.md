# T24 — Telemetry soak: tex/pool/ring stats + 2-min hardware protocol (P2.3)

**Prerequisite: T23 must be ✅ reviewed first.** Adds the P2.3 telemetry lines the
hardware soak reads, so the next deploy verifies the whole batch in one run.

## Read first
- `game/graphics/pipelines/dk_renderer.cpp` — find the existing periodic `[dk]` fps log
  line (per-second cadence) and piggyback on its exact timing/guard pattern.
- Accessors available: `dkdev::ubo_ring()` (used/high_water/failures/segment_size),
  `dkdev::buffer_pool()` (outstanding/pooled_chunks/pooled_bytes/region_unused_bytes/
  failed_allocations — see dk_buffer_pool.h), `dktexgpu::` telemetry (T22:
  live_count/high_water/failed_allocs/image_bytes_used).

## Rules
- Only `dk_*.cpp` files include deko3d/libnx; `dk_renderer.cpp` is dk-owned (editable).
- Log format: `[dk]` prefix, one line, numbers with units where sensible.
- STOP rule: on any command failure append ONLY the last 5 lines, no self-fixing, STOP.

## Edits

### `game/graphics/pipelines/dk_renderer.cpp`
Next to the existing periodic `[dk]` line, add ONE more per-second line (only when the
relevant objects exist — null checks like the rest of the file):
`[dk] tex live=%u high=%u fail=%d | pool out=%u pooled=%u fail=%d | ring used=%u/%u hw=%u`
- tex: dktexgpu telemetry; pool: dkdev::buffer_pool() telemetry; ring: ubo_ring() used /
  segment_size / high_water.
- If a subsystem is inactive (e.g. buffer_pool() returns nullptr), print `pool -` for
  that group — never crash, never spam per frame.

### `docs/deko3d/` — no new docs required; the soak protocol below goes in your report.

## Done-check (background wrappers ONLY)
```
scripts/ai/check-bg.sh jak1
scripts/ai/check-tail.sh
```
QWEN_REPORT `## T24`: files changed, the exact new log line as it will appear, last 5
log lines + md5. Flip README row to 🟡. No commit, no deploy.

## Hardware soak (USER step, after review — same protocol as T08)
Deploy the reviewed NRO + `gk_dk3d.txt`, run 2 minutes, then read
`gk_run_log.txt`: the `[dk] tex` line must appear at 1 Hz, `fail=` fields must stay 0,
no `UNUSABLE` lines, and `[dk] tex live` should grow during level load then stabilize.
Paste the log excerpt back to the reviewer.
