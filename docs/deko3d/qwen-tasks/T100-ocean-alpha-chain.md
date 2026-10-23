# T100 — Ocean alpha chain: find where the texgen alpha is lost (AI-assisted)

Status: ✅ closed by reviewer 2026-10-23 (no coder run — premise disproven by
code reading, see Verdict). Superseded by FIX 113, built + gated the same day.

## Verdict (2026-10-23 full re-analysis)

The task's premise ("RT alpha ~3–14× too low vs the walk verts' 128") was
**wrong**: GL's texgen never uses the vertex alpha either.
`opengl_renderer/shaders/ocean_texture.vert:13` (and the verbatim dk port)
hardcodes `fragment_color = vec4(rgba_in.rgb * 2., 1.)` — alpha is 1.0, and
the texgen flush runs with blend off (`glDisable(GL_BLEND)`), so
**RT alpha = 1 × envmap.a per texel**. The probed avg 9–48/255 *is* the
envmap alpha, and GL's ocean-texture alpha is the same bytes (decode shared):
there is nothing to fix on the alpha chain. The body passes then use it
exactly like GL: near b1 stamps dst.a = vert.a×2 (~1.0) before the envmap
pass, mid b3 REPLACE writes a = (vert.a×2) × tex.a, and the mip chain fades
alpha by max(0, 1−0.51·i) (0 from level 2 on) — the reflection distance fade.

## What the 2026-10-23 HW run actually ran

NRO `81ee5bdd…` (22 Oct 15:40 build) **predates FIX 112**: its log line
`ocean tex target at tbp 8160: id 351` is the old format — the current code
prints `id N (mips 8, texgen temp N)`. The "ocean disaster" verdict describes
the T95 single-mip ocean (blue horizon line + black minification spots);
**no hardware run has exercised the mip chain yet**.

## FIX 113 (built; gate PASS; awaiting SD deploy + one HW run)

- texgen envmap sampler: LINEAR → LINEAR_MIPMAP_LINEAR (GL flush parity;
  a single-mip envmap image clamps to L0, so it is a no-op there);
- T99 probe extended: also copies the RESULT texture L0 (128×128) + L3
  (16×16) and logs `[dk] ocean probe #N MIP(result) @fF: L0 avg … | L3 avg …`
  — the body draws sample the result, so this verifies the mip chain
  end-to-end. Healthy = L0 ≈ the temp numbers and L3 alpha ≈ 0; black L0 with
  a healthy temp = record_mip_chain failed silently (the real flat-water
  failure mode).

## Next HW run checklist

1. eye: water around Sandover — waves, horizon fade, no flat plane;
2. `grep "ocean probe"` — MIP(result) L0 ≈ temp avg, L3 alpha ≈ 0;
3. `[dk] ocean mip chain running (8 levels)` appears once;
4. `[dk] ocean |` line still `texgen 1.0 … no_tex 0 drop 0 miss 0 ubo 0`.

## 2026-10-23/24 HW result + FIX 114 (actual root cause found; T100 addendum)

The FIX 112+113 NRO ran on HW (`hw-logs/20261023-1708`): items 2–4 above all
PASS — the texture chain (texgen → mip chain → result) is verified healthy on
hardware. User verdict "0 difference", symptom set: black block holes in the
water, light-blue wash on the beach, black horizon band / background mountains
at Sentinel Beach, wrong title colors. Root cause was **not** in the texture at
all: `record_body_flush`'s mid branch never passed `depth_always`, so the mid
plane ran `DkCompareOp_Gequal` (GL `flush_mid` = `ALWAYS` + depth write) and was
depth-culled by the far band recorded just before it — the exact failure the
T95 comment in `dk_ocean_flush.cpp` describes. FIX 114 wires the flag
(`DkCompareOp_Always`), gate PASS `d9792af7efc853c57b6cc4b2739448af`.
Post-114 checklist: Sentinel Beach — holes gone, no light-blue wash on the
beach, horizon band continuous; title-screen colors still expected wrong
(separate path, new investigation).

