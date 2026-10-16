# T01 — UAM-ready splash shaders

**Goal:** first shaders in the deko3d shader folder: a copy of the GL splash shaders
rewritten so UAM (the deko3d shader compiler) accepts them.

**Create (2 new files):**
- `game/graphics/deko3d_renderer/shaders/splash.vert`
- `game/graphics/deko3d_renderer/shaders/splash.frag`

**Source to copy from (read, do not edit):**
`game/graphics/opengl_renderer/shaders/splash.vert`, `splash.frag`.

**Changes UAM requires** (see `docs/deko3d/uam-spike/README.md`):
1. First line `#version 460` (deko3d examples use 460; no `core`, no `precision`).
2. No loose uniforms. Put `u_res` and `u_tex` in one std140 block, binding 0:
   `layout(std140, binding = 0) uniform SplashParams { vec2 u_res; vec2 u_tex; };`
3. Sampler with explicit binding: `layout(binding = 0) uniform sampler2D splash_tex;`
4. Explicit locations on stage in/out:
   vert `layout(location = 0) out vec2 frag_uv;`
   frag `layout(location = 0) in vec2 frag_uv;` and `layout(location = 0) out vec4 out_color;`
5. Keep `main()` math identical (fullscreen strip from `gl_VertexID`, aspect fit).

Add one comment line at the top of each file:
`// deko3d port of opengl_renderer/shaders/splash.* (AI-assisted)` — after `#version`.

**Done-check (run from repo root):**
```sh
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest bash -c \
 'for s in vert frag; do /opt/devkitpro/tools/bin/uam --stage $s -o /tmp/splash.$s.dksh \
  game/graphics/deko3d_renderer/shaders/splash.$s && echo "uam $s ok"; done'
```
Expected: `uam vert ok` and `uam frag ok`.
