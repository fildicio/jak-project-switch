# T91b — Ocean survey 2/3: GL census, read-backs, loader (AI-assisted)

Track D ocean · Depends on: T91a · Report only, NO repo code change · One SHORT session

## Goal

Append sections 3 and 6 to `docs/deko3d/ocean-survey.md` (T91a created it).
ZERO design, ZERO opinions.

## Session rules (hard)

- You may ONLY APPEND to `docs/deko3d/ocean-survey.md` (`cat >> … <<'EOF'`,
  at most ~25 lines per chunk, retry a failed chunk only). No other file.
- Headers first; do NOT open OceanMid_PS2.cpp / OceanNear_PS2.cpp bodies.
- Commands from the repo root, each under 30 s. Report in QWEN_REPORT.md `## T91b`.

## Steps

1. Section `## 3. GL census`, per ocean/*.cpp (counts only):
   ```sh
   for f in game/graphics/opengl_renderer/ocean/*.cpp; do
     echo "== $f"
     grep -c "glDraw" $f; grep -c "glBindTexture\|glTexImage\|glTexSubImage\|glCompressedTexImage" $f
     grep -c "glGet\|glReadPixels\|glMapBuffer" $f; grep -c "glUniform\|glProgramUniform" $f
     grep -c "glBufferSubData\|glBufferData" $f; grep -c "glFramebuffer\|glRenderbuffer" $f
   done
   ```
   One table row per file with the six counts. Then ONE list of every DISTINCT
   read-back call (glGet*/glReadPixels/glMapBuffer) with file:line. If a grep
   exits 1 (no matches) the count is 0 — that is a fact, write it.
2. Section `## 6. Loader`:
   ```sh
   grep -rni "ocean" game/graphics/opengl_renderer/loader --include=*.cpp | head -20
   ```
   plus any pools/lookups the ocean headers reference. Table: data · source ·
   format · approximate size. If the loader has nothing ocean-specific, write
   exactly that sentence — it is a fact, not a failure.

## Done-check

```sh
grep -c "^## " docs/deko3d/ocean-survey.md && git status --short
```

Expected: `5`, and no code files in git status. Set the README T91b row to 🟡.
