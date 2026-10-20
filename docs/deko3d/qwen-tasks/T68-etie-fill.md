# T68 — P3.3 prep: `fill_etie` (port of GL init_etie_cam_uniforms) + host unit test (AI-assisted)

Track D P3.3 · Depends on: T67 (EtieParams must exist) · Inert on Switch (host test only) · One session (~25 min)

## Goal

Port the GL function that computes the ETIE uniforms (`Tie3.cpp:560-584`) to a pure C++
function that fills `dktie::EtieParams`, and prove it with a host gtest. All code is given.

Reviewer trial 2026-10-20: header + test compile on the Mac, `EtieFill.*` 2/2 PASS,
dk-lint PASS. The Switch build is NOT affected (nothing on the Switch includes the header).

## Session rules (hard)

- Copy the code blocks EXACTLY. No other edits. No Switch build in this task.
- If any output differs from "Expected": STOP and report it.
- Report goes in QWEN_REPORT.md under `## T68`.

## Steps

1. Check T67 is in: `grep -c "struct EtieParams" game/graphics/deko3d_renderer/dk_tie3_prep.h`
   Expected: `1`. If `0`: STOP (T67 not done).

2. `game/graphics/deko3d_renderer/dk_tie3_prep.h`: find the line
   `inline constexpr uint32_t kEtieParamsBinding = 8u;` (added by T67). After it there is one
   blank line, then `}  // namespace dktie`. Insert this block DIRECTLY ABOVE
   `}  // namespace dktie` (keep the blank line at the end of the block):
   ```cpp
// --- T68 (AI-assisted): fill EtieParams = port of GL init_etie_cam_uniforms ----
// (Tie3.cpp:560-584) + the envmap tint set at Tie3.cpp:726. Pure math, no GL.
// Inputs are GoalBackgroundCameraData fields as raw floats (4 vec4s each):
//   rot = data.rot[0..3], perspective = data.perspective[0..3],
//   hvdf_off = data.hvdf_off, fog0 = data.fog[0].
// tint = m_common_data.envmap_color for the ETIE pass, nullptr for ETIE_BASE (zeros).
inline void fill_etie(EtieParams* out,
                      const float rot[16],
                      const float perspective[16],
                      const float hvdf_off[4],
                      float fog0,
                      const float tint[4]) {
  for (int i = 0; i < 16; i++) {
    out->cam_no_persp[i] = rot[i];
  }
  const float inv_fog = 1.f / fog0;
  const float pxx = perspective[0 * 4 + 0];  // perspective[0].x()
  const float pyy = perspective[1 * 4 + 1];  // perspective[1].y()
  const float pzz = perspective[2 * 4 + 2];  // perspective[2].z()
  const float pzw = perspective[2 * 4 + 3];  // perspective[2].w()
  const float pwz = perspective[3 * 4 + 2];  // perspective[3].z()
  const float scale = pzw * inv_fog;
  out->persp0[0] = scale * hvdf_off[0];
  out->persp0[1] = scale * hvdf_off[1];
  out->persp0[2] = scale * hvdf_off[2] + pzz;
  out->persp0[3] = scale;
  out->persp1[0] = pxx;
  out->persp1[1] = pyy;
  out->persp1[2] = pwz;
  out->persp1[3] = 0.f;
  for (int i = 0; i < 4; i++) {
    out->envmap_tod_tint[i] = tint ? tint[i] : 0.f;
  }
}

   ```

3. Create the new file `test/test_dk_etie_fill.cpp` with EXACTLY this content:
   ```cpp
// T68 (AI-assisted): dktie::fill_etie vs hand-computed values of GL
// init_etie_cam_uniforms (Tie3.cpp:560-584).
#include "game/graphics/deko3d_renderer/dk_tie3_prep.h"

#include "gtest/gtest.h"

TEST(EtieFill, PerspAndRot) {
  float rot[16];
  for (int i = 0; i < 16; i++) {
    rot[i] = 100.f + i;
  }
  float persp[16] = {};
  persp[0] = 5.f;   // pxx
  persp[5] = 6.f;   // pyy
  persp[10] = 7.f;  // pzz
  persp[11] = 8.f;  // pzw
  persp[14] = 9.f;  // pwz
  const float hvdf[4] = {1.f, 2.f, 3.f, 4.f};
  dktie::EtieParams p;
  dktie::fill_etie(&p, rot, persp, hvdf, 2.f, nullptr);
  // scale = pzw / fog0 = 8 / 2 = 4
  EXPECT_FLOAT_EQ(p.persp0[0], 4.f);
  EXPECT_FLOAT_EQ(p.persp0[1], 8.f);
  EXPECT_FLOAT_EQ(p.persp0[2], 19.f);  // 4 * 3 + 7
  EXPECT_FLOAT_EQ(p.persp0[3], 4.f);
  EXPECT_FLOAT_EQ(p.persp1[0], 5.f);
  EXPECT_FLOAT_EQ(p.persp1[1], 6.f);
  EXPECT_FLOAT_EQ(p.persp1[2], 9.f);
  EXPECT_FLOAT_EQ(p.persp1[3], 0.f);
  for (int i = 0; i < 16; i++) {
    EXPECT_FLOAT_EQ(p.cam_no_persp[i], 100.f + i);
  }
  for (int i = 0; i < 4; i++) {
    EXPECT_FLOAT_EQ(p.envmap_tod_tint[i], 0.f);
  }
}

TEST(EtieFill, Tint) {
  float rot[16] = {};
  float persp[16] = {};
  persp[11] = 1.f;
  const float hvdf[4] = {};
  const float tint[4] = {0.5f, 0.25f, 1.f, 2.f};
  dktie::EtieParams p;
  dktie::fill_etie(&p, rot, persp, hvdf, 1.f, tint);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[0], 0.5f);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[1], 0.25f);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[2], 1.f);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[3], 2.f);
}
   ```

4. `test/CMakeLists.txt`: find the line
   `        ${CMAKE_CURRENT_LIST_DIR}/test_dk_tfrag_replay.cpp`
   and add this line DIRECTLY BELOW it (same 8-space indent):
   ```cmake
        ${CMAKE_CURRENT_LIST_DIR}/test_dk_etie_fill.cpp
   ```

5. Build and run the host tests:
   ```sh
   cmake --build build-host --target goalc-test -j8 2>&1 | grep -E "error|FAILED" | head -20
   ./build-host/goalc-test --gtest_filter='EtieFill.*:DkTfragReplay.*:DkRing.*:DkBufferPool*' 2>&1 | tail -5
   python3 scripts/ai/check-dk.py
   ```
   Expected: no `error` lines; `16 tests from 4 test suites ran`, `[  PASSED  ] 15 tests.`,
   1 SKIPPED (`DkTfragReplay.MatchesSwitchGolden`, needs the Switch dump), `dk-lint: PASS`.
   If a `EtieFill` test FAILS: compare your step 2/3 text with the blocks above
   character by character (1 fix allowed), then STOP.

6. Report: the step 5 output lines, `git status --short`. Set the README T68 row to 🟡.

## Why (for the report reader)

The GL code sets uniforms one by one (`glUniform4f`). deko3d uploads one block of bytes, so
we compute the values into the struct first. A pure function (no GL, no globals) can be
tested on the Mac with known numbers: fog0=2, pzw=8 → scale=4 → persp0=(4, 8, 4*3+7=19, 4).
