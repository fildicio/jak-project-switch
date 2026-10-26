# T113 — dkgl ShadowRenderer (bucket 47, the character/blob shadows)

**Depends on:** T112 (dkgl layer grown there; brief order Generic2 → DepthCue → Shadow).
**Category:** D dkgl. **Size:** medium (1 session). **Law:** zero design freedom.
ShadowRenderer's GL surface beyond the dkgl set (grep over ShadowRenderer.cpp, 459 lines;
`Shadow_PS2.cpp` is CPU-only): **`glColorMask`** only. Everything else (per-frame
buffer gen/delete, VAO, depth func/mask, blend func/equation/separate, draw_elements)
is already covered. One new glad override, one emitter method. Anything else → STOP.

## Goal

Run the UNCHANGED `ShadowRenderer` under dkgl, bucket SHADOW = 47 — GL order: right
after generic-alpha 46 (T111's hook) and before generic-pris 50. The renderer decodes
VU DMA (`xgkick`, Shadow_PS2.cpp CPU half untouched), then draws shadow quads with the
`ShaderId::SHADOW` program; `color_uniform` is a plain `glUniform4f` (covered).

## Create 1 — dkgl layer growth: color write mask

1. Shadow state: `u32 color_mask` (default 0xF = RGBA), setter from `glColorMask(r,g,b,a)`
   (packed exactly as the GL booleans arrive — 1 bit per channel).
2. **Emitter method** (verbatim):
   ```cpp
   virtual void set_color_mask(u32 rgba_bits) = 0;
   ```
   Switch impl (⧉ deko3d.h:919-932 + the bind call — copy exact spelling):
   ```cpp
   DkColorWriteState cws;
   dkColorWriteStateDefaults(&cws);
   dkColorWriteStateSetMask(&cws, 0, rgba_bits);
   dkCmdBufBindColorWriteState(cb, &cws);
   ```
   Diff it against the shadow state (re-emit only on change, T106 pattern) and restore
   0xF in `begin_bucket()`/`end_bucket()` — native buckets after the shadow bucket must
   not inherit a masked write.
3. check-dk: `DkColorWriteState`, `dkColorWriteStateDefaults`, `dkColorWriteStateSetMask`,
   `dkCmdBufBindColorWriteState` —
   list them in the report (all verified in-tree via the header lines above).

## Edit 2 — `dk_renderer.cpp` hook

In `record_tfrag_and_shrub`, in T111's after-merc block, between the generic-alpha 46
call and the generic-pris 50 call:

```cpp
if (dkgl::active()) {
  dkgl::run_bucket(data, BucketId::SHADOW);  // T113: GL order — shadow 47 between generic-alpha 46 and generic-pris 50
}
```

## Create 3 — host test additions (T108 fixture)

New TEST_F "Shadow": program hint "shadow", one VBO+VAO, `glColorMask(GL_FALSE,GL_FALSE,
GL_FALSE,GL_TRUE)` (the alpha-only pattern the renderer uses), uniform4f color, one
`draw_elements`, then `glColorMask(GL_TRUE…)` and a second draw. Assert: two
`set_color_mask` emits (0x8 then 0xF), the second draw's snapshot did NOT re-emit it
unnecessarily, and `end_bucket` restored 0xF; the blend factors recorded are the
renderer's `glBlendFuncSeparate` args mapped by the T106 table.

## Checks

- check-dk PASS; check-switch jak1 PASS (dksh count +1: SHADOW program).
- Host suite PASS; census audit: no `ShadowRenderer` first-use lines.
- No `opengl_renderer/` edits.

## Report

Report: where in ShadowRenderer.cpp glColorMask is called (file:line list — it drives
the mask-restore correctness), checks + md5, STOP conditions.

## HW note

Jak's blob shadow + eco effect shadows on the beach — observe on the next SD trip after
this lands. If shadows are invisible-but-present (check the frame shots from FIX 115 if
still enabled), suspect the mask-restore path first.
