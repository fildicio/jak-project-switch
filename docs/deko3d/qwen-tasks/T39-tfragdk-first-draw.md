# T39 — Live wiring: chain → world recording in present_frame (P3.2, AI-assisted)

Track D P3.2 · Depends on: T38 ✅ + **P2.3 soak PASS (hard gate — this task goes LIVE)** ·
One session · First behavior change since T24: expect a NEW NRO md5.

## Goal

Connect the engine to T38: keep the received DMA chain (pointer, no copy), walk
it once per frame for the tfrag bucket, and let `present_frame` record the world
pass between its clear and the splash/present tail — behind a compile-time
kill-switch that keeps today's splash-only behavior when OFF.

## Context

- `game/graphics/pipelines/dk_renderer.cpp` — the seam sites:
  - `dk_send_chain` L352-365: currently discards data. Store `{const void* data;
    u32 offset;}` in `DkSyncData` (data is owned by GOAL and valid only until
    the chain is ACKNOWLEDGED — so capture, consume, then ack, in that order).
  - `DkDisplay::render` L174-188: the 40 ms wait + ack block — walk happens
    between wait-success and the `has_data_to_render = false` store.
  - Telemetry block L249-290: T40 extends it, not you.
- `game/graphics/pipelines/dk_device.cpp` — `present_frame` (L560-625): add the
  world hook between the clear and the splash blit; `kSplash3D` L78 is the
  kill-switch precedent → add `constexpr bool kTfrag3Draw = false;` beside it.
- `game/graphics/opengl_renderer/buckets.h` — find the jak1 tfrag bucket id
  (`grep -n -i tfrag buckets.h`); the walk is `DmaFollower` until that bucket
  (see `common/dma/dma_chain_read.h` + how GL dispatch skips to buckets).
- **Stop-point — LevelData access**: T38's `setup_for_level` needs the loader's
  `LevelData`. Find the shared access path:
  `grep -rn 'loader_data' game/graphics/opengl_renderer/OpenGLRenderer.cpp | head`
  and check whether it reaches the loader WITHOUT going through GL renderer
  objects. If it does not, STOP after the sends-chain part and report the
  candidates — the reviewer decides the seam (do not invent a global).

## API cheat-sheet (verified)

- Plain registration API (dk_device.h stays deko3d-free):
  `void set_world_recorder(void (*fn)(void));` — present_frame calls `fn()`
  after the clear when `kTfrag3Draw && fn`; TFragmentDk.cpp's `record_frame`
  entry pulls `cmd_handle()` itself via dk_device_internal.h (T22a pattern).
- `DkDisplay::render` must set the recorder once (first-frame block L150-156 is
  the precedent place): `dkdev::set_world_recorder(dktf::record_frame_entry);`
  — dk_renderer.cpp may include `TFragmentDk.h` (orchestrator layer, plain decl).
- Recording order in present_frame: clear (3D) → `fn()` (3D draws) → barrier →
  blit (2D) — the barrier already exists at L611; world draws land BEFORE it.
- Fence discipline untouched: world recording happens after the slice-fence wait
  at the top of present_frame — safe by construction; do not add waits.

## Steps

1. dk_device.h/.cpp: `set_world_recorder` + call site in present_frame + the
   `kTfrag3Draw = false` constant (comment: flip for T40 gate testing only).
2. dk_renderer.cpp: DkSyncData chain fields; send_chain stores; render():
   on got_chain, if `kTfrag3Draw`-compiled world recorder registered and device
   up, run the bucket walk + `dktf::` entry (guarded try-free: every null → skip
   frame, one `[dk]` line max per failure type per report window — do not spam
   per frame), THEN ack. When the kill-switch is false, behavior must be
   byte-identical to today (splash-only, chain acked unread).
3. Wire `setup_for_level` at the LevelData access point you found (or stop per
   the stop-point above).
4. Report MUST state: new NRO md5, and that with kTfrag3D=false the runtime
   behavior is unchanged (the added call sites are compile-time disabled).

## Rules

- Touch: `dk_device.h/.cpp`, `dk_renderer.cpp`, `TFragmentDk.h/.cpp` (entry fn).
  Nothing else. No GL files.
- The chain pointer is read-only shared state — never free/mutate it.
- If the game boots to splash as today with the switch off, that is SUCCESS for
  this task (the gate flip is T40, on hardware, by the human).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + **new md5 expected** (live call graph changed). Paste the md5.
Reviewer note: with `kTfrag3Draw=false` the recorded work is identical to T24's;
md5 change is from the new (called) wiring code only.

## Doubts

- If walking to the bucket needs more DMA machinery than DmaFollower alone
  provides (tag skips etc.), copy the exact skip loop from GL dispatch — cite
  the lines you copied from.
