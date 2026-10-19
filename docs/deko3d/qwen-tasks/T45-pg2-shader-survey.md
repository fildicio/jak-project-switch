# T45 — PG.2 survey: which GL shaders convert to deko3d as-is (AI-assisted)

Track D PG.2 · Depends on: — · Report only, NO repo code change · One session (~30 min)

## Goal

Measure the rest of PG.2: run the T44 converter over ALL GL shaders (into /tmp,
never into the repo), compile every clean one with UAM, and write one doc table.
ZERO decisions: copy command output into the doc.

## Session rules (hard)

- Do NOT edit any code or shader. The ONLY new file is
  `docs/deko3d/pg2-shader-survey.md`.
- Run commands from the repo root, exactly as written. If one fails: STOP, paste it.
- Report goes in QWEN_REPORT.md under `## T45`.

## Steps

1. Survey (reviewer-tested: 94 lines, 54 OK / 38 NEEDS-BLOCK / 2 ODD-VERSION):
   ```sh
   G=game/graphics/opengl_renderer/shaders
   python3 scripts/ai/pg2-convert-shader.py --survey $G/*.vert $G/*.frag > /tmp/t45-survey.txt
   wc -l < /tmp/t45-survey.txt; awk '{print $1}' /tmp/t45-survey.txt | sort | uniq -c
   ```

2. Convert every OK shader into /tmp and compile it with UAM (~5 min):
   ```sh
   rm -rf /tmp/t45 && mkdir -p /tmp/t45
   G=game/graphics/opengl_renderer/shaders
   for f in $(awk '$1=="OK"{print $2}' /tmp/t45-survey.txt); do s=${f##*.}; python3 scripts/ai/pg2-convert-shader.py $G/$f $s /tmp/t45/$f.glsl || echo "CONVERT-FAIL $f"; done
   docker run --rm -v "$PWD:/work" -v /tmp/t45:/t45 -w /work devkitpro/devkita64:latest bash -c 'for f in /t45/*.glsl; do b=$(basename $f .glsl); python3 scripts/gen-dksh.py --input $f --stage ${b##*.} --output /t45/$b.dksh >/dev/null 2>/t45/$b.err && echo "UAM-OK $b" || echo "UAM-FAIL $b: $(head -c 200 /t45/$b.err | tr "\n" " ")"; done' > /tmp/t45-uam.txt
   grep -c UAM-OK /tmp/t45-uam.txt; grep UAM-FAIL /tmp/t45-uam.txt
   ```

3. Create `docs/deko3d/pg2-shader-survey.md` with exactly these sections:
   - `# PG.2 shader survey (T45, AI-assisted)` + date + the step-1 counts.
   - `## Survey` — the full content of /tmp/t45-survey.txt inside a ``` block.
   - `## UAM` — the full content of /tmp/t45-uam.txt inside a ``` block.
   - `## Missing uniforms` — every distinct name from the `missing=` column,
     one per line, with how many shaders need it:
     ```sh
     awk -F'missing=' '{print $2}' /tmp/t45-survey.txt | tr ' ' '\n' | grep -v '^-$' | grep -v has-uniform-block | grep -v '^$' | sort | uniq -c | sort -rn
     ```
   No opinions, no plans — the reviewer designs the new blocks from this.

4. Report: the counts from steps 1–2 and any CONVERT-FAIL / UAM-FAIL lines.
   Set the README T45 row to 🟡. `rm -rf /tmp/t45`.
