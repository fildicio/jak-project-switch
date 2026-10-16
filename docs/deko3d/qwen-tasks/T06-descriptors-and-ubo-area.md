# T06 — Descriptor sets and a UBO area for the splash

**Goal:** the splash image gets an image descriptor + sampler descriptor (so a shader can
sample it), and the data pool gets a small uniform-buffer area. Still nothing drawn (T07).

**Edit (1 file):** `game/graphics/pipelines/dk_device.cpp`.

**1. Data pool layout.** Today: `[cmd slices][staging]`. Append two regions:
```cpp
constexpr uint32_t kOffDesc = kOffStaging + kStagingMax;          // image desc @+0, sampler desc @+0x20
constexpr uint32_t kDescBytes = 0x100;
constexpr uint32_t kOffUbo = kOffDesc + kDescBytes;               // must be DK_UNIFORM_BUF_ALIGNMENT-aligned
constexpr uint32_t kUboBytes = 0x100;                             // one SplashParams (16 B used)
constexpr uint32_t kDataPool = kOffUbo + kUboBytes;
static_assert(kOffDesc % DK_IMAGE_DESCRIPTOR_ALIGNMENT == 0, "desc align");
static_assert(kOffUbo % DK_UNIFORM_BUF_ALIGNMENT == 0, "ubo align");
```
Replace the old `kDataPool` definition (don't keep two).

**2. Descriptors**, at the end of `upload_splash()` (after the image is uploaded):
```cpp
DkImageView sv; dkImageViewDefaults(&sv, &s_splash);
DkImageDescriptor idesc; dkImageDescriptorInitialize(&idesc, &sv, false, false);
DkSampler smp; dkSamplerDefaults(&smp);
smp.minFilter = DkFilter_Linear; smp.magFilter = DkFilter_Linear;
smp.wrapMode[0] = smp.wrapMode[1] = smp.wrapMode[2] = DkWrapMode_ClampToEdge;
DkSamplerDescriptor sdesc; dkSamplerDescriptorInitialize(&sdesc, &smp);
uint8_t* base = (uint8_t*)dkMemBlockGetCpuAddr(s_dataMem);
memcpy(base + kOffDesc, &idesc, sizeof(idesc));
memcpy(base + kOffDesc + 0x20, &sdesc, sizeof(sdesc));
s_have_desc = true;
```
**Verify every field name** (`minFilter`, `magFilter`, `wrapMode`) in `struct DkSampler`
in `deko3d.h` — if a name differs, use the header's name and say so.
Add `bool s_have_desc = false;` with the other statics; reset it in `shutdown()`.
Writing descriptors with memcpy is fine: the data block is `CpuUncached`.

**Done-check:** `scripts/ai/check-switch.sh jak1` → `PASS`.
