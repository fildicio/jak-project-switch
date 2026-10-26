# T101 — link a patched deko3d 0.5.0 (alpha-blend + cmdbuf-leak fixes) (AI-assisted)

Track D lib · Depends on: — · One session (~20 min, mostly the build)
Reviewer trial (2026-10-24): every command below was dry-run on this Mac → checksums
below match; the patch applies to the real repo; the patched library builds with
devkitPro's `Switch.cmake` in `devkitpro/devkita64:latest` → 0 warnings, `-std=gnu++17`,
117 exported `dk*` functions, **identical set** to the image's stock `libdeko3d.a`.
The full game build was NOT trial-run (your Done-check is the first one).

## Session rules (hard)

- Zero decisions: run the commands, apply the patch, build, report. Do NOT edit any
  C++ or CMake by hand. Do NOT touch `game/graphics/**`.
- FORBIDDEN reads: `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`,
  `third-party/deko3d/source/**` (you copy it, you don't read it).
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T101`.

## Why

The devkita64 image ships deko3d 0.5.0 (latest release) with two bugs, found and fixed
by SwitchWakerHD and still unfixed upstream (master `373bfd4`):

1. `dkCmdBufBindBlendStates` writes `dstColorBlendFactor` into the alpha destination
   factor. Five of our blend modes (`dk_state.cpp` StdAlpha, PremulAdd, Fix128Add,
   Fix64Sub, SrcAlphaSub; ocean `kBlendStdAlpha`/`kBlendAlphaOnly`) set `dstA=Zero`,
   so the framebuffer alpha is wrong — and the ocean envmap pass reads it back with
   `DkBlendFactor_DstAlpha`.
2. `dkCmdBufClear` (called every frame in `dk_device.cpp`) recycles only the newest
   1 KiB control chunk and leaks the older ones → heap shrinks over long sessions.

We build deko3d 0.5.0 from source (same version, header byte-identical) with the two
one-spot fixes, and link that instead of the image's `libdeko3d.a`. Full notes:
`third-party/deko3d/PATCHES.md` (created by the patch).

## Steps (exactly these, from the repo root)

1. Vendor upstream deko3d v0.5.0 + the generated headers:
   ```sh
   mkdir -p third-party/deko3d && cd third-party/deko3d
   curl -sfL https://github.com/devkitPro/deko3d/archive/refs/tags/v0.5.0.tar.gz | tar xz --strip-components=1 deko3d-0.5.0/include deko3d-0.5.0/source deko3d-0.5.0/LICENSE
   mkdir generated && for f in engine_2d.h engine_3d.h engine_3d.mme engine_compute.h engine_copy.h engine_gpfifo.h engine_inline.h mme_macros.h; do curl -sfL -o generated/$f "https://raw.githubusercontent.com/centollOS/SwitchWakerHD/44dc0e23e35ed10495cea7c6866e7e5a3e3aba4d/runtime/third_party/deko3d/generated/$f" || echo "FAIL $f"; done
   echo "files: $(find . -type f | wc -l | tr -d ' ')  sum: $(find . -type f | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -c1-16)"
   cd ../..
   ```
   Must print **`files: 76  sum: f77b4b53b5f0a285`** and no `FAIL`.
   Anything else: STOP, paste the output in the report.
2. `git apply docs/deko3d/qwen-tasks/patches/T101-deko3d-patched.patch`
   - If it fails: STOP, paste the error. Do not hand-edit.
3. Verify the final tree:
   ```sh
   (cd third-party/deko3d && echo "files: $(find . -type f | wc -l | tr -d ' ')  sum: $(find . -type f | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -c1-16)")
   git diff --stat
   ```
   Must print **`files: 78  sum: 8d181905931ff396`**, and `git diff --stat` must list
   `game/CMakeLists.txt | 5 ++++-` (plus any files that were already modified before
   you started — list them in the report, don't touch them).
4. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat check-tail until DONE-PASS / DONE-FAIL
python3 scripts/ai/check-dk.py
grep -c "libdeko3d_patched.a" build-switch-jak1/build.ninja
grep -cE -- "-ldeko3d( |$)" build-switch-jak1/build.ninja
```

Expected:
- **DONE-PASS**. The md5 will differ from every earlier build (expected — new library).
- `dk-lint: PASS`.
- First grep prints **≥ 1** (we link the patched library; was 0 before T101).
- Second grep prints **0** (the stock `-ldeko3d` is gone; was 2 before T101).
  (The build uses Ninja: link lines are `LINK_LIBRARIES` in `build.ninja`, no `link.txt`.)
- DONE-FAIL, or a grep result that doesn't match: STOP, paste the last 30 log lines /
  grep output in the report. Do not try to fix CMake yourself.

Report `## T101`: step 1 and step 3 output lines, the `git diff --stat`, the PASS line +
md5, dk-lint line, both grep counts. Flip the README T101 row to 🟡. No commit, no deploy.
