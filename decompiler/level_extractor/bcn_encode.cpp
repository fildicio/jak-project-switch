#include "decompiler/level_extractor/bcn_encode.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "common/util/Assert.h"

// FIX 74 (AI-assisted): hand-written BC1/BC3 encoder (see header for rationale).
// The bit layouts follow the S3TC/DXT spec exactly:
//  - BC1 block (8 bytes): u16 c0, u16 c1 (RGB565), then 16x 2-bit indices (LSB = pixel 0,
//    row-major). We only emit 4-color mode (c0 >= c1); punchthrough alpha is never needed.
//  - BC3 block (16 bytes): 8-byte alpha block (u16 a0, u16 a1, 16x 3-bit indices in bits
//    16..63) followed by a BC1 color block on RGB.
// Endpoint fitting: bounding box + extremes projected on the bbox diagonal (and the bbox
// corners as the alternate candidate), picking whichever candidate has lower quantization
// error. Good enough for PS2-era level art at 720p, and it is a one-time extract cost.

namespace bcn {
namespace {

using Pixel = std::array<u8, 4>;  // r, g, b, a

Pixel unpack_word(u32 word) {
  // tfrag3::Texture words are 0xAABBGGRR
  return {u8(word & 0xff), u8((word >> 8) & 0xff), u8((word >> 16) & 0xff), u8(word >> 24)};
}

u32 pack_word(const Pixel& p) {
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

Pixel avg4(const Pixel& a, const Pixel& b, const Pixel& c, const Pixel& d) {
  Pixel out;
  for (int i = 0; i < 4; i++) {
    out[i] = u8((u32(a[i]) + b[i] + c[i] + d[i] + 2) >> 2);
  }
  return out;
}

// 2x2 box filter with clamped edges - the same filter class glGenerateMipmap
// implements. Straight alpha (no premultiply) to match the runtime mips the
// game was rendering with before FIX 74.
std::vector<u32> half_size(const std::vector<u32>& src, u32 w, u32 h) {
  const u32 nw = std::max(1u, w / 2);
  const u32 nh = std::max(1u, h / 2);
  std::vector<u32> out(nw * nh);
  for (u32 y = 0; y < nh; y++) {
    const u32 y0 = 2 * y;
    const u32 y1 = std::min(2 * y + 1, h - 1);
    for (u32 x = 0; x < nw; x++) {
      const u32 x0 = 2 * x;
      const u32 x1 = std::min(2 * x + 1, w - 1);
      out[y * nw + x] = pack_word(avg4(unpack_word(src[y0 * w + x0]), unpack_word(src[y0 * w + x1]),
                                       unpack_word(src[y1 * w + x0]), unpack_word(src[y1 * w + x1])));
    }
  }
  return out;
}

u16 pack_565(const Pixel& p) {
  return u16(((u32(p[0]) >> 3) << 11) | ((u32(p[1]) >> 2) << 5) | (u32(p[2]) >> 3));
}

Pixel expand_565(u16 c) {
  const u32 r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
  return {u8((r5 << 3) | (r5 >> 2)), u8((g6 << 2) | (g6 >> 4)), u8((b5 << 3) | (b5 >> 2)), 255};
}

// Pick the nearest palette entry per pixel; returns the total squared error.
u32 pick_indices_color(const Pixel px[16], const Pixel pal[4], u8 indices[16]) {
  u32 err = 0;
  for (int i = 0; i < 16; i++) {
    u32 best = UINT32_MAX;
    u8 best_i = 0;
    for (u8 c = 0; c < 4; c++) {
      const int dr = int(px[i][0]) - int(pal[c][0]);
      const int dg = int(px[i][1]) - int(pal[c][1]);
      const int db = int(px[i][2]) - int(pal[c][2]);
      const u32 e = u32(dr * dr + dg * dg + db * db);
      if (e < best) {
        best = e;
        best_i = c;
      }
    }
    indices[i] = best_i;
    err += best;
  }
  return err;
}

struct Bc1Candidate {
  u16 c0 = 0, c1 = 0;
  u8 idx[16] = {};
  u32 err = UINT32_MAX;
};

Bc1Candidate evaluate_bc1(const Pixel px[16], const Pixel& a, const Pixel& b) {
  u16 ca = pack_565(a), cb = pack_565(b);
  // 4-color mode needs code(c0) >= code(c1). The 4-entry palette set is
  // symmetric under swapping c0/c1 (the two mixed entries map onto each
  // other), so swapping only relabels the indices.
  if (ca < cb) {
    std::swap(ca, cb);
  }
  const Pixel p0 = expand_565(ca), p1 = expand_565(cb);
  Pixel pal[4] = {p0, p1, p0, p0};
  for (int ch = 0; ch < 3; ch++) {
    pal[2][ch] = u8((2 * p0[ch] + p1[ch]) / 3);
    pal[3][ch] = u8((p0[ch] + 2 * p1[ch]) / 3);
  }
  Bc1Candidate out;
  out.c0 = ca;
  out.c1 = cb;
  out.err = pick_indices_color(px, pal, out.idx);
  return out;
}

void encode_bc1_block(const Pixel px[16], u8* out) {
  // bounding box over RGB
  Pixel lo{255, 255, 255, 255}, hi{0, 0, 0, 0};
  for (int i = 0; i < 16; i++) {
    for (int ch = 0; ch < 3; ch++) {
      lo[ch] = std::min(lo[ch], px[i][ch]);
      hi[ch] = std::max(hi[ch], px[i][ch]);
    }
  }
  // candidate 1: actual extreme pixels, projected on the bbox diagonal
  // candidate 2: the bbox corners themselves
  Pixel e0 = lo, e1 = hi;
  const int dR = hi[0] - lo[0], dG = hi[1] - lo[1], dB = hi[2] - lo[2];
  const long long axis_len_sq = (long long)dR * dR + (long long)dG * dG + (long long)dB * dB;
  if (axis_len_sq > 0) {
    int tmin = INT32_MAX, tmax = INT32_MIN;
    for (int i = 0; i < 16; i++) {
      const int t = int(px[i][0] - lo[0]) * dR + int(px[i][1] - lo[1]) * dG +
                    int(px[i][2] - lo[2]) * dB;
      if (t < tmin) {
        tmin = t;
        e0 = px[i];
      }
      if (t > tmax) {
        tmax = t;
        e1 = px[i];
      }
    }
  }
  Bc1Candidate best = evaluate_bc1(px, e0, e1);
  const Bc1Candidate corners = evaluate_bc1(px, lo, hi);
  if (corners.err < best.err) {
    best = corners;
  }
  // pack: c0, c1, then 16x2-bit indices (pixel i at bit 2*i)
  u32 bits = 0;
  for (int i = 0; i < 16; i++) {
    bits |= u32(best.idx[i]) << (2 * i);
  }
  const u16 words[4] = {best.c0, best.c1, u16(bits & 0xffff), u16((bits >> 16) & 0xffff)};
  memcpy(out, words, 8);
}

void build_alpha_pal_8(u8 a0, u8 a1, u8 pal[8]) {
  pal[0] = a0;
  pal[1] = a1;
  for (int i = 2; i < 8; i++) {
    pal[i] = u8(((8 - i) * int(a0) + (i - 1) * int(a1)) / 7);
  }
}

void build_alpha_pal_6(u8 a0, u8 a1, u8 pal[8]) {
  pal[0] = a0;
  pal[1] = a1;
  for (int i = 2; i < 6; i++) {
    pal[i] = u8(((6 - i) * int(a0) + (i - 1) * int(a1)) / 5);
  }
  pal[6] = 0;
  pal[7] = 255;
}

u32 pick_indices_alpha(const Pixel px[16], const u8 pal[8], u8 indices[16]) {
  u32 err = 0;
  for (int i = 0; i < 16; i++) {
    u32 best = UINT32_MAX;
    u8 best_i = 0;
    for (u8 c = 0; c < 8; c++) {
      const int d = int(px[i][3]) - int(pal[c]);
      const u32 e = u32(d * d);
      if (e < best) {
        best = e;
        best_i = c;
      }
    }
    indices[i] = best_i;
    err += best;
  }
  return err;
}

void encode_bc3_alpha(const Pixel px[16], u8* out) {
  u8 amin = 255, amax = 0;
  for (int i = 0; i < 16; i++) {
    amin = std::min(amin, px[i][3]);
    amax = std::max(amax, px[i][3]);
  }
  u8 a0 = amin, a1 = amin;
  u8 idx[16];
  memset(idx, 0, sizeof(idx));
  if (amin != amax) {
    // 8-alpha interpolation mode needs a0 > a1; 6-alpha mode needs a0 <= a1.
    u8 pal[8];
    build_alpha_pal_8(amax, amin, pal);
    u8 idx8[16];
    const u32 e8 = pick_indices_alpha(px, pal, idx8);
    build_alpha_pal_6(amin, amax, pal);
    u8 idx6[16];
    const u32 e6 = pick_indices_alpha(px, pal, idx6);
    if (e6 < e8) {
      a0 = amin;
      a1 = amax;
      memcpy(idx, idx6, 16);
    } else {
      a0 = amax;
      a1 = amin;
      memcpy(idx, idx8, 16);
    }
  }
  // pack: a0, a1, then 16x3-bit indices in bits 16..63 (pixel i at bit 16+3*i)
  u64 block = u64(a0) | (u64(a1) << 8);
  for (int i = 0; i < 16; i++) {
    block |= u64(idx[i]) << (16 + 3 * i);
  }
  memcpy(out, &block, 8);
}

}  // namespace

bool encode_texture(const std::vector<u32>& rgba, u16 w, u16 h, EncodedTexture* out) {
  if (w == 0 || h == 0 || rgba.size() != size_t(w) * size_t(h)) {
    return false;
  }
  // any non-opaque pixel -> BC3 (BC1 has no usable alpha in 4-color mode)
  bool opaque = true;
  for (u32 word : rgba) {
    if ((word >> 24) != 0xff) {
      opaque = false;
      break;
    }
  }
  out->format = opaque ? FMT_BC1 : FMT_BC3;

  // build the full mip chain source levels (base first, down to 1x1)
  std::vector<std::vector<u32>> mips;
  mips.push_back(rgba);
  u32 mw = w, mh = h;
  while (mw > 1 || mh > 1) {
    mips.push_back(half_size(mips.back(), mw, mh));
    mw = std::max(1u, mw / 2);
    mh = std::max(1u, mh / 2);
  }

  out->data.clear();
  out->mip_offsets.clear();
  out->mip_offsets.reserve(mips.size());
  const u32 bytes_per_block = opaque ? 8 : 16;
  for (size_t level = 0; level < mips.size(); level++) {
    const u32 lw = std::max(1u, u32(w) >> level);
    const u32 lh = std::max(1u, u32(h) >> level);
    const u32 blocks_x = (lw + 3) / 4;
    const u32 blocks_y = (lh + 3) / 4;
    out->mip_offsets.push_back(u32(out->data.size()));
    out->data.resize(out->data.size() + size_t(blocks_x) * blocks_y * bytes_per_block);
    u8* dst = out->data.data() + out->mip_offsets.back();
    const std::vector<u32>& level_px = mips[level];
    for (u32 by = 0; by < blocks_y; by++) {
      for (u32 bx = 0; bx < blocks_x; bx++) {
        Pixel block[16];
        for (int py = 0; py < 4; py++) {
          const u32 sy = std::min(by * 4 + py, lh - 1);
          for (int px = 0; px < 4; px++) {
            const u32 sx = std::min(bx * 4 + px, lw - 1);
            block[py * 4 + px] = unpack_word(level_px[sy * lw + sx]);
          }
        }
        if (!opaque) {
          encode_bc3_alpha(block, dst);
        }
        encode_bc1_block(block, dst + (opaque ? 0 : 8));
        dst += bytes_per_block;
      }
    }
  }
  return true;
}

}  // namespace bcn
