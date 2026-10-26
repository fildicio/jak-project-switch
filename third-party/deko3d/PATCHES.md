# deko3d in this repo (AI-assisted, T101)

deko3d **0.5.0**, the same version as the devkita64 image's `libdeko3d.a` (`include/` is
byte-identical to `/opt/devkitpro/libnx/include/deko3d.h`), built from source by
`CMakeLists.txt` as the static library `deko3d_patched`. Only `include/`, `source/`,
`generated/` and `LICENSE` are kept.

- `include/`, `source/`, `LICENSE`: https://github.com/devkitPro/deko3d tag `v0.5.0`.
- `generated/`: what upstream's Makefile writes with devkitPro's dekotools (`dekodef`,
  `dekomme`), which the devkita64 image does not ship. Taken from
  https://github.com/centollOS/SwitchWakerHD `runtime/third_party/deko3d/generated/` at
  `44dc0e2` (built from the same v0.5.0 sources). Regenerate only if a `.def`/`.mme` changes.

Every change to upstream is marked `jak-switch patch N`. Both bugs were found and
documented by SwitchWakerHD (`runtime/third_party/deko3d/PATCHES.md` there) and are still
unfixed upstream (checked 2026-10-24, master `373bfd4`).

1. **Separate alpha destination factor** (`source/maxwell/gpu_3d_state.cpp`,
   `dkCmdBufBindBlendStates`): upstream writes `dstColorBlendFactor` into `FuncAlphaDst`,
   so any blend state whose `dstAlphaBlendFactor` differs from `dstColorBlendFactor`
   wrote the wrong framebuffer alpha. Here that is `dk_state.cpp` StdAlpha, PremulAdd,
   Fix128Add, Fix64Sub and SrcAlphaSub (all `dstA=Zero`; only DstAlphaAdd is unaffected)
   and the ocean flush passes (`dk_ocean_flush.cpp` kBlendStdAlpha/kBlendAlphaOnly),
   whose alpha the envmap pass then reads via `DkBlendFactor_DstAlpha`.
2. **Control memory chunks lost on clear** (`source/dk_cmdbuf.cpp`, `CmdBuf::clear`):
   used control chunks (1 KiB each) are a list, newest first. `clear()` linked only the
   newest to the free list, leaking every older one each time a recording needed more than
   one chunk. `dkCmdBufClear` runs every frame here (`dk_device.cpp`), so the heap shrinks
   over a long session.
