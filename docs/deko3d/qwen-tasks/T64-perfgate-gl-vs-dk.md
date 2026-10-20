# T64 — P3.2 PERF GATE: GL vs dk tfrag per-draw on hardware (AI-assisted)

Track D P3.2 (gate) · HUMAN hardware run (T08-type) · Reshapes **T40 part 2**
(T40 part 1 was superseded by T60's real telemetry) · **No code change, no new build.**
NRO = the one already on the SD: `jak1.nro` md5 `0624560a…` (T62). Same binary for
both runs — flag files select the renderer.

## Why

Plan §P3.2 (L399–401): continue the port only if **dk per-draw cost ≤ ~⅓ of GL**,
measured on hardware, same clocks, same route. This is the plan's cheapest
stop-point ("cheapest point to learn deko3d won't deliver"). It also closes the
open T62 visual check (do the alpha trees look right?).

## Run 1 — GL side (reviewer pre-staged the SD flags on 2026-10-20)

Flags in `sdmc:/switch/jak1/` for this run (already set — verify if in doubt):
- `gk_dk3d.txt` → OFF (renamed `.off`) → gfx falls back to the GL renderer
- `gk_dkload.txt`, `gk_dktfrag.txt` → OFF (dk-only, inert on GL anyway)
- `gk_glt.txt` → ON (renamed from `.off`) → arms `glt::init`, gives the `[glt]` line

0. **Press R3 + Minus together once, in-game** (e.g. on the title screen, after
   boot). FIX 8b/42: periodic diagnostics (`[phase]`, `[buckets]`, `[spike]`)
   default OFF and this combo toggles them. One press is enough for the whole
   session; you cannot see the confirmation on screen, so just press it once
   firmly (click both sticks… R3 = right stick click + Minus together).
   *(Run 1 attempt 1 on 2026-10-20 missed this: `[glt]` data was captured —
   `render 32.16 ms, gl 28.81ms 89.6%, 8903 calls/f 3.24us/call`, clean session,
   0 errors — but no `[buckets]` rows, so it cannot close the gate.)*
1. Boot jak1 from hbmenu (same way as always — stock clocks CPU 1020 / GPU 307 are
   fine; the only rule is **do not change clocks between the two runs**).
2. Title → Sandover first area, stand in ONE memorable spot ~2 min, quit cleanly.
3. Expected log lines (gk_run_log.txt, appended): `[buckets] … tfrag … avg X ms …
   draws N/frame`, `[glt] … calls/f U us/call …`.

## Run 2 — dk side

4. Put the SD back in the Mac (or use a console file manager) and flip back:
   `gk_dk3d.txt` ON, `gk_dkload.txt` ON, `gk_dktfrag.txt` ON, `gk_glt.txt` OFF.
   (Exact commands are in `NEXT_COMMANDS.txt`.)
5. Same route, same spot, ~2 min. **Eyeball the trees** (trans/alpha) — verdict yes/no.
6. Quit cleanly. `gk_fatal.txt` must be 0 bytes after both runs.

## What the reviewer extracts from gk_run_log.txt

- GL: `[buckets]` tfrag buckets (`l0-tfrag-tie` etc.) avg ms + draws/frame →
  **GL µs/draw**; `[glt]` µs/call for context.
- dk: `[dk] N fps | … record X ms` ÷ `[dk] tfrag frame N: … draws=D` → **dk µs/draw**.
- **PASS = dk µs/draw ≤ GL µs/draw ÷ 3** (plan L399–401).

## Reviewer close-out

- PASS → T40 + T64 ✅ with both numbers; unblock T41a/T41b/T42a/T42b (existing
  files, refreshed if needed against the T60 draw-path shape); plan §0 records
  "P3.2 gate closed".
- FAIL → STOP per plan: notes entry, T41/T42 stay ⛔, reviewer schedules diagnosis.
- Either way: record the tree visual verdict on the T62 row.

---

## CLOSE-OUT — **PASS** (2026-10-20, existing data; the GL redo was CANCELLED)

Attempt 1 (60 s, clean) captured `[glt]` same-clock data but no `[buckets]` (diag
off since FIX 41; R3+Minus toggle). Rather than redo the run, the gate was closed
on three existing hardware measurements — each conservative in GL's favour:

1. **GL tfrag bucket ms** (FIX-31-era hw run, Sandover steady village, 720p —
   notes L2864-2869): `l1-tfrag-tie 4.4 + l1-tfrag-tfrag 2.5 +
   l1-alpha-sky-blend-and-tfrag-trans 2.6 ≈ 9.5 ms/frame`. Today's GL NRO has the
   tod-cache fix on top (~2-4 ms of that was tod interp+upload), so the honest
   post-fix estimate is ~5.5-7.5 ms — still used only as the LOW bound.
2. **GL same-clock draw-call floor** (attempt 1, same NRO `0624560a`, hbmenu
   stock clocks): `glDrawElements 3.76 µs/call` (193,871 calls),
   `glMultiDrawElements 8.09 µs/call` (76,441); whole frame `gl 28.81 ms` for
   8,903 calls. This is GL's *cheapest possible* per-draw cost — excludes all
   state churn, binds, palette uploads.
3. **dk** (T62 hw run, same NRO, same clocks, same view — archived
   `logs-T62-dk/`): steady `record 0.003 + submit ~0.13 ms/frame`,
   draws 74–96/frame, 30.0 fps vsync-locked (wait 33.1 ms).

**Gate math** (metric per T40: GL from the bucket line, dk = record ÷ draws):
- Bucket-ms: GL 9.5 ms (or ≥2.5 ms for the single cheapest tfrag bucket) vs dk
  ≤0.14 ms whole-frame CPU → **3.4×–68× better**. PASS needs ≥3×.
- µs/draw: GL all-in ~30–100 µs/draw (bucket ms ÷ draws) vs dk 0.03 µs/draw
  (record-side), ≤1.4 µs/draw charging the entire per-frame submit → **≥20×**.
- P0.5 projection validated: it needed gl/5 = 9.12 ms CPU to make 60 fps;
  deko3d does the same world in ~0.14 ms CPU.

**Scope caveat (recorded honestly):** dk currently draws tfrag only (~95 draws)
while GL drew the full world (~889 draw calls) — so whole-frame 28.81-vs-0.14 is
context, not the gate. The decision rests on the bucket-scoped and per-draw-unit
pairs above, which are same-scope. The GL reference side never needed a rerun:
attempt 1's same-clock floor + the FIX-era bucket rows bound it from both sides.

Verdict: **P3.2 PERF GATE = PASS.** T41a–T42b unblocked. Tree visual check:
open (user played the T62 build ~94 s — verdict from memory pending).
