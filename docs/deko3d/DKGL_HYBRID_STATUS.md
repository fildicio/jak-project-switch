# dkgl hybrid — status after T104b–T111 (AI-assisted)

Branch `dkgl-tasks`, on top of `deko3d-migration` cf2280971. Written for the reviewer/owner:
what was done, how it works, what was fixed in the T103–T106 drafts, and what is still open.

## Result on hardware (jak1 PAL, handheld 720p, stock clocks)

- Ocean through dkgl: visible and correct, **60 fps**, `gpu world` 6–11 ms/frame.
- Generic2 (T111) runs through dkgl too (10 buckets), no regressions.
- Splash: stays on screen until the game's first frame (was: ~0.5 s of splash, then ~15 s black).

## How it works (T107 design, deviation from the task text)

The task ran the GL renderers from the world-recorder sites. That cannot work: `dk_renderer.cpp`
acks the DMA chain right after its captures (“after it the EE rewrites that memory”, T60), and the
ocean walks the chain itself (CALL/RET, CNT+NOP, end tags), so a flattened copy is not an option.

So `dkgl_frame.cpp` splits the work:

1. **Capture time** (`dkgl::capture_frame`, called where `dkocean::capture_chain` was, chain still
   valid): the UNCHANGED GL renderers run (`OceanMidAndFar` bucket 4, `OceanNear` 63, one shared
   `Generic2` + 10 `Generic2BucketRenderer`s 11 18 24 30 46 50 53 56 59 62). Their GL calls go
   through the glad overrides into the dkgl shadow state; the Switch emitter records the GPU work
   into **dkgl's own command lists** (own memblock: 3 slots × {cmd memory, vertex ring, UBO ring},
   each slot reused only after its fence; plus a persistent region for init-time buffers).
   A target switch to FBO 0 ends a list segment (the frame views are only known later).
2. **World recorder** (`dkgl::record_bucket(b)`): calls those lists with `dkCmdBufCallList` at the
   bucket's place in the GL order (4 after sky, 11–30 after shrub, 46–62 after merc, 63 before
   sprite), re-binding the frame target before frame segments, then signals the slot fence.

Fallbacks: `gk_dkocean_native.txt` = native dkocean path, `gk_dkgl_nogeneric.txt` = no Generic2,
any init failure logs `[dkgl] ...` and keeps the native ocean.

## Bugs found and fixed in the T103–T106 drafts (each has a host test, 28 pass)

| bug | symptom it would/did cause |
|---|---|
| generator: every uniform in a 16-byte slot without GLSL padding (std140 packs scalars at 4) | all ocean uniforms read at wrong offsets |
| generator: varyings without locations, `ocean_texture` declares them in a different order per stage | crossed varyings (UAM compiles stages alone) |
| `GL_PRIMITIVE_RESTART` was 0x8558 (real: 0x8F9D, glad remaps to 0x8D69 on Switch) | **HW**: strips joined into screen-wide black triangles, 70 ms GPU, 12 fps |
| `kMaxFbos = 4` (ocean needs 11+: one FBO per mip level) | **HW**: mip passes drew into the frame, pixelated square bottom-left |
| `glVertexAttribIPointer(4 x u8)` and stride 0 unsupported | **HW**: every ocean draw skipped (black ocean) |
| frag UBO at `vert.block_size` (not 0x100-aligned) | wrong/unaligned UBO address |
| FBO mip level ignored, `glGetIntegerv` missing (census no-op) | mips drawn into level 0, garbage viewport restore |
| pool texture params (REPEAT, mips) dropped; one VBO per VAO; init-time buffers in the frame ring | clamped water, `OceanTexture_PC` broken, static quads lost after 3 frames |
| depth written with the depth test off; blend enable never set (DkColorState) | GL semantics differences |
| `u32` in `dk_device.h`; sampler `DkMipFilter_Nearest` without mip bit | Switch compile error; LOD-picked mips for GL_LINEAR |
| uniform shadow 256 B, `generic` needs 272 B | memory overwrite (T111) |

Also added: `[dkgl]` stats line every ~10 s (draws, skipped, inert, ring use, overflows) and a
one-time reason for the first skipped draw; PCRTC brightness/contrast pass (`dk_device.cpp
record_pcrtc`, GL `do_pcrtc_effects` parity); Windows host build guard in `LoaderStages.cpp`.

## How to build and test

- Switch: `cmake --build build-switch --target gk_nro` in `devkitpro/devkita64`.
- Host tests (Windows clang preset or build-host): `goalc-test --gtest_filter='Dkgl*'` → 28 PASS.
- `python3 scripts/ai/check-dk.py` → PASS. `scripts/ai/dkgl_gen_shaders.py --check` → PASS.
- SD: the GL shader folder `data/game/graphics/opengl_renderer/shaders/` must be present
  (ShaderLibrary reads it; content is unused, identity = name hint, behaviour = embedded dksh).
- Do not launch with the netloader while the live log is on: the boot freezes (thread `DMP`
  stuck in the run-log fsync while holding the log mutex; only with `gk_log_host.txt` / nxlink host).

## Still open (native path, not dkgl)

- **Distant mountains badly drawn / missing.** A per-bucket census of the GL path shows that
  outside the sage's hut nothing goes through Generic2: the mountains are tfrag (6/13) or tie
  (9/16). They cannot be swapped to the GL renderer like the ocean (needs GL level buffers from
  the GL Loader). Next step: A/B without `gk_dktie.txt` to tell tie from tfrag, then compare that
  native path with GL for far geometry (LOD, fog, depth mapping). `upload_rgba8_mipped` was
  checked and looks correct.
- Level textures pop in ~1 s after a level starts; the intro logo video (Daxter + Naughty Dog)
  shows saturated/jumping textures for ~1 s.
- Colours look duller than the GL path even with the PCRTC pass.
- Not done: T108 (host replay), T109 (jak2/jak3 recheck), T112 DepthCue, T113 Shadow.
