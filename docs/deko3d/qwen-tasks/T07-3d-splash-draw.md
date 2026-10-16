# T07 — Draw the splash with the 3D pipeline (behind a switch, off by default)

**Goal:** plan item P1.4b. A second way to draw the splash: a real 3D draw with the
T05 shaders and T06 descriptors, instead of the 2D-engine blit. Off by default, so the
P1 behavior doesn't change until the hardware test (T08).

**Edit (1 file):** `game/graphics/pipelines/dk_device.cpp`.

**1.** Near the constants: `constexpr bool kSplash3D = false;  // P1.4b: flip after T08 hardware test`

**2.** New helper in the anonymous namespace, `void record_splash_3d(uint32_t fb_w, uint32_t fb_h)`,
called from `present_frame()` **instead of the blit block** when
`kSplash3D && s_have_shaders && s_have_desc` (keep the blit as the `else`). Steps, in order:
```cpp
const DkGpuAddr data = dkMemBlockGetGpuAddr(s_dataMem);
DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
dkCmdBufSetViewports(s_cmd, 0, &vp, 1);
DkShader const* sh[] = {&s_splashVsh, &s_splashFsh};
dkCmdBufBindShaders(s_cmd, DkStageFlag_GraphicsMask, sh, 2);
DkRasterizerState rs; dkRasterizerStateDefaults(&rs); rs.cullMode = DkFace_None;
DkColorState cs; dkColorStateDefaults(&cs);
DkColorWriteState cw; dkColorWriteStateDefaults(&cw);
dkCmdBufBindRasterizerState(s_cmd, &rs);
dkCmdBufBindColorState(s_cmd, &cs);
dkCmdBufBindColorWriteState(s_cmd, &cw);
dkCmdBufBindVtxAttribState(s_cmd, nullptr, 0);   // no vertex buffers: gl_VertexID only
dkCmdBufBindVtxBufferState(s_cmd, nullptr, 0);
dkCmdBufBindImageDescriptorSet(s_cmd, data + kOffDesc, 1);
dkCmdBufBindSamplerDescriptorSet(s_cmd, data + kOffDesc + 0x20, 1);
struct { float res[2]; float tex[2]; } p = {{(float)fb_w, (float)fb_h}, {(float)s_splash_w, (float)s_splash_h}};
dkCmdBufPushConstants(s_cmd, data + kOffUbo, kUboBytes, 0, sizeof(p), &p);
dkCmdBufBindUniformBuffer(s_cmd, DkStage_Vertex, 0, data + kOffUbo, kUboBytes);
dkCmdBufBindTexture(s_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
dkCmdBufDraw(s_cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
```
**Check every name in `deko3d.h`** (`DkFace_None`, `cullMode`, `DkStage_Fragment`, the
`DkViewport` field order). Compare the bind order with
`docs/deko3d-ref/examples/deko_examples/source/Example04_TexturedCube.cpp` and follow the example if it differs; mention any difference.

**3.** The 3D path needs no `dkCmdBufBarrier` before it (clear and draw are both 3D engine).

**Done-check:** `scripts/ai/check-switch.sh jak1` → `PASS`. Then temporarily set
`kSplash3D = true`, run the check again (must also PASS), and **set it back to false**.
Report both PASS lines.
