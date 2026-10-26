# T112 — dkgl DepthCue (bucket 64, the distance-fog overlay)

**Depends on:** T111 (dkgl layer grown there). **Category:** D dkgl. **Size:** medium
(1 session). **Law:** zero design freedom. DepthCue's GL surface beyond the dkgl set
(measured by grep over DepthCue.cpp, 699 lines): `glBlitFramebuffer`,
`glCheckFramebufferStatus`, `glTexImage2D` (own pages), `glUniform4fv`, plus the already
covered FBO/blend/depth/viewport/buffer/VAO calls. Anything else → STOP + census line.

## Goal

Run the UNCHANGED `DepthCue` GL renderer under dkgl, bucket DEPTH_CUE = 64 — in GL
order that is AFTER ocean-near 63 and BEFORE sprite 66.

What it does (for orientation only — code is unchanged): reads fog setup from DMA,
builds a gradient sprite over its own "depth-cue-base-page" framebuffer, blits/composites
it onto the frame.

## Create 1 — dkgl layer growth

1. `glUniform4fv(loc, count, v)` — vec4 ARRAY (count > 1 legal here): shadow entry
   sized `16 * count` bytes at the table offset; per-draw snapshot copies all of it.
   Host test: two vec4s land in the right slots.
2. `glTexImage2D` on a dkgl texture id (DepthCue's own pages, no pool involvement):
   full-replacement semantics → free the old dktexgpu id (respect P8: recycle ids —
   reuse the T105 release/free-list path) + `dktexgpu::upload_*` with the matching
   internal format (RGBA8 is the only one DepthCue uses — verify by grep; another
   format → STOP). Contents are CPU bytes → same upload path as T105.
3. `glCheckFramebufferStatus` → override returns `GL_FRAMEBUFFER_COMPLETE` always
   (dkgl FBOs are dktexgpu RTs; there is nothing to check).
4. `glBlitFramebuffer(src…, dst…, mask, filter)` → **Emitter method** (verbatim):
   ```cpp
   virtual void blit_texture(u32 src_tex, int sx, int sy, int sw, int sh,
                             u32 dst_kind, int dx, int dy, int dw, int dh,
                             bool nearest) = 0;
   ```
   `dst_kind`: 0 = frame target, 1 = currently-bound dkgl FBO. Switch impl:
   `dkCmdBufBlitImage` with `DkBlitFlag_FilterLinear/Nearest` — copy the call shape
   from `dk_device.cpp` `present_frame`'s blit (recipe #4). **Pitfalls P3+P4:** blit
   runs on the 2D engine → needs `dkCmdBufBarrier(cb, DkBarrier_Full, 0)` before it if
   a 3D draw preceded in the same bucket, and our device is OriginLowerLeft → the
   frame-target blit uses the SAME FlipY convention `present_frame` uses; a
   FBO→FBO/RT blit does not. Copy both variants' flags from `present_frame` /
   `dk_ocean_flush.cpp` (grep `BlitImage`) — do not decide by eye.
   Host mock: record string with all args.
5. Shadow-state + snapshot plumbing for the above per T104a/T106 patterns; no new
   glad override style (function-pointer reassignment like T104b).

## Edit 2 — `dk_renderer.cpp` hook

In `record_tfrag_and_shrub`, between `dkocean::record_near` (63) and `dksprite::record_world`
(66):

```cpp
if (dkgl::active()) {
  dkgl::run_bucket(data, BucketId::DEPTH_CUE);  // T112: GL order — depth-cue 64 after ocean-near 63, before sprite 66
}
```

(No native else-branch — there is no dk-native depth cue; when dkgl is
inactive the bucket stays unrendered exactly as today.)

## Create 3 — host test additions (T108 fixture)

New TEST_F "DepthCue": bind its FBO, `glTexImage2D` a 512x256 page, clear, one gradient
`draw_arrays`, `glBlitFramebuffer` FBO→frame, re-bind frame target. Assert: one
`blit_texture` with dst_kind=0 and nearest=false (match the recorded filter arg), the
clear NOT lost (lost-clear counter 0), a barrier emitted before the blit in the mock
record, exactly one dktexgpu upload per `glTexImage2D`.

## Checks

- check-dk PASS (new identifiers: only `dkCmdBufBlitImage` + flags already in-tree —
  list them); check-switch jak1 PASS (dksh count +1: DEPTH_CUE program).
- Host suite PASS. Census audit like T111 (no `DepthCue` first-use lines).
- No `opengl_renderer/` edits (program identity via the existing Shader hint).

## Report

Report: which formats glTexImage2D actually used (grep list), the FlipY decision per
blit variant with the file/line copied from, checks + md5, STOP conditions.

## HW note

Fog gradient at distance (Sentinel Beach morning fog, jungle) — observe on the next SD
trip after this lands.
