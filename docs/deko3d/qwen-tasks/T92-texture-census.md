# T92 — Texture-reuse census: how much image memory the dk textures really use (AI-assisted)

Track D texmem · Depends on: — · Report only, NO repo code change · One session (~20 min)

## Goal

Before the reviewer designs any texture dedup or image-memory recycling, get the
numbers. The Switch already logs every upload (`[dk] tex id=… WxH rgba8 N KB`).
You only collect, count and quote. ZERO design, ZERO opinions, ZERO code changes.
If something is ambiguous, write `UNCLEAR: …` and move on.

## Session rules (hard)

- The ONLY new file is `docs/deko3d/texture-reuse-census.md`.
  `git status --short` must show nothing else at the end.
- No builds, no hardware runs. Commands from the repo root, each under 30 s.
- Report goes in QWEN_REPORT.md under `## T92`.

## Steps

1. Per-log census (run on every hardware log we have):
   ```sh
   for f in hw-logs/*/gk_run_log.txt; do
     echo "== $f"
     grep -c "\[dk\] tex id=" "$f"
     grep "\[dk\] tex id=" "$f" | grep -o "[0-9]* KB" | awk '{s+=$1} END {print s " KB total"}'
     grep "\[dk\] tex live=" "$f" | tail -1
   done
   ```
   Table: log · uploads · total MB · `live/high/fail` (newest logs also print
   `freed= mem= holes=` — include the FIRST and LAST `tex live=` line of the
   NEWEST log verbatim so the reviewer sees the before/after of level frees) ·
   the single largest upload line.

2. Size-signature duplicates (FIRST CUT ONLY — same size is NOT same pixels):
   ```sh
   cat hw-logs/*/gk_run_log.txt | grep "\[dk\] tex id=" | awk '{print $(NF-2)}' | sort | uniq -c | sort -rn | head -20
   ```
   Table: WxH · count. Label the section clearly `same-size only, not dedup proof`.

3. How texture ids are keyed today: read `game/graphics/texture/TexturePool.h` and
   `.cpp`. Quote (≤ 10 lines) how a texture record maps to an id, and answer
   YES/NO with file:line: can two ids hold identical pixel data today?
   Also report the pool's upload entry points the dk path calls
   (grep `update_rgba8\|upload_rgba8` in `game/graphics/deko3d_renderer/` and
   `game/graphics/texture/`).

4. dk release path: quote (≤ 10 lines) from
   `game/graphics/deko3d_renderer/dk_texture_gpu.cpp` the `release()` and
   slot-reuse code, then from the newest log report whether ANY release happened
   during that run (compare `live=` vs `high=`).

## Done-check

```sh
test -f docs/deko3d/texture-reuse-census.md && git status --short
```

Expected: the command prints only `?? docs/deko3d/texture-reuse-census.md`, and the
doc has the four sections. Paste the output. Set the README T92 row to 🟡.
