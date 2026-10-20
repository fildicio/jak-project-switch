# deko3d rules for this repo (AI-assisted)

Also read `AI_ROLES.md` (who plans, who codes) and §0 of `DEKO3D_MIGRATION_PLAN.md`.

## Recipes + lint (read first)
- Copy-paste patterns, the GL→deko3d table and the pitfall list: `.clinerules/deko3d-recipes.md`.
- After every edit run `python3 scripts/ai/check-dk.py` → must print `dk-lint: PASS`
  (fails on invented dk*/Dk*/DK_* names and GL calls in deko3d code).

## Source of truth: never guess an API
- Headers (deko3d 0.5.0, same as the build image): `docs/deko3d-ref/include/deko3d.h`
  (C API) and `deko3d.hpp` (C++ wrappers). Grep them for any function, struct, enum or
  flag before you use it. If it isn't there, it doesn't exist.
- Working patterns: `docs/deko3d-ref/examples/` (`deko_basic/` = smallest C example;
  `deko_examples/source/Example0*.cpp` = triangle, cube, textures, MSAA, compute;
  `SampleFramework/CMemPool`, `CShader`, `CDescriptorSet` = helper patterns).
- This repo's style: `game/graphics/pipelines/dk_device.cpp` uses the **C API**
  (`dkXxxMakerDefaults` then `dkXxxCreate`). Follow it in new files.

## Core model (how deko3d differs from OpenGL)
- Nothing is implicit. You allocate GPU memory (`DkMemBlock`), place objects in it,
  record commands into a `DkCmdBuf`, submit the `DkCmdList` to a `DkQueue`, and
  synchronise with `DkFence`. No driver validates or tracks state for you.
- Memblock sizes are multiples of `DK_MEMBLOCK_ALIGNMENT` (0x1000). Flags:
  - images/render targets: `GpuCached | Image`
  - CPU-written data (vertices, UBOs, command memory, staging): `CpuUncached | GpuCached`
  - shader code: `CpuUncached | GpuCached | Code`
- Other alignments: UBO `DK_UNIFORM_BUF_ALIGNMENT` (0x100, max 64 KB), shader code
  `DK_SHADER_CODE_ALIGNMENT` (0x100), descriptors 0x20. Use the constants, not numbers.
- Images: fill a `DkImageLayoutMaker`, `dkImageLayoutInitialize`, allocate
  `dkImageLayoutGetSize` bytes at `dkImageLayoutGetAlignment`, then `dkImageInitialize`.
  BC1/BC3/BC7 are native formats: upload the compressed bytes directly.
- Uploads: write to a CPU-visible staging block, then
  `dkCmdBufCopyBufferToImage` on the queue.
- Command memory reuse: never `dkCmdBufClear` + reuse memory that the GPU may still be
  reading. Wait on that slice's fence first (see `present_frame` in `dk_device.cpp`).
- Textures in shaders: descriptor sets + `dkMakeTextureHandle(imageId, samplerId)`.
- Mixing 3D-engine work (clear/draw) with 2D-engine blits needs a `dkCmdBufBarrier`.

## Shaders (UAM, GLSL → .dksh at build time)
See `docs/deko3d/uam-spike/README.md` and `docs/deko3d-ref/UAM_README.md`.
- `--stage vert|frag` (not `vertex`/`fragment`).
- Every sampler and UBO needs `layout(binding = N)`, including vertex-shader samplers.
- **No loose uniforms.** Move them into `layout(std140, binding = N) uniform Name { ... };`
  and match the C++ struct to std140 layout (vec3 pads to 16 bytes, mat4 = 64 bytes).
- Apply the same token substitutions as `Shader.cpp` (HEIGHT_SCALE etc., see spike README).

## Project rules (break these and the build or the console breaks)
- deko3d code only under `#if defined(__SWITCH__)`. Only `dk_*.cpp` files include
  `<deko3d.h>` / `<switch.h>`; headers stay plain C++ (see `dk_device.h` comment).
- Never edit `game/graphics/opengl_renderer/` or any desktop path. Copy and adapt into
  new files instead.
- Never create a runtime `std::thread` (heap is ~4 MB free).
- Check every `dk*Create` result. On failure, log with `switch_run_logf("[dk] ...")`
  and fall back. Never crash.
- Log format: `[dk]` prefix, one line, numbers with units.
- Build: `scripts/build-switch.sh` in Docker, one game at a time, `JOBS=2`.
- Commits: one deployable change, message ends with `(AI-assisted)`.

## Repo knowledge (read on demand, not every time)
Before touching OpenGL renderers, GOAL↔C++ handoff, textures/loader, build, or deploy:
open `docs/deko3d/AGENT_KNOWLEDGE_MAP.md`, read only the section you need
(it has file paths + line numbers), then read those files. Never read
`SWITCH_PORT_SESSION_NOTES.md` whole — grep it for a FIX number.

## Task queue
Small coding tasks live in `docs/deko3d/qwen-tasks/` (status in its README:
⏳ todo, 🟡 coder done / needs review, ✅ reviewed). The coder writes results to
`QWEN_REPORT.md`; only the reviewer sets ✅ and updates plan §0.

## Self-check before saying "done"
Run `scripts/ai/check-switch.sh jak1` (docker build + EL0 sysreg audit, ~short PASS/FAIL).
Fix every FAIL yourself first; paste the final PASS line in your summary. It never deploys.
