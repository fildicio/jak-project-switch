# T91c — Ocean survey 3/3: VU functions, frame flow, final check (AI-assisted)

Track D ocean · Depends on: T91b · Report only, NO repo code change · One SHORT session

## Goal

Append sections 5 and 7 to `docs/deko3d/ocean-survey.md`, then run the final
whole-survey check. ZERO design, ZERO opinions.

## Session rules (hard)

- You may ONLY APPEND to `docs/deko3d/ocean-survey.md` (`cat >> … <<'EOF'`,
  at most ~25 lines per chunk, retry a failed chunk only). No other file.
- VU files are 4–5k lines: read the HEADERS fully, and per function at most its
  signature plus the first ~10 lines of body. NEVER read a whole _PS2 body.
- Commands from the repo root, each under 30 s. Report in QWEN_REPORT.md `## T91c`.

## Steps

1. Section `## 5. VU1 emulation`: list the functions of OceanMid_PS2 and
   OceanNear_PS2 (headers, or `grep -n "^[A-Za-z].*(" *.h`), then for each:
   name · takes (params / member m_vu_data / accumulators) · produces (vertex
   array / scratch) · one line of PS2 meaning (from comments or the caller).
   Also note where the input data enters at runtime (which render() memcpy's
   into m_vu_data, i.e. DMA/VIF), per the callers in the non-_PS2 files.
2. Section `## 7. Frame flow`: read ONLY the render()/draw() methods of the two
   bucket renderer classes (OceanMidAndFar.cpp, OceanNear.cpp): ordered step
   list (state sets → uploads → draws → restores), noting envmap /
   FramebufferTexturePair use, scissor or blend-mode changes.
3. Final check for the WHOLE survey:
   ```sh
   for f in game/graphics/opengl_renderer/ocean/*.cpp; do grep -q "$(basename $f)" docs/deko3d/ocean-survey.md || echo "MISSING $(basename $f)"; done; git status --short
   ```

## Done-check

The command above prints NO `MISSING` lines, and git status shows no code files.
Set the README T91c row to 🟡. The reviewer then reviews the whole survey.
