# Texture-reuse census (T92, AI-assisted)

Report only. No repo code change. Numbers come from the hardware logs in `hw-logs/*/gk_run_log.txt`
(the Switch logs every upload as `[dk] tex id=… WxH rgba8 N KB` and periodically a
`[dk] tex live=… high=… fail=…` summary). Collected 2026-10-22.

All 12 hw-logs that contain a `gk_run_log.txt` are covered (the other hw-logs folders
have no `gk_run_log.txt`).

---

## 1. Per-log census

Commands used (repo root):

```sh
for f in hw-logs/*/gk_run_log.txt; do
  echo "== $f"
  grep -c "\[dk\] tex id=" "$f"
  grep "\[dk\] tex id=" "$f" | grep -o "[0-9]* KB" | awk '{s+=$1} END {print s " KB total"}'
  grep "\[dk\] tex live=" "$f" | tail -1
done
```

| log | uploads (`tex id=`) | total KB | total MB | last `tex live=` (live / high / fail) |
|---|---:|---:|---:|---|
| 20261020-1617 | 1169 | 21144 | 20.65 | `live=1169 high=1169 fail=0` |
| 20261020-1717 | 1171 | 21208 | 20.71 | `live=1171 high=1171 fail=0` |
| 20261020-1804 | 1171 | 21208 | 20.71 | `live=1171 high=1171 fail=0` |
| 20261020-1808 | 1175 | 21336 | 20.84 | `live=1175 high=1175 fail=0` |
| 20261020-1820 | 1175 | 21336 | 20.84 | `live=1175 high=1175 fail=0` |
| 20261020-2053 | 13249 | 254926 | 248.95 | `live=1215 high=1215 fail=0` |
| 20261020-2057 | 1175 | 21336 | 20.84 | `live=1215 high=1215 fail=0` |
| 20261020-2105 | 1176 | 21368 | 20.87 | `live=1216 high=1216 fail=0` |
| 20261020-2119 | 2008 | 42367 | 41.37 | `live=2048 high=2048 fail=4772` |
| 20261020-2128 | 1176 | 21368 | 20.87 | `live=1216 high=1216 fail=0 freed=0 mem=26937KB holes=0KB` |
| 20261020-2141 | 15 | 449 | 0.44 | `live=837 high=1363 fail=0 freed=4012 mem=41428KB holes=14644KB` |

Observations (facts only, no opinion):

- **Steady-state upload volume is ~21 MB / ~1175 textures** for a full session (1617→2105
  all cluster at 1169–1176 uploads, 20.65–20.87 MB).
- **20261020-2053** is the outlier: 13249 uploads / 248.95 MB but only 1215 live at the end.
  This is the known T87-era retry storm (level loads failing and retrying), per the T87
  comment in `dk_texture_gpu.cpp:60-63`. Not a normal session.
- **20261020-2119** is the only run with `fail=4772` (uploads that could not be allocated),
  reaching `live=2048 high=2048` — the 2048-slot store cap. 2008 successful `tex id=` lines
  plus 4772 failed = ~6780 alloc attempts.
- **20261020-2128** is the earliest log that prints the new T87 fields (`freed= mem= holes=`);
  it shows `freed=0 holes=0KB` → **no release happened** in that run (live == high).
- **20261020-2141** is the newest log and the only one with `freed=4012` / `holes=14644KB`
  → **releases did happen** (high 1363 − live 837 = 526 net freed, 4012 cumulative). It also
  logged only 15 uploads / 0.44 MB, i.e. a very short/early-truncated session, so its per-log
  totals are not comparable to the others; the `tex live=` lines (below) are the relevant before/after.

### First and last `tex live=` of the NEWEST log (20261020-2141), verbatim

FIRST:
```
[10.271] [dk] tex live=347 high=347 fail=0 freed=0 mem=6702KB holes=0KB | pool out=2 pooled=0 fail=0 | ring used=0/262144 hw=0
```
LAST:
```
[461.013] [dk] tex live=837 high=1363 fail=0 freed=4012 mem=41428KB holes=14644KB | pool out=81 pooled=95 fail=76 | ring used=2816/262144 hw=35328
```

Reading of the before/after (no deduction about whether this is "good"): at 10 s no range
has been freed yet (`freed=0`, `holes=0KB`), mem already at 6.5 MB. At 461 s the image-mem
cursor had reached `mem=41428KB` (~40 MB) but 14644 KB (~14.3 MB) is now in a free hole,
i.e. T87 release + coalescing is actively reclaiming image memory.

### Single largest upload line, per log

The literal size-sort in the task (`sort -t' ' -k8 -rn`) is not reliable because the KB
field is followed by trailing whitespace / the WxH column sits at `$(NF-3)`. The true max
was found by parsing the `N KB` integer per line:

| log | largest `[dk] tex id=` line |
|---|---|
| 1617 / 1717 / 1804 / 1808 / 1820 / 2053 / 2057 / 2105 / 2119 / 2128 | `[…] [dk] tex id=192 slot=1857 256x512 rgba8 512 KB` (512 KB = 0.5 MB, id=192 is the sky/texture that is always first big upload) |
| 2141 | `[23.928] [dk] tex id=767 slot=1282 8192x1 rgba8 32 KB` (only 15 uploads, so the max is a 32 KB tod palette, not a 512 KB level texture) |

---

## 2. Size-signature duplicates — SAME SIZE ONLY, NOT DEDUP PROOF

This is a FIRST CUT. Identical WxH does **not** mean identical pixels: many different
textures share a size, and the same texture can appear at one size. Treat as a lower-bound
on "how many same-size textures exist", not on how many could be deduplicated.

Command:
```sh
cat hw-logs/*/gk_run_log.txt | grep "\[dk\] tex id=" | awk '{print $(NF-3)}' | sort | uniq -c | sort -rn | head -20
```
(`$(NF-3)` because the line is `… id=N slot=N WxH rgba8 N KB`; the task literal `$(NF-2)`
returns the literal token `rgba8`, it does not carry the size.)

| WxH | count (upload lines, all logs, with repeats) |
|---|---:|
| 32x32 | 6123 |
| 64x64 | 3577 |
| 16x16 | 3087 |
| 128x128 | 1862 |
| 4x4 | 1542 |
| 8x8 | 1254 |
| 32x16 | 1065 |
| 64x32 | 936 |
| 64x128 | 746 |
| 128x64 | 637 |
| 32x64 | 593 |
| 64x16 | 403 |
| 16x32 | 350 |
| 256x128 | 278 |
| 128x32 | 255 |
| 8192x1 | 230 |
| 16x8 | 167 |
| 32x128 | 147 |
| 8x32 | 144 |
| 32x8 | 125 |

Note: counts span all 12 logs and include every retry (e.g. the 2053 retry storm and the
2119 fail=4772 run), so the totals far exceed a single session's live texture count. This
table is purely "how often each size is uploaded"; it is **not proof of duplicate pixel
data** and must be paired with a content hash before any dedup decision.

---

## 3. How texture ids are keyed today

Source: `game/graphics/texture/TexturePool.{h,cpp}` and the deko3d entry point
`game/graphics/deko3d_renderer/dk_texture_gpu.{h,cpp}`.

### 3a. The PC-port TexturePool deduplicates by *combined name*, not by pixel content.

`TexturePool.h:53-55`:
```
 * - Two textures with the same "combined name" are always identical data. (This is verified by
 *   the decompiler). So we can use the name as an ID for the texture.
```

`TexturePool.cpp:277-283` — a record (PcTextureId) is created per `(tpage dir, tex_idx)`
and the combined name string is registered into `m_name_to_id`:
```cpp
PcTextureId current_id(texture_page.id, tex_idx);
if (!m_id_to_name.lookup_existing(current_id)) {
  auto name = ... goal_string(texture_page.name_ptr,...) + goal_string(tex.name_ptr,...);
  *m_id_to_name.lookup_or_insert(current_id).first = name;
  m_name_to_id[name] = current_id;
}
```
`give_texture` (`TexturePool.cpp:100-113`) keys `m_loaded_textures` by `in.id` and only
inserts a new `GpuTexture` when `lookup_or_insert(in.id)` has no existing entry. `PcTextureId`
is allocated by `allocate_pc_port_texture` (`TexturePool.cpp:441-454`) from a monotonically
increasing `m_next_pc_texture_to_allocate`.

**Conclusion at the TexturePool layer: NO.** Two different `PcTextureId`s cannot hold
identical *named* pixel data on the TexturePool path, because the combined name is the key
and the decompiler guarantees same-name ⇒ same-bytes (the header comment above). Two ids
that share a name are merged into one `GpuTexture`. (file: `TexturePool.cpp:100-113`,
`TexturePool.h:53-55`.)

### 3b. The deko3d `dktexgpu` layer does NOT deduplicate by content.

`dk_texture_gpu.cpp:178` `upload_rgba8(w, h, src)` allocates a fresh store id + a fresh
bump-carved image region on every call, with no content comparison. The PC-port seam that
calls it is `TexturePool::upload_to_gpu` (`TexturePool.cpp:68-80`):
```cpp
u64 upload_to_gpu(const u8* data, u16 w, u16 h) {
#if defined(__SWITCH__)
  if (dkdev::is_up() && dktexgpu::active()) {
    uint32_t dk_tex = dktexgpu::upload_rgba8(w, h, data);   // new store id every call
    return dk_tex;
  }
#endif
  ...
}
```

**Conclusion at the dktexgpu layer: YES.** Two `dktexgpu` store ids *can* hold identical
pixel data today, because `upload_rgba8` allocates per-call and never compares texels.
(file: `dk_texture_gpu.cpp:178`; `TexturePool.cpp:68-80`.)

### 3c. dk-path upload entry points (grep `update_rgba8|upload_rgba8`)

`game/graphics/deko3d_renderer/`:
- `dk_texture_gpu.h:71` `uint32_t upload_rgba8(uint32_t w, uint32_t h, const void* src);`
- `dk_texture_gpu.h:74` `uint32_t upload_rgba8_mipped(...)`
- `dk_texture_gpu.h:116` `bool update_rgba8(uint32_t id, uint32_t w, uint32_t h, const void* src);`
- `dk_texture_gpu.h:123` `bool update_rgba8_deferred(...)`
- `TFragmentDk.cpp:114` calls `dktexgpu::upload_rgba8(8192, 1, s_tod_zeros)` (tod zero-palette)
- `TFragmentDk.cpp:335` calls `dktexgpu::upload_rgba8(kTodColors, 1, s_zeros)`
- `TFragmentDk.cpp:350` calls `dktexgpu::update_rgba8_deferred(e.id, ...)` per tod color update
- `TieDk.cpp:162` calls `dktexgpu::upload_rgba8(kTodColors, 1, s_zeros)` (tie tod palette)
- `dk_texture_gpu.cpp:178` definition of `upload_rgba8`; `:612` definition of `update_rgba8`;
  `:681` definition of `update_rgba8_deferred`; `:502` the bcn path reuses the same submit.

`game/graphics/texture/`:
- `TexturePool.cpp:25` `#include ".../dk_texture_gpu.h"`
- `TexturePool.cpp:78` the seam: `uint32_t dk_tex = dktexgpu::upload_rgba8(w, h, data);`
  inside `TexturePool::upload_to_gpu` under `#if defined(__SWITCH__)`.

---

## 4. dk release path

`game/graphics/deko3d_renderer/dk_texture_gpu.cpp` — `release()` (`:520-530`) and the
deferred, fence-safe free in `end_frame()` (`:532-552`):

```cpp
void release(uint32_t id) {
  if (!on_owner_thread("release")) { return; }
  if (id == 0 || s_store.slot_of(id) == UINT32_MAX) { return; }
  s_releaseQ.push_back({id, s_presents});          // queued, not freed immediately
}

void end_frame() {
  s_presents++;
  ...
  const uint32_t slot = s_store.slot_of(r.id);
  if (slot == UINT32_MAX || !s_store.release(r.id)) { continue; }
  if (s_store.slot_of(r.id) == UINT32_MAX) {        // last level ref gone
    img_free_range(s_slotOff[slot], s_slotSize[slot]);   // coalesced back to s_freeRanges
    s_slotSize[slot] = 0;
    s_released++;
  }
}
```

The freed range is re-claimed by `img_alloc_range` (`:90-112`): it scans `s_freeRanges`
(first-fit, alignment-aware) before bumping `s_cursor`, with `img_free_range` (`:69-87`)
coalescing adjacent ranges. So image memory is now recycled.

Did any release happen in the NEWEST run? **YES.** `20261020-2141`:
- LAST `tex live=`: `live=837 high=1363 fail=0 freed=4012 mem=41428KB holes=14644KB`
- `high` (1363) > `live` (837) → 526 net-id difference; `freed=4012` (cumulative releases)
  and `holes=14644KB` (~14.3 MB in reclaimable free ranges) → the release path fired.
- FIRST `tex live=` of the same run: `live=347 high=347 fail=0 freed=0 ... holes=0KB`
  → at 10 s nothing had been freed yet; by 461 s it had.

Contrast with the older `20261020-2128` run (the first that prints the T87 fields):
`live=1216 high=1216 fail=0 freed=0 mem=26937KB holes=0KB` — there, `freed=0` and
`holes=0KB` → **no release happened** in that session (an early T87 build before live frees).

---

## UNCLEAR / caveats

- `20261020-2053` (13249 uploads / 248.95 MB) and `20261020-2119` (fail=4772, live=2048)
  are not normal sessions; they inflate the size-signature table in §2. A content-hash
  census would need to exclude or separately bucket the retry-storm runs.
- `20261020-2141` logged only 15 uploads; it is an early/short capture. It is the only run
  with `freed>0`, so it is the right log for the before/after of level frees, but its
  per-log upload totals are not comparable to the 1175-upload full sessions.
- Same-size counts in §2 are upload-line counts across all logs (with retries), not a
  de-duplicated pixel census.
