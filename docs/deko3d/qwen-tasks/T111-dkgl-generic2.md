# T111 — dkgl Generic2 (the "distant mountains" buckets) — brief's designated next target

**Depends on:** T107 (dkgl layer + frame hook live). **Category:** D dkgl.
**Size:** medium (1 session). **Law:** zero design freedom. The brief pre-measured this
renderer's GL surface: *"Generic2 adds only: glUniform1ui, glUniform3f,
glUniformMatrix4fv, glBlendColor"* — nothing else may be needed. Anything beyond that
list appearing in the census → STOP, paste the census line.

## Goal

Run the UNCHANGED `Generic2` GL renderer (`opengl_renderer/foreground/Generic2.*`, 322
lines, all GL via the existing helpers + `ShaderLibrary`) under dkgl. Its jak1
registrations (transcribed from `init_bucket_renderers_jak1`, OpenGLRenderer.cpp:996-1390
— there are ELEVEN, the brief said ten; the source is the truth):

| bucket id (buckets.h) | name | mode |
|---|---|---|
| GENERIC_TFRAG_TEX_LEVEL0 (=11) / …LEVEL1 (=18) | "l0/l1-tfrag-generic" | NORMAL |
| SHRUB_GENERIC_LEVEL0 (=24) / …LEVEL1 (=30) | "l0/l1-shrub-generic" | NORMAL |
| GENERIC_ALPHA (=46) | "common-alpha-generic" | NORMAL |
| GENERIC_PRIS_LEVEL0 (=50) / LEVEL1 (=53) / GENERIC_PRIS (=56) | "l0/l1/common-pris-generic" | NORMAL |
| GENERIC_WATER_LEVEL0 (=59) / LEVEL1 (=62) | "l0/l1-water-generic" | NORMAL |

## Create 1 — dkgl layer growth (4 glad overrides + shadow state + emitter)

Exactly these additions, nothing more:

1. **Uniforms** (shadow-state entries + snapshot copy in the T106 per-draw UBO path,
   std140 rules already fixed by T105 — 16 B/slot, mat4 64 B):
   - `uniform1ui(loc, v)` → `u32` slot at the offset the T103 table gives
   - `uniform3f` → vec3 padded to 16 B
   - `uniformMatrix4fv(loc, 1, GL_FALSE, m)` → mat4 64 B block (transpose false only;
     true → STOP, GL never sends true here)
   - `glBlendColor(r,g,b,a)` → shadow + `Emitter::set_blend_color(r,g,b,a)`
2. **Emitter method** (add to T104a's interface verbatim):
   ```cpp
   virtual void set_blend_color(float r, float g, float b, float a) = 0;
   ```
   Switch impl: `dkCmdBufSetBlendConst(dkdev::cmd_handle(), r, g, b, a);`
   (⧉ deko3d.h:1293). Host mock: record string.
3. Extend T103's `dkgl_gen_shaders.py` program table with the Generic2 program(s):
   the ShaderId(s) `Generic2` uses (grep its ctor/shader use; if it activates more than
   one program, add each). Same ES310 + std140 treatment, same uam rule, embedded dksh
   via the existing rule. Update the golden host test for the new uniform table entries.
4. check-dk allowance already covers dkgl files (T104b).

## Edit 2 — `dk_renderer.cpp` hooks (inside `record_tfrag_and_shrub`, line 275)

Insert `dkgl::run_bucket(data, id)` calls at the numeric bucket positions, mirroring the
existing neighbor comments (sky 3 / ocean 4 / tfrag 6-7 / hfrag 8 / tie 9 / shrub 20 /
merc 10+ / ocean-near 63 / sprite 66):

- after `dktie::record_world` (tie 9): buckets 11, 18
- after `dkshrub::record_world` (shrub 19-30): buckets 24, 30
- after `dkmerc::record_world`: buckets 46, 50, 53, 56, 59, 62 (ascending, one comment:
  `// T111: GL order — generic-alpha 46 … generic-water 62, before ocean-near 63`)

**Ordering hazard check (do this first):** each native `dk*::record_*` draws whole
categories in one call. Verify with a grep of each native's bucket list that no native
call draws a bucket BOTH below 11 and above 18 (etc.) around your insert point — if a
native call spans across one of the new hook positions (i.e. the true GL order is
interleaved inside a native category call), STOP and report the pair; do not reorder
natives.

`dkgl::run_bucket` already constructs the DmaFollower per bucket (T107 D); `data` comes
from the world-recorder argument the same way T107's sites get it.

## Create 3 — host test additions (extend the T108 fixture)

One new TEST_F: link the generic program via hint, `glBlendColor(0.25,0.5,0.75,1)`,
`uniformMatrix4fv` of a known matrix, `uniform1ui`, one `draw_elements`. Assert: the
blend-const string recorded; the UBO slice bytes match the matrix (column-major,
64 B at the table offset) + the uint at its offset; the mock emits exactly one
`set_blend_color`.

## Checks

- `python3 scripts/ai/check-dk.py` PASS; `scripts/ai/check-switch.sh jak1` PASS (the new
  dksh count in the build log = T103's 10 + the new generic programs).
- Host dkgl suite + new test PASS.
- Census audit: run jak1 boot with the census enabled (T49's `gen-gl-census` output) and
  confirm NO first-use log line names `Generic2*`/`generic2` files. If one appears →
  a call slipped past dkgl → STOP with the line.
- `git status`: dkgl files + `dk_renderer.cpp` only (no `opengl_renderer/` edits —
  Generic2 needs NO hint change; its Shader activates go through the same `Shader::activate`
  override the ocean already covers).

## Report

Report: the exact list of ShaderIds added to the generator, the ordering-hazard verdict
per insert point, census grep output, checks, NRO md5, STOP conditions.

## HW note

The visual payoff — distant mountains at Sentinel Beach / Sandover — is observed on the
first SD trip after this lands (can share T110's trip if the timing works). FIX 114's
"black horizon band" symptom may already have masked how much was ocean vs missing
generic draws; compare before/after.
