# T109 — jak2/jak3 recheck: dkgl compiles everywhere, stays inactive (report only)

**Depends on:** T107. **Category:** D dkgl (repo hygiene; brief scope is jak1).
**Size:** small (half session). **Law:** zero design freedom; build-batch pattern of
T76–T79. Any build error → fix ONLY if it is a missing `__SWITCH__`/include guard in the
dkgl files themselves; anything else → STOP, paste the error.

## Goal

The dkgl files are compiled into every game's NRO. For jak2/jak3 nothing must change at
runtime: `dkgl::init` gates on `gGameVersion == GameVersion::Jak1` (T107 B1) and stays
inactive — the native `dkocean` path keeps serving those games exactly as before.

## Steps

1. `task set-game-jak2` → `scripts/ai/check-switch.sh jak2` → PASS required.
2. `task set-game-jak3` → `scripts/ai/check-switch.sh jak3` → PASS required.
3. Confirm the gating is compile-visible in the report: quote the `dkgl::init` gate line
   and the `dkgl::active()` call sites in `dk_renderer.cpp` (they must be no-ops when
   inactive — `if (dkgl::active())` false → `dkocean::record_*` runs, unchanged).
4. Confirm no `opengl_renderer/` behavioral leak: `git diff --stat` for the whole dkgl
   series must show `Shader.cpp` as the ONLY opengl_renderer file (the hint), plus the
   allowed TexturePool.cpp seam (that file is `game/graphics/texture/`, not
   `opengl_renderer/`).
5. Jak1 regression: `scripts/ai/check-switch.sh jak1` PASS again after any fix.

## Checks

- 3× check-switch PASS (jak1/jak2/jak3), md5s recorded.
- `python3 scripts/ai/check-dk.py` PASS.

## Report

Report: the three build results + md5s, the gate quote, the `git diff --stat` of the
dkgl series, STOP conditions.
