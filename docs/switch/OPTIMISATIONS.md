# Switch performance and loading optimisations (AI-assisted)

This catalog lists every optimisation tried in the Jak 1 Switch port, including the ones that were kept, reverted or rejected. It is meant for reuse in other libnx + Mesa OpenGL ports, such as an OpenRAC-NX fork.

Sources:
- `SWITCH_PORT_SESSION_NOTES.md` (FIX numbers)
- `performance-improvement.md`
- `DEKO3D_MIGRATION_PLAN.md`
- `docs/deko3d/qwen-tasks/` (T numbers)
- `docs/deko3d/DKGL_TODO.md`

Portable means the idea works on a Mesa-GL libnx game without deko3d.

> **Rule zero (FIX 56):** profile before changing anything. Split each frame into
> CPU game logic, GL call submission, GPU time and I/O. Our "slow loading" turned
> out to be texture uploads and loader policy, not the renderer API.

## 1. Frame pacing (apply first: cheap, high impact)

| ID | Problem | Fix | Status | Portable |
|---|---|---|---|---|
| FIX 10 | A software frame limiter stacked on top of vsync made frames alternate between 16 and 33 ms. | Disable the limiter when vsync is on, and pick swap interval 1 or 2 from the target fps (`opengl.cpp`). | KEPT | yes |
| FIX 11 | A lost condition-variable wakeup between the engine and renderer caused 8–20 % duplicate frames. | The waiter and the notifier share one mutex/cv (`dma_mutex`/`dma_cv`). | KEPT | yes |
| FIX 13 | Acquiring the swapchain at frame start added about 31 ms, giving 53 ms frames. | Defer the default-framebuffer clear until the final blit, so the acquire overlaps rendering. | KEPT | yes |
| FIX 17 | Truncating the sim tick dropped the fractional time each frame, so time drifted. | Carry the tick fraction over to the next frame. | KEPT | partly (fixed-step engines) |
| FIX 21–23 | Camera springs and smoothers jittered below 60 fps. | Frame-time-compensated spring, follow and rotation blends. | KEPT | partly (game-specific) |
| FIX 24 | The swap interval was not reliably applied. | Verify the interval and cache it after setting it. | KEPT | yes |
| FIX 14 | Timeout frames were presented with nothing new in them. | Suppress the present on timeout. | REVERTED (FIX 19; boot deadlock) | avoid |

## 2. CPU draw submission / GL driver overhead (usually the biggest win on Mesa)

| ID | Problem | Fix | Status | Portable |
|---|---|---|---|---|
| FIX 29 | Hundreds of per-draw `glGetUniformLocation` calls and redundant binds, plus a `GL_MAX_SAMPLES` query every frame. | Cache uniform locations, skip binds that are already set, and query limits once. | KEPT | yes |
| FIX 43 | About 1,147 draws per frame plus 5–7k `glTexParameter` calls cost about 32 ms of CPU. | Use 16 pre-built sampler objects instead of per-draw texture parameters, and memoise uniform locations. | KEPT | yes |
| FIX 43a | Sampler objects crashed because glad left the GLES entry points null. | Re-resolve them with `SDL_GL_GetProcAddress`, null-check, and fall back. | KEPT | yes (**check every GL pointer**) |
| FIX 37 | Time-of-day colour interpolation plus palette upload cost about 12 ms per frame. | Per-tree cache, recomputing at most 3–8 trees per frame on Switch. | KEPT | yes (cache unchanged per-object state) |
| dkgl | Many redundant GL state calls per draw. | Shadow GL state on the CPU and emit only the changes at draw time. | KEPT | yes (a state cache in front of GL) |
| P0.3/0.4 | GL draw projection had a p95 of 10.6 ms. Natively, 10,000 draws recorded in 0.98 ms. | Offline-compiled shaders, a static pipeline table, explicit std140 UBOs. | KEPT | partly (program binaries / UBOs in GL) |
| Capture/record | GPU submission was on the game thread. | The game thread captures DMA chains and the render thread records and submits them. | KEPT | yes (render queue) |

## 3. Texture upload (load time and pop-in)

| ID | Problem | Fix | Status | Portable |
|---|---|---|---|---|
| FIX 39 | A `glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY)` call per texture cost about 2.2 ms per texture. | Query it once per process. Uploads dropped to about 0.79 ms per texture. | KEPT | **yes (huge win)** |
| FIX 42 | Mipmap generation cost about 0.43 ms per texture, i.e. 82–360 ms of hitches during loads. | Queue `glGenerateMipmap` and drain it over frames. Use `MAX_LEVEL=0` until generated, then restore it (FIX 42a). | KEPT | yes |
| FIX 47/48 | Uploads took up to 2.1 s for 1,222 textures, and format conversion on the render thread cost 2.43 ms each. | Upload the raw data as `GL_UNSIGNED_BYTE` with no CPU swizzle. FIX 47's byte swap gave purple textures and was removed. | KEPT | yes (verify the byte order first) |
| FIX 52 | Fixed upload budgets stalled while the GPU was busy. | Measure the upload/mip GPU cost and scale the dispatch to GPU pressure. | KEPT | yes |
| FIX 66 | The dispatch count was capped at 4 textures even with a 1 MB budget, so pop-in lasted 3–5 s. | Derive the count (4–20) from the byte budget. | KEPT | yes |
| T18/T58 | Uploads waited behind vsync: 16.6 ms each. | Separate upload queue with staging and a fence. Uploads now take 0.05–0.3 ms. | KEPT | partly (PBOs in GL) |
| T88/T92 | Minified textures without mips thrash the texture cache. | Build mip chains on the CPU or offline. | KEPT | yes |
| T87/T89 | Texture memory was fragmented by level reloads. | Fence-delayed release plus a coalescing free-list. | KEPT | yes (pool GL textures and delete them late) |
| FIX 33a / 34 | Band-texture and alternate-GLES-pixel-type paths. | — | REVERTED (boot crash/freeze) | avoid untested format changes |
| FIX 69 | PBO async upload. | Experimental branch only. | NOT SHIPPED | worth trying, test carefully |

## 4. Loader / streaming policy

| ID | Problem | Fix | Status | Portable |
|---|---|---|---|---|
| FIX 9 | 1 MB uploads plus a 4.5 ms loader budget caused hitches of more than 20 ms. | Use a 2 ms budget, and cut vertex, index and texture chunks from 1 MB to 256 KB. | KEPT | yes |
| FIX 33 / 46 | The live-level cap was too low, so levels were evicted and reloaded over and over. | Derive the cap from the real residency, with a minimum reclaim age, and never evict a level the game still wants. | KEPT | yes |
| FIX 36 / 38 | Flat budgets starved the backlog. | Per-phase timers, and give the backlog a minimum service floor that overrides frame pressure. | KEPT | yes |
| FIX 68 | A 5 ms / 1 MB pace allowed only about 3 textures per frame, and the mip backlog shimmered for 7–14 s. | A 7 ms / 2 MB catch-up tier, plus a mip drain that scales with the backlog. | KEPT | yes |
| FIX 103c | A later EMA-driven cap caused a streaming "death spiral". | Roll back to the known-good v1.1 loader. | KEPT (rollback) | lesson: never starve streaming |

## 5. File I/O and saves

| ID | Problem | Fix | Status | Portable |
|---|---|---|---|---|
| FIX 65 | One 32 KB `fread` plus a service round-trip per page. | 256 KB sequential read-ahead, with inline reads. | KEPT | **yes** |
| FIX 35 | Repeated saves froze for about 270 ms. | Skip saves whose payload checksum hasn't changed. | KEPT (frame-sliced writes reverted) | yes |
| FIX 34c | A threaded save worker crashed. | Keep saves synchronous. | KEPT | yes |
| FIX 40 / 41 / 67 | Synchronous SD log writes cost 4.7 ms per frame, with 150–260 ms spikes. | 64 KB in-memory log buffers, batched flushes, and telemetry off by default. | KEPT | **yes** |

## 6. GPU / fill rate

| ID | Problem | Fix | Status | Portable |
|---|---|---|---|---|
| FIX 30 | The game rendered at 1080p despite a 720p setting: 2.25× the pixel work. | Honour the resolution setting; handheld follows the operation mode. | KEPT | **yes (check this first)** |
| #17 | Screen-sized particle sprites caused 4.6–5 ms of blended overdraw. | Fade out and drop near-camera sprites in the vertex shader (`sprite3.vert.glsl`). | KEPT (awaiting HW test) | yes |
| Early-Z | Try early depth testing on sprites. | `layout(early_fragment_tests)` | KEPT, no gain (blended) | yes |
| FIX 16 / 39 LoadBoost | Lower or dynamic internal resolution. | — | REVERTED (pacing issues; an FBO resize wedged boot) | avoid |

## 7. Architecture ideas (deko3d era; partly portable)

- Per-frame ring buffers with "renaming": every dynamic VBO/UBO upload gets a new slice, so the GPU never reads overwritten data (`dk_ring.h`). In GL, use buffer orphaning or persistent-mapped rings.
- Fence-guarded recycling: a resource is freed only after the GPU finishes the frame that used it.
- Snapshot uniforms per draw. With one shared UBO, every draw sees the final value.
- One barrier before the first sampling, not a barrier per upload.
- Per-pass GPU timestamps (T64): measure each pass, not whole frames blurred by vsync.
- SD flag files for A/B tests (`gk_dk*.txt`): toggle a feature without rebuilding.
- Host replay tests (`docs/deko3d/REPLAY_TEST.md`) before every SD trip.
- dkgl shim: keep the existing GL renderers and swap the backend underneath (`dkgl/`).

## 8. Rules learned (the short version)

1. Profile first, and split the frame into CPU, GL calls, GPU and I/O (FIX 56).
2. Never stack a frame limiter on vsync (FIX 10).
3. A cv predicate and its notify share one mutex (FIX 11).
4. Query GL limits once and cache uniforms and binds (FIX 29, 39).
5. Use sampler objects, not per-draw `glTexParameter` (FIX 43). Null-check every GL pointer (43a).
6. Defer mipmap generation, and restore `MAX_LEVEL` afterwards (FIX 42).
7. Tie texture caps to byte budgets, and give streaming a minimum floor (FIX 66, 38, 103c).
8. Batch sequential reads: 256 KB read-ahead (FIX 65).
9. Never write to the SD synchronously per frame; ship with logging off (FIX 40, 41, 67).
10. Don't create new runtime `std::thread`s on libnx (FIX 34b). Don't keep a `FILE*` open across frames (FIX 38).
11. Make sure the render resolution is what you think it is (FIX 30).
12. Hardware-test texture-format changes; Mesa/nouveau may crash (FIX 33a, 34).
