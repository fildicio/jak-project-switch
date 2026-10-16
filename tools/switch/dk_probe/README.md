# dk_probe — deko3d P0.4 triangle NRO + per-draw cost probe (AI-assisted)

Standalone homebrew (not the game). One launch on hardware answers the open
P0.4/P0.6 questions in `DEKO3D_MIGRATION_PLAN.md`:

1. deko3d device / swapchain / memory pools come up (plain hbmenu launch, no Mesa);
2. the offline UAM-compiled repo shader `tfrag3.{vert,frag}.dksh` loads;
3. per-draw cost on deko3d: 1k / 3k / 6k / 10k draws per frame, each with a UBO
   update (`dkCmdBufPushConstants`) + texture rebind + draw — the same shape as
   the GL renderers' `glUniform*` + `glBindTexture` + `glDraw*` pattern
   (GL today: ~850 draws, 5–15k GL calls, 11.6 ms/frame, `[glt]` run 3);
4. process memory before/after device and pools (`[mem]` lines, P0.6 input).

Runs by itself for ~15 s (a grid of flickering textured triangles), then exits
to hbmenu. `+` quits early. Log: `sdmc:/switch/dk_probe/dk_probe_log.txt`,
fsync'd per line so a crash still leaves the last good step.

Build (repo root):

```
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v "$PWD":/work \
  -w /work/tools/switch/dk_probe -e DEVKITPRO=/opt/devkitpro devkitpro/devkita64 \
  bash -c 'export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH; make'
```

Install: copy `dk_probe.nro` to `sdmc:/switch/dk_probe/dk_probe.nro`.
`romfs/tfrag3.*.dksh` are copies of `docs/deko3d/uam-spike/` artifacts.
