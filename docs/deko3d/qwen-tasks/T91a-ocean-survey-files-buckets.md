# T91a — Ocean survey 1/3: files, buckets, shaders (AI-assisted)

Track D ocean · Depends on: — · Report only, NO repo code change · One SHORT session

## Goal

First third of the ocean inventory (T91b and T91c continue the same doc). You
create `docs/deko3d/ocean-survey.md` with sections 1, 2 and 4. ZERO design,
ZERO opinions. Anything ambiguous → write `UNCLEAR: …` and move on.

## Session rules (hard)

- The ONLY file you may create is `docs/deko3d/ocean-survey.md`.
- WRITE INCREMENTALLY (three earlier sessions died on one big write): first
  `cat > docs/deko3d/ocean-survey.md <<'EOF'` with the title line only, `EOF`.
  Then append each section with `cat >> docs/deko3d/ocean-survey.md <<'EOF'` …
  at most ~25 lines … `EOF`. NEVER one big write. A failed chunk → retry ONLY
  that chunk.
- READ HEADERS FIRST. Do NOT open OceanMid_PS2.cpp / OceanNear_PS2.cpp bodies
  (T91c handles those, signatures only).
- Commands from the repo root, each under 30 s. Report in QWEN_REPORT.md `## T91a`.

## Steps

1. Section `## 1. Files`:
   ```sh
   wc -l game/graphics/opengl_renderer/ocean/*.cpp game/graphics/opengl_renderer/ocean/*.h | sort -n
   ```
   One row per file: file · LOC · main class(es) · one line from the header's
   top comment or class name.
2. Section `## 2. Buckets`: grep where OceanMidAndFar / OceanNear are
   constructed + registered, and their bucket ids per game
   (jak1/jak2/jak3 enums — write the numeric value only if the enum sets one).
   Table: renderer · bucket id per game · draw order.
3. Section `## 4. Shaders`: every ShaderId the ocean files use → shader file
   names; for each shader file `grep -n "uniform" <file>` →
   table: shader · used by · uniforms.

## Done-check

```sh
test -f docs/deko3d/ocean-survey.md && grep -c "^## " docs/deko3d/ocean-survey.md && git status --short
```

Expected: `3`, and git status shows only the new doc (+ report/README edits).
Set the README T91a row to 🟡.
