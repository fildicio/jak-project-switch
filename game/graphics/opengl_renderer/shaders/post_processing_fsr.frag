#version 410 core
// AMD FidelityFX Super Resolution 1.0 -- EASU (Edge Adaptive Spatial Upsampling),
// GLSL port for the OpenGOAL Switch port (FIX 73 / PERF_PLAN step 4).
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// This replaces the bilinear final blit when the game renders below the panel
// resolution. It is a single pass: the 12-tap EASU kernel replaces the bilinear
// sample inside the same post-processing quad, with the brightness
// color_mult/color_add folded in, so no intermediate FBO is required.

uniform sampler2D tex_T0;
out vec4 color;
in vec2 tex_coord;  // unused: EASU maps gl_FragCoord through con0 instead

uniform vec4 color_mult;
uniform vec4 color_add;

// Constants from FsrEasuCon() (FSR 1.0), in gl_FragCoord form:
//   con0.xy = input/output scale (src/dst)
//   con0.zw = -draw_offset * scale - 0.5 (gl_FragCoord is window-relative and already
//             includes the +0.5 pixel-center half)
uniform vec4 con0;
// 1.0 / input width, 1.0 / input height
uniform vec2 inv_input_size;

// FsrEasuTapF: filtering for a given tap
void fsrTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp,
            vec3 c) {
  // Rotate offset by direction.
  vec2 v;
  v.x = (off.x * dir.x) + (off.y * dir.y);
  v.y = (off.x * (-dir.y)) + (off.y * dir.x);
  // Anisotropy.
  v *= len2;
  // Compute distance^2, limited to the window as at corner, 2 taps can easily be outside.
  float d2 = v.x * v.x + v.y * v.y;
  d2 = min(d2, clp);
  // Approximation of lanczos2 without sin() or rcp() or sqrt():
  //  (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2
  float wB = (2.0 / 5.0) * d2 - 1.0;
  float wA = lob * d2 - 1.0;
  wB *= wB;
  wA *= wA;
  wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
  float w = wB * wA;
  // Do weighted average.
  aC += c * w;
  aW += w;
}


// FsrEasuSetF: accumulate direction and length
void fsrSet(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
            float lA, float lB, float lC, float lD, float lE) {
  // Compute bilinear weight, branches factor out as predicates are compiler time immediates.
  //  s t
  //  u v
  float w = 0.0;
  if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
  if (biT) w =        pp.x  * (1.0 - pp.y);
  if (biU) w = (1.0 - pp.x) *        pp.y ;
  if (biV) w =        pp.x  *        pp.y ;
  // Direction is the '+' diff.
  //    a
  //  b c d
  //    e
  // Then takes magnitude from abs average of both sides of 'c'.
  float dc = lD - lC;
  float cb = lC - lB;
  float lenX = max(abs(dc), abs(cb));
  lenX = 1.0 / lenX;
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
  lenX *= lenX;
  len += lenX * w;
  // Repeat for the y axis.
  float ec = lE - lC;
  float ca = lC - lA;
  float lenY = max(abs(ec), abs(ca));
  lenY = 1.0 / lenY;
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
  lenY *= lenY;
  len += lenY * w;
}

// Fetch one EASU tap: texel (fp + offset) of the input image
vec3 tapColor(vec2 fp, float ox, float oy) {
  return texture(tex_T0, (fp + vec2(ox, oy) + 0.5) * inv_input_size).rgb;
}

void main() {
  // Position of 'f' in input pixel coordinates.
  vec2 pp = gl_FragCoord.xy * con0.xy + con0.zw;
  vec2 fp = floor(pp);
  pp -= fp;

  // 12-tap kernel.
  //     b  c
  //  e  f  g  h
  //  i  j  k  l
  //     n  o
  vec3 b = tapColor(fp,  0.0, -1.0);
  vec3 c = tapColor(fp,  1.0, -1.0);
  vec3 e = tapColor(fp, -1.0,  0.0);
  vec3 f = tapColor(fp,  0.0,  0.0);
  vec3 g = tapColor(fp,  1.0,  0.0);
  vec3 h = tapColor(fp,  2.0,  0.0);
  vec3 i = tapColor(fp, -1.0,  1.0);
  vec3 j = tapColor(fp,  0.0,  1.0);
  vec3 k = tapColor(fp,  1.0,  1.0);
  vec3 l = tapColor(fp,  2.0,  1.0);
  vec3 n = tapColor(fp,  0.0,  2.0);
  vec3 o = tapColor(fp,  1.0,  2.0);

  // Simplest multi-channel approximate luma possible (luma times 2, in 2 FMA/MAD).
  float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
  float cL = c.b * 0.5 + (c.r * 0.5 + c.g);
  float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
  float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
  float gL = g.b * 0.5 + (g.r * 0.5 + g.g);
  float hL = h.b * 0.5 + (h.r * 0.5 + h.g);
  float iL = i.b * 0.5 + (i.r * 0.5 + i.g);
  float jL = j.b * 0.5 + (j.r * 0.5 + j.g);
  float kL = k.b * 0.5 + (k.r * 0.5 + k.g);
  float lL = l.b * 0.5 + (l.r * 0.5 + l.g);
  float nL = n.b * 0.5 + (n.r * 0.5 + n.g);
  float oL = o.b * 0.5 + (o.r * 0.5 + o.g);

  // Accumulate for bilinear interpolation.
  vec2 dir = vec2(0.0);
  float len = 0.0;
  fsrSet(dir, len, pp, true , false, false, false, bL, eL, fL, gL, jL);
  fsrSet(dir, len, pp, false, true , false, false, cL, fL, gL, hL, kL);
  fsrSet(dir, len, pp, false, false, true , false, fL, iL, jL, kL, nL);
  fsrSet(dir, len, pp, false, false, false, true , gL, jL, kL, lL, oL);

  // Normalize with approximation, and cleanup close to zero.
  vec2 dir2 = dir * dir;
  float dirR = dir2.x + dir2.y;
  bool zro = dirR < (1.0 / 32768.0);
  dirR = 1.0 / sqrt(dirR);
  dirR = zro ? 1.0 : dirR;
  dir.x = zro ? 1.0 : dir.x;
  dir *= dirR;
  // Transform from {0 to 2} to {0 to 1} range, and shape with square.
  len *= 0.5;
  len *= len;
  // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
  float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
  // Anisotropic length after rotation,
  //  x := 1.0 lerp to 'stretch' on edges
  //  y := 1.0 lerp to 2x on edges
  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  // Based on the amount of 'edge',
  // the window shifts from +/-{sqrt(2.0) to slightly beyond 2.0}.
  float lob = 0.5 + (1.0 / 4.0 - 0.04 - 0.5) * len;
  // Set distance^2 clipping point to the end of the adjustable window.
  float clp = 1.0 / lob;

  // Accumulation mixed with min/max of 4 nearest (f, g, j, k).
  vec3 min4 = min(min(f, g), min(j, k));
  vec3 max4 = max(max(f, g), max(j, k));
  vec3 aC = vec3(0.0);
  float aW = 0.0;
  fsrTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
  fsrTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
  fsrTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
  fsrTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
  fsrTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
  fsrTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
  fsrTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
  fsrTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
  fsrTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
  fsrTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
  fsrTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
  fsrTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);

  // Normalize and dering.
  vec3 pix = min(max4, max(min4, aC / aW));

  // Brightness / contrast fold from post_processing.frag.
  color = vec4(pix * color_mult.rgb * color_mult.a, 1.0) + color_add;
}
