// T37b host test (AI-assisted). Run from the repo root.
#include "game/graphics/deko3d_renderer/dk_tfrag_ubo.h"
#include <cstdio>
int main() {
  math::Vector4f cam[4], pc[4], hvdf, trans, fog;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) { cam[r][c] = r * 4 + c; pc[r][c] = 100 + r * 4 + c; }
  for (int i = 0; i < 4; i++) { hvdf[i] = 1 + i; trans[i] = 5 + i; fog[i] = 9 + i; }
  dktf::VertParams v;
  dktf::fill_vert(&v, cam, pc, hvdf, trans, fog, true);
  const float* vf = reinterpret_cast<const float*>(&v);
  bool ok = vf[0] == 1 && vf[4] == 5 && vf[8] == 100 && vf[23] == 115 && vf[24] == 0 &&
            vf[39] == 15 && vf[40] == 9 && vf[41] == 10 && vf[42] == 11 && v.decal == 1;
  dktf::FragParams f;
  math::Vector<uint8_t, 4> rgb;
  rgb[0] = 255; rgb[1] = 0; rgb[2] = 51; rgb[3] = 7;
  dktf::fill_frag(&f, dktf::kAlphaMinAfailSecond, 0.5f, rgb, 127.5f, false);
  const float* ff = reinterpret_cast<const float*>(&f);
  ok = ok && ff[0] == -10.f && ff[1] == 0.5f && ff[4] == 1.f && ff[5] == 0.f &&
       ff[6] == 0.2f && ff[7] == 0.5f && f.gfx_hack_no_tex == 0;
  std::printf(ok ? "T37B-HOST-OK\n" : "T37B-HOST-FAIL\n");
  return ok ? 0 : 1;
}
