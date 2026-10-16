# Qwen task queue — deko3d migration (AI-assisted)

One file = one task for the local coder model. The reviewer (GLM / Copilot) checks
each result before the next task starts. Written 2026-10-16, updated 2026-10-17 (Gate P1 hardware PASS — see plan §0).

## How to run a task

From the repo root, in the Qwen terminal:

```sh
cline qwen "Do the task in docs/deko3d/qwen-tasks/T01-splash-shaders.md. Follow it exactly."
```

When Qwen finishes, tell the reviewer: `check T01`. The reviewer reads the diff, runs the
done-check, fixes small issues, and marks the row below ✅. Nothing is committed or
deployed by Qwen.

## Rules for every task (Qwen: read this once)

- Only change the files the task lists. No renames, no reformatting, no extra refactors.
- deko3d API: only names that exist in `docs/deko3d-ref/include/deko3d.h`.
  Patterns: `docs/deko3d-ref/examples/`. libnx: only names the task gives you.
- Never edit `game/graphics/opengl_renderer/` or desktop-only code.
- New C++ files: whole body inside `#if defined(__SWITCH__)` … `#endif` unless the task
  says the file is host-testable. Files that include `<switch.h>` must not include
  `common/common_types.h` (u128 clash) — declare `switch_run_logf` by hand like
  `game/graphics/pipelines/dk_device.cpp` does.
- No `std::thread`, no exceptions, no new dependencies.
- **Shell commands are killed after 30 s** (Cline `run_commands` hard cap, not
  configurable). Never `sleep` more than ~20 s; never run a Docker build or
  `check-switch.sh` in the foreground. For anything long:
  `nohup scripts/ai/check-switch.sh jak1 > /tmp/ai-check.log 2>&1 &` (returns
  instantly), then poll with short commands like `tail -5 /tmp/ai-check.log`
  until the PASS/FAIL line appears. Treat "Command timed out after 30000ms"
  as "use the background pattern", not as a build failure.
- Run the task's **Done-check** and paste its last lines in your final message.
- If something in the task looks wrong or an API is missing: stop and say so. Don't guess.

## Queue

Status: ⏳ todo · 🟡 coder done, needs review · ✅ reviewed · ⛔ blocked · ⏸ deferred

| ID | Track | Task | Depends on | Status |
|---|---|---|---|---|
| T01 | A shaders | UAM-ready splash shaders | — | ✅ |
| T02 | A shaders | `cmake/EmbedBinary.cmake` (binary → C header) | — | ✅ |
| T03 | A shaders | `cmake/Deko3dShaders.cmake` (uam rule) | T02 | ✅ |
| T04 | A shaders | Wire shader build into `game/CMakeLists.txt` | T01 T03 | ✅ |
| T05 | B 3D splash | Code memblock + shader loading in `dk_device.cpp` | T04 | ✅ |
| T06 | B 3D splash | Descriptor sets + UBO area in the data pool | T05 | ✅ |
| T07 | B 3D splash | 3D splash draw behind `kSplash3D` (off) | T06 | ✅ |
| T08 | human | HW test of T07 (`kSplash3D=true`) + HOME/sleep/resume + dock/undock | T07 + Gate P1 ✅ | ⏳ NRO ready: md5 `ad789291…`, `~/Desktop/jak bakcups/gk.T08-splash3d.ad7892914d7d.nro` |
| T09 | C applet | Log docked/handheld changes | — | ✅ |
| T10 | C applet | Stop presenting while out of focus | T09 | ✅ |
| T11 | C applet | Swapchain resize on dock/undock | T10 + reviewer decision | ⏸ deferred 2026-10-17 (decision in task file) |
| T12 | D P2 prep | `dk_ring.h` per-frame ring allocator (host-testable) | — | ✅ |
| T13 | D P2 prep | gtest for `dk_ring.h` | T12 | ✅ |
| T14 | D P2 prep | `PipelineTable.h` plain structs | — | ✅ |
| T15 | D P2 prep | `scripts/gen-dk-pipeline-table.py` → `.inc` | T14 | ✅ |
| T16 | D P2 prep | `dk_state.cpp`: plain entry → deko3d state structs | T15 | ✅ |

Tracks A/C/D are independent: any ready task can go next. **Next batch: T14 → T15 → T16**
(in order — T16 depends on both). T08 is the human hardware test (NRO built, deploy when
the user is ready; the SD is currently on the GL path — `gk_dk3d.txt` removed). T11 deferred.
