# AI roles for the deko3d migration (AI-assisted)

Two models work together. Read this file, `AGENTS.md` and §0 of
`DEKO3D_MIGRATION_PLAN.md` at the start of every session.

## Architect / reviewer (cloud model, e.g. GLM 5.3)

You plan, delegate and review. You do not write first-draft production code.

1. Pick the next unfinished item from §0 of `DEKO3D_MIGRATION_PLAN.md`.
2. Split it into tasks of at most ~150 changed lines and 1–2 files each.
3. Every task states:
   - files and functions to touch;
   - the exact deko3d calls to use, verified in `docs/deko3d-ref/include/deko3d.hpp`
     (deko3d 0.5.0, same version as the build image);
   - a done-check (`scripts/ai/check-switch.sh jakN` PASS, expected log line, etc.).
4. Hand the task to the coder model.
5. Review the diff for: API calls that don't exist in the headers, missing fences
   or memblock lifetimes, GL calls under deko3d code, edits inside
   `opengl_renderer/` or the desktop path, new runtime `std::thread`, missing
   `#ifdef __SWITCH__`.
   - Small issues: fix them yourself.
   - Large issues: send the task back with exact feedback.
6. Stop after one deployable change. Update §0 of the plan and the session notes,
   commit with `(AI-assisted)`, then wait for the user's hardware test.

## Coder (local model, e.g. Qwen3.8-27B via Ollama)

- Implement only the task you were given. No extra refactors or renames.
- Use only functions and types that exist in `docs/deko3d-ref/include/`.
  For usage patterns, copy from `docs/deko3d-ref/examples/`.
- Shaders: follow `docs/deko3d-ref/UAM_README.md` and `docs/deko3d/uam-spike/`
  (explicit `layout(binding = N)`, std140 UBOs, no loose uniforms).
- If the task is unclear or an API seems missing, stop and ask. Never guess.
- Before handing back, run `scripts/ai/check-switch.sh jakN` and fix every FAIL.
  Report the PASS line.
- Also run `python3 scripts/ai/check-dk.py` (deko3d name + pitfall lint); it must print
  `dk-lint: PASS`. Recipes and GL→deko3d table: `.clinerules/deko3d-recipes.md`.

## Task-queue workflow (how we actually run it)

Tasks are files in `docs/deko3d/qwen-tasks/` (`TNN-name.md`), one Qwen run each.
Status lives in that folder's `README.md`: ⏳ todo · 🟡 coder done · ✅ reviewed · ⛔ blocked.

**Reviewer writes tasks** (copy the style of T01–T16): goal, why, exact files,
exact API names checked in `deko3d.h`/`deko3d.hpp` and the examples
(`docs/deko3d-ref/examples/deko_examples/source/`), and a Done-check command
with the expected output. Check every path/name/version before handing over
(e.g. the project's CMake minimum is 3.10). Add a row to the README table.

**Coder runs a batch** (user runs `ai-run`, which gives Qwen one task per session;
or pastes a prompt listing task IDs). Per task: do it,
run its Done-check (max 2 fixes), set README row 🟡, append
`## TNN – PASS/FAIL`, files changed, last check lines and doubts to
`docs/deko3d/qwen-tasks/QWEN_REPORT.md`. Stop on a failure that persists, a
missing API, or a task that says to ask the reviewer.

**User says `check report`** → reviewer, for each 🟡 task:
1. Read its section in `QWEN_REPORT.md`, then `git diff` / the new files.
2. Compare with the task file; verify API names against the headers.
3. Re-run the Done-check yourself (don't trust the report).
4. Fix small issues directly; send big ones back as a new task file.
5. Set ✅, update the "Qwen task queue" row in plan §0, ask the user before committing
   (`(AI-assisted)` in the message).

**Saving cloud tokens:** the reviewer reads the report + diff, not whole files;
reviews in batches; never rewrites work that already passes. Questions and quick
fixes go to Qwen first.

**Hard rules:** nothing goes on hardware before Gate P1 passes.
**Human hardware tests** are T08-type tasks; ⛔ tasks wait for a user decision.

**Creating the next tasks:** when fewer than 3 tasks are ⏳ (or the user asks
`plan next`), take the next unfinished item in plan §0, read its section in plan
§4–§5 (architecture, P1/P2/P3 steps and gates) and the code it touches, then write
new task files numbered after the highest existing TNN. Keep the plan's phase
order and gates: don't start a phase whose gate hasn't passed — instead write
code-only tasks that stay off by default (like `kSplash3D=false`) and say so.
Tell the user which tasks you added and why, in a few lines.

**End EVERY reviewer reply with "Your next commands"** — a copy-paste block of
the exact Terminal commands the user should type next, in order, with one short
comment each. The user is learning; never leave them guessing. Pick what fits:
- tasks are ⏳ and ready → start Qwen (it explains, asks "Start?", keeps the Mac awake):
  `cd ~/Documents/giochi/SWITCH/jak-project-switch-original && ai-run`
- hardware test needed → the build/deploy command, what to do on the Switch,
  which log lines to send back, and the `cline glm "..."` prompt to paste them in;
- review done → `cline glm "Read AI_ROLES.md. commit"`;
- queue empty → `cline glm "Read AI_ROLES.md, then plan next."`.
Also write the same commands to `NEXT_COMMANDS.txt` next to AI_ROLES.md (overwrite it): one command per line, explanations on `#` lines above each. The user runs `ai-next` to list them and `ai-next N` to copy one.

**Reviewer NEVER starts Qwen.** Never run `ai-run`, `cline qwen`, or any background/retry
script (`nohup`, `&`, `/tmp/*.sh`, loops) yourself — only the user starts Qwen, in a terminal.
`ai-run` refuses without a terminal anyway. Put the command in "Your next commands" instead.



## Model-fit law (2026-10-19)

Task authoring rule for the coder model (qwen3.8-cline): the reviewer designs
and pastes ALL code verbatim into task files; the coder only types, builds,
A/Bs and reports. See docs/deko3d/qwen-tasks/README.md "MODEL-FIT AUDIT" for
the evidence and the per-task audit.

## Cost law (2026-10-19) — the goal is to SAVE cloud tokens

Qwen is free; cloud tokens cost money. Before writing any task, ask: "is the task file
+ review cheaper than doing it myself?" If not, don't make a task.

- **Give to Qwen (big saving):** long build loops (jak1/2/3 checks, A/B md5), surveys
  and reports (grep/count/collect into a table), hardware-log extraction, applying
  pattern edits repeated many times where the reviewer writes ONE worked example +
  a check script and Qwen repeats it N times.
- **Reviewer does directly (no task file):** code changes under ~40 lines or any change
  that needs design. Writing verbatim code for Qwen costs MORE than committing it
  (lesson: T47/T48 — reviewer wrote + trial-built the patches, Qwen only ran `git apply`).
- **Short task files:** goal, files, example, done-check command, expected output.
  No essays. Reuse `scripts/ai/` checks instead of describing steps.
- **Cheap review:** read report + `git diff --stat` + md5/check output; open full
  files only when a check fails.
- **Batch:** queue several Qwen tasks so one review covers them all.

## Token-saving rules (2026-10-20)

1. **Start every review with `scripts/ai/review-pack.sh`.** It prints the 🟡 rows, the diff
   stat, the lint, the last md5 per game and the replay-test result. Open a file only when
   the pack shows a problem.
2. **Pick the model by job.** Cheap model (`cline glm`): `check report`, `commit`, task
   files for clone/recheck work. Strong model: new renderer design, GPU-half code, hardware
   bugs nobody can explain from the logs.
3. **Host checks before hardware.** CPU-half changes must pass the replay test
   (`docs/deko3d/REPLAY_TEST.md`) before any SD trip. One hardware trip should test several
   changes at once.
4. **Qwen may draft clone tasks.** For work that copies an existing, hardware-proven pattern
   (a jak2/jak3 bucket table, a renderer copied from tfrag, a recheck), the reviewer can ask
   Qwen to write the task file in the T60 style with every API name passing
   `check-dk.py`. The reviewer only approves or corrects it (status ⏳ after approval).
