# T37b — UBO fill functions (GL math port) (P3.2, AI-assisted)

Track D P3.2 · Depends on: T37a ✅ · Inert until T38 · One session (~40 min)

## Goal

Half 2 of T37. PURE fill functions in `dk_tfrag_ubo.h`: port the GL uniform
math so dk and GL produce identical UBO bytes. No GL calls, no deko3d calls.

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at a clean host-check; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T37b`.

## Read (once, exactly these)

1. `grep -n 'first_tfrag_draw_setup' \
   game/graphics/opengl_renderer/background/background_common.cpp` → read THAT
   FUNCTION ONLY (the hvdf/fog math from `GoalBackgroundCameraData`).
2. `game/graphics/opengl_renderer/background/TFragment.cpp` L520-570 — the
   decal uniform set (L525) + the AFAIL alpha_min/max double-draw (L548-551).

## Steps

1. Add to `dk_tfrag_ubo.h` (still header-only, inline functions):
   ```cpp
   void fill_vert(VertParams* out, const GoalBackgroundCameraData& cam,
                  bool decal);
   void fill_frag(FragParams* out, float alpha_min, float alpha_max,
                  const math::Vector4f& fog_color, bool hack_no_tex);
   ```
   (`GoalBackgroundCameraData` comes from background_common.h — pure data
   struct; if including it drags GL, take the needed members as plain floats
   instead and report which way you went.)
2. `fill_vert`: port `first_tfrag_draw_setup` TERM FOR TERM (hvdf_offset,
   cam_trans, pc_camera, camera, fog_constant/min/max). If a term's source
   field is not in `GoalBackgroundCameraData`, STOP and report where GL gets
   it — do not guess.
3. `fill_frag`: plain stores. Default args at the call site (T38):
   alpha_min `-10.f`, alpha_max `-255.f`... NO — read the GL AFAIL lines and
   use EXACTLY what they show; document the values in a comment.
4. No asserts beyond T37a's; functions are pure (no globals touched).

## Done-check (host first, then build)

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_tfrag_ubo.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 `67018cd5a4411b3f3cc77f198773b261` UNCHANGED.

## Doubts

- If a GL term uses a GLSL-only transform you cannot express with the math
   types, quote it and propose the plain-float equivalent — do not approximate
   silently.
