/*
 * dk_probe -- deko3d migration P0.4 "triangle NRO" + per-draw cost probe. (AI-assisted)
 *
 * Standalone homebrew, NOT the game. One launch answers what the plan needs
 * before P1, on real hardware:
 *   1. does deko3d come up (device, swapchain, pools)?
 *   2. do the offline-compiled repo shaders (tfrag3 .dksh, UAM spike) load?
 *   3. what does a draw cost on deko3d? Bench: N draws/frame, each with a
 *      per-draw UBO update (dkCmdBufPushConstants) + texture rebind + draw --
 *      the same shape as the GL renderers' glUniform* + glBindTexture + glDraw*
 *      pattern that costs 11.6 ms/frame on Mesa today ([glt] run 3).
 *   4. how much process memory do deko3d's pools take (P0.6 input)?
 *
 * Every step is logged to sdmc:/switch/dk_probe/dk_probe_log.txt, flushed and
 * fsync'd immediately, so a crash still leaves the last good step on the card.
 * Fully automatic: launch, watch triangles ~15 s, it exits by itself (+ quits early).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <deko3d.h>
#include <switch.h>

namespace {

// ---------------------------------------------------------------- logging
FILE* g_log = nullptr;
u64 g_t0 = 0;

double now_ms() {
  return (double)armTicksToNs(armGetSystemTick() - g_t0) / 1.0e6;
}

void logf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_log) {
    fprintf(g_log, "[%9.3f] %s\n", now_ms(), buf);
    fflush(g_log);
    fsync(fileno(g_log));
  }
}

void dk_debug_cb(void*, const char* context, DkResult result, const char* message) {
  logf("[dk-debug] ctx=%s result=%d msg=%s", context ? context : "?", (int)result,
       message ? message : "?");
}

void log_mem(const char* tag) {
  u64 used = 0, total = 0;
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  logf("[mem] %-16s used=%llu KB total=%llu KB", tag, (unsigned long long)(used / 1024),
       (unsigned long long)(total / 1024));
}

void log_clocks() {
  // plan section 9: every capture records the clocks. Failure is not fatal.
  if (R_FAILED(clkrstInitialize())) {
    logf("[clk] clkrst unavailable");
    return;
  }
  const struct {
    PcvModuleId id;
    const char* name;
  } mods[] = {{PcvModuleId_CpuBus, "cpu"}, {PcvModuleId_GPU, "gpu"}, {PcvModuleId_EMC, "emc"}};
  for (const auto& m : mods) {
    ClkrstSession s;
    u32 hz = 0;
    if (R_SUCCEEDED(clkrstOpenSession(&s, m.id, 3))) {
      clkrstGetClockRate(&s, &hz);
      clkrstCloseSession(&s);
    }
    logf("[clk] %s = %u MHz", m.name, hz / 1000000);
  }
  clkrstExit();
}

// ---------------------------------------------------------------- config
constexpr unsigned kFbNum = 2;
constexpr unsigned kFbW = 1280;
constexpr unsigned kFbH = 720;
constexpr unsigned kTexDim = 64;
constexpr unsigned kNumTex = 2;

constexpr uint32_t kCodePool = 256 * 1024;
constexpr uint32_t kCmdSlice = 4 * 1024 * 1024;  // per in-flight frame
// data pool layout: [cmd slice 0][cmd slice 1][ubo][img desc][smp desc][staging]
constexpr uint32_t kOffUbo = kFbNum * kCmdSlice;
constexpr uint32_t kOffImgDesc = kOffUbo + 0x100;
constexpr uint32_t kOffSmpDesc = kOffImgDesc + 0x100;
constexpr uint32_t kOffStaging = kOffSmpDesc + 0x100;
constexpr uint32_t kDataPool = kOffStaging + 1 * 1024 * 1024;

// bench: draws per frame (GL today: ~850 draws, 5-15k GL calls per frame)
constexpr unsigned kBenchDraws[] = {1000, 3000, 6000, 10000};
constexpr unsigned kFramesPerStep = 180;  // ~3 s each at 60 Hz
constexpr unsigned kWarmup = 20;
constexpr unsigned kBytesPerDrawBudget = 128;  // generous cmd-memory estimate

struct Params {
  float offset_scale[4];
  float color[4];
};
static_assert(sizeof(Params) == 32, "std140 layout");

// ---------------------------------------------------------------- state
DkDevice g_dev;
DkQueue g_queue;
DkMemBlock g_fbMem, g_codeMem, g_dataMem, g_imgMem;
DkImage g_fb[kFbNum];
DkSwapchain g_swap;
DkShader g_vsh, g_fsh;
DkCmdBuf g_cmd;
DkFence g_sliceFence[kFbNum];
uint32_t g_codeOff = 0;
DkImage g_tex[kNumTex];
DkGpuAddr g_uboAddr, g_imgDescAddr, g_smpDescAddr;

uint8_t* data_cpu(uint32_t off) {
  return (uint8_t*)dkMemBlockGetCpuAddr(g_dataMem) + off;
}

uint32_t align_up(uint32_t v, uint32_t a) {
  return (v + a - 1) & ~(a - 1);
}

bool load_shader(DkShader* sh, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    logf("[shader] FAIL open %s", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  const uint32_t size = (uint32_t)ftell(f);
  rewind(f);
  const uint32_t aligned = align_up(size, DK_SHADER_CODE_ALIGNMENT);
  if (g_codeOff + aligned + DK_SHADER_CODE_UNUSABLE_SIZE > kCodePool) {
    logf("[shader] FAIL code pool full for %s", path);
    fclose(f);
    return false;
  }
  fread((uint8_t*)dkMemBlockGetCpuAddr(g_codeMem) + g_codeOff, size, 1, f);
  fclose(f);
  DkShaderMaker m;
  dkShaderMakerDefaults(&m, g_codeMem, g_codeOff);
  dkShaderInitialize(sh, &m);
  g_codeOff += aligned;
  const bool ok = dkShaderIsValid(sh);
  logf("[shader] %s %s (%u B, stage %d)", ok ? "ok  " : "FAIL", path, size,
       ok ? (int)dkShaderGetStage(sh) : -1);
  return ok;
}

void graphics_init() {
  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.cbDebug = dk_debug_cb;
  g_dev = dkDeviceCreate(&dm);
  logf("[init] device created");
  log_mem("after device");

  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, g_dev);
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = kFbW;
  lm.dimensions[1] = kFbH;
  DkImageLayout fbLayout;
  dkImageLayoutInitialize(&fbLayout, &lm);
  const uint32_t fbSize =
      align_up((uint32_t)dkImageLayoutGetSize(&fbLayout), dkImageLayoutGetAlignment(&fbLayout));

  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, g_dev, align_up(kFbNum * fbSize, DK_MEMBLOCK_ALIGNMENT));
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  g_fbMem = dkMemBlockCreate(&mm);
  DkImage const* swapImgs[kFbNum];
  for (unsigned i = 0; i < kFbNum; i++) {
    dkImageInitialize(&g_fb[i], &fbLayout, g_fbMem, i * fbSize);
    swapImgs[i] = &g_fb[i];
  }
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), swapImgs, kFbNum);
  g_swap = dkSwapchainCreate(&sm);
  // interval 0 = do not wait for vblank: the bench measures throughput, not pacing
  dkSwapchainSetSwapInterval(g_swap, 0);
  logf("[init] swapchain %ux%u x%u ok (fb %u KB each)", kFbW, kFbH, kFbNum, fbSize / 1024);

  dkMemBlockMakerDefaults(&mm, g_dev, kCodePool);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
  g_codeMem = dkMemBlockCreate(&mm);
  dkMemBlockMakerDefaults(&mm, g_dev, align_up(kDataPool, DK_MEMBLOCK_ALIGNMENT));
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  g_dataMem = dkMemBlockCreate(&mm);
  log_mem("after pools");

  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, g_dev);
  qm.flags = DkQueueFlags_Graphics;
  g_queue = dkQueueCreate(&qm);
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, g_dev);
  g_cmd = dkCmdBufCreate(&cm);
  memset(g_sliceFence, 0, sizeof(g_sliceFence));
  logf("[init] queue + cmdbuf ok");

  const DkGpuAddr base = dkMemBlockGetGpuAddr(g_dataMem);
  g_uboAddr = base + kOffUbo;
  g_imgDescAddr = base + kOffImgDesc;
  g_smpDescAddr = base + kOffSmpDesc;
}

bool shaders_init() {
  bool ok = load_shader(&g_vsh, "romfs:/shaders/bench_vsh.dksh");
  ok = load_shader(&g_fsh, "romfs:/shaders/bench_fsh.dksh") && ok;
  // P0.4: the offline-compiled real repo shader (UAM spike) must load too
  DkShader tv, tf;
  bool repo = load_shader(&tv, "romfs:/tfrag3.vert.dksh");
  repo = load_shader(&tf, "romfs:/tfrag3.frag.dksh") && repo;
  logf("[shader] repo tfrag3 .dksh load: %s", repo ? "PASS" : "FAIL");
  return ok;
}

void textures_init() {
  // two 64x64 RGBA checkerboards, uploaded through a staging buffer
  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, g_dev);
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = kTexDim;
  lm.dimensions[1] = kTexDim;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &lm);
  const uint32_t size =
      align_up((uint32_t)dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));

  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, g_dev, align_up(kNumTex * size, DK_MEMBLOCK_ALIGNMENT));
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  g_imgMem = dkMemBlockCreate(&mm);

  dkCmdBufClear(g_cmd);
  dkCmdBufAddMemory(g_cmd, g_dataMem, 0, kCmdSlice);

  const uint32_t texBytes = kTexDim * kTexDim * 4;
  DkImageDescriptor imgDesc[kNumTex];
  for (unsigned t = 0; t < kNumTex; t++) {
    dkImageInitialize(&g_tex[t], &layout, g_imgMem, t * size);
    uint32_t* px = (uint32_t*)data_cpu(kOffStaging + t * texBytes);
    for (unsigned y = 0; y < kTexDim; y++) {
      for (unsigned x = 0; x < kTexDim; x++) {
        const bool on = ((x / 8) ^ (y / 8)) & 1;
        px[y * kTexDim + x] = on ? (t ? 0xFF40B0FFu : 0xFFFFB040u) : 0xFFFFFFFFu;  // ABGR
      }
    }
    DkImageView view;
    dkImageViewDefaults(&view, &g_tex[t]);
    DkCopyBuf src = {dkMemBlockGetGpuAddr(g_dataMem) + kOffStaging + t * texBytes, 0, 0};
    DkImageRect rect = {0, 0, 0, kTexDim, kTexDim, 1};
    dkCmdBufCopyBufferToImage(g_cmd, &src, &view, &rect, 0);
    dkImageDescriptorInitialize(&imgDesc[t], &view, false, false);
  }
  DkSampler smp;
  dkSamplerDefaults(&smp);
  smp.minFilter = DkFilter_Linear;
  smp.magFilter = DkFilter_Linear;
  DkSamplerDescriptor smpDesc;
  dkSamplerDescriptorInitialize(&smpDesc, &smp);

  dkCmdBufPushData(g_cmd, g_imgDescAddr, imgDesc, sizeof(imgDesc));
  dkCmdBufPushData(g_cmd, g_smpDescAddr, &smpDesc, sizeof(smpDesc));
  dkCmdBufBarrier(g_cmd, DkBarrier_Full, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Image);
  dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(g_cmd));
  dkQueueWaitIdle(g_queue);
  dkCmdBufClear(g_cmd);
  logf("[init] %u textures %ux%u uploaded + descriptors written", kNumTex, kTexDim, kTexDim);
  log_mem("after textures");
}

struct FrameTimes {
  double wait_ms, record_ms, submit_ms;
};

// Record + submit + present one frame of `draws` draws. Returns CPU timings.
FrameTimes render_frame(unsigned frame, unsigned draws) {
  FrameTimes ft{};
  const unsigned slice = frame % kFbNum;

  double t = now_ms();
  // this slice's previous commands must have finished before its memory is reused
  dkFenceWait(&g_sliceFence[slice], -1);
  const int img = dkQueueAcquireImage(g_queue, g_swap);
  ft.wait_ms = now_ms() - t;

  t = now_ms();
  dkCmdBufClear(g_cmd);
  dkCmdBufAddMemory(g_cmd, g_dataMem, slice * kCmdSlice, kCmdSlice);

  DkImageView fbView;
  dkImageViewDefaults(&fbView, &g_fb[img]);
  dkCmdBufBindRenderTarget(g_cmd, &fbView, nullptr);
  DkViewport vp = {0.0f, 0.0f, (float)kFbW, (float)kFbH, 0.0f, 1.0f};
  DkScissor sc = {0, 0, kFbW, kFbH};
  dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
  dkCmdBufSetScissors(g_cmd, 0, &sc, 1);
  dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 0.05f, 0.07f, 0.12f, 1.0f);

  DkShader const* shaders[] = {&g_vsh, &g_fsh};
  dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, shaders, 2);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  dkCmdBufBindRasterizerState(g_cmd, &rs);
  dkCmdBufBindColorState(g_cmd, &cs);
  dkCmdBufBindColorWriteState(g_cmd, &cws);
  dkCmdBufBindImageDescriptorSet(g_cmd, g_imgDescAddr, kNumTex);
  dkCmdBufBindSamplerDescriptorSet(g_cmd, g_smpDescAddr, 1);
  dkCmdBufBindUniformBuffer(g_cmd, DkStage_Vertex, 0, g_uboAddr, sizeof(Params));
  dkCmdBufBindUniformBuffer(g_cmd, DkStage_Fragment, 0, g_uboAddr, sizeof(Params));

  // grid of small triangles; per draw: UBO update + texture rebind + draw
  const unsigned cols = 100;
  const unsigned rows = (draws + cols - 1) / cols;
  const float cw = 2.0f / cols, ch = 2.0f / (float)rows;
  const float phase = (float)(frame % 120) / 120.0f;
  for (unsigned i = 0; i < draws; i++) {
    Params p;
    p.offset_scale[0] = -1.0f + cw * ((float)(i % cols) + 0.5f);
    p.offset_scale[1] = -1.0f + ch * ((float)(i / cols) + 0.5f);
    p.offset_scale[2] = cw * 0.45f;
    p.offset_scale[3] = ch * 0.45f;
    const float k = (float)((i + frame) % 64) / 63.0f;
    p.color[0] = 0.5f + 0.5f * k;
    p.color[1] = 0.5f + 0.5f * phase;
    p.color[2] = 1.0f - 0.5f * k;
    p.color[3] = 1.0f;
    dkCmdBufPushConstants(g_cmd, g_uboAddr, sizeof(Params), 0, sizeof(Params), &p);
    dkCmdBufBindTexture(g_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(i & 1, 0));
    dkCmdBufDraw(g_cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  }
  dkCmdBufSignalFence(g_cmd, &g_sliceFence[slice], true);
  const DkCmdList list = dkCmdBufFinishList(g_cmd);
  ft.record_ms = now_ms() - t;

  t = now_ms();
  dkQueueSubmitCommands(g_queue, list);
  dkQueuePresentImage(g_queue, g_swap, img);
  ft.submit_ms = now_ms() - t;
  return ft;
}

int cmp_double(const void* a, const void* b) {
  const double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

bool keep_running(PadState* pad) {
  if (!appletMainLoop()) {
    return false;
  }
  padUpdate(pad);
  return !(padGetButtonsDown(pad) & HidNpadButton_Plus);
}

}  // namespace

int main(int, char**) {
  g_t0 = armGetSystemTick();
  mkdir("sdmc:/switch/dk_probe", 0777);
  g_log = fopen("sdmc:/switch/dk_probe/dk_probe_log.txt", "w");
  logf("[probe] dk_probe start (deko3d migration P0.4, AI-assisted) -- made by fildicio");
  log_clocks();
  log_mem("at start");

  const Result rc = romfsInit();
  logf("[init] romfsInit rc=0x%x", rc);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  graphics_init();
  bool quit = false;
  if (!shaders_init()) {
    logf("[probe] ABORT: bench shaders failed to load");
  } else {
    textures_init();
    unsigned frame = 0;
    static double rec[kFramesPerStep], wait[kFramesPerStep], sub[kFramesPerStep];
    for (unsigned step = 0; step < sizeof(kBenchDraws) / sizeof(kBenchDraws[0]) && !quit;
         step++) {
      const unsigned draws = kBenchDraws[step];
      if (draws * kBytesPerDrawBudget > kCmdSlice) {
        logf("[bench] skip %u draws: cmd slice too small", draws);
        continue;
      }
      unsigned n = 0;
      for (unsigned f = 0; f < kWarmup + kFramesPerStep; f++) {
        if (!keep_running(&pad)) {
          quit = true;
          break;
        }
        const FrameTimes ft = render_frame(frame++, draws);
        if (f >= kWarmup) {
          rec[n] = ft.record_ms;
          wait[n] = ft.wait_ms;
          sub[n] = ft.submit_ms;
          n++;
        }
      }
      if (n == 0) {
        break;
      }
      // CPU+GPU for one isolated frame at this load: drain, render, drain
      dkQueueWaitIdle(g_queue);
      const double t = now_ms();
      render_frame(frame++, draws);
      dkQueueWaitIdle(g_queue);
      const double iso = now_ms() - t;

      qsort(rec, n, sizeof(double), cmp_double);
      qsort(wait, n, sizeof(double), cmp_double);
      qsort(sub, n, sizeof(double), cmp_double);
      const double r50 = rec[n / 2];
      logf("[bench] draws=%5u n=%u | record p50 %.2f p95 %.2f ms = %.3f us/draw | submit+present "
           "p50 %.2f ms | wait p50 %.2f ms | isolated cpu+gpu frame %.2f ms",
           draws, n, r50, rec[(n * 95) / 100], r50 * 1000.0 / draws, sub[n / 2], wait[n / 2],
           iso);
    }
    log_mem("after bench");
    logf("[probe] bench done%s. GL reference ([glt] run 3): ~850 draws, 5-15k GL calls, "
         "11.6 ms/frame GL work",
         quit ? " (stopped early)" : "");
    for (unsigned f = 0; f < 120 && !quit; f++) {  // show the result ~2 s, then exit
      if (!keep_running(&pad)) {
        break;
      }
      render_frame(frame++, 1000);
    }
  }

  dkQueueWaitIdle(g_queue);
  dkQueueDestroy(g_queue);
  dkCmdBufDestroy(g_cmd);
  if (g_imgMem) {
    dkMemBlockDestroy(g_imgMem);
  }
  dkMemBlockDestroy(g_dataMem);
  dkMemBlockDestroy(g_codeMem);
  dkSwapchainDestroy(g_swap);
  dkMemBlockDestroy(g_fbMem);
  dkDeviceDestroy(g_dev);
  logf("[probe] clean exit");
  romfsExit();
  if (g_log) {
    fclose(g_log);
  }
  return 0;
}
