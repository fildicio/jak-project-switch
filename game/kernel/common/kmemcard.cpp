/*!
 * @file kmemcard.cpp
 * Memory card interface. Very messy code. Most of it is commented out now, as we've switched away
 * from memory cards to just raw saves.
 *
 * Not checked carefully for differences in jak 2.
 */

#include "kmemcard.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/FsLock.h"
#include "common/util/Timer.h"

#include "game/sce/sif_ee.h"
#include "game/sce/sif_ee_memcard.h"
#if defined(__SWITCH__)
#include "game/switch/run_log.h"  // FIX 7u: mirror [MC] to the live network log
#endif

#include "fmt/format.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__SWITCH__)
#include <fcntl.h>
#include <mutex>

// Shared, lazily-opened append-only trace sink. NOTE: this deliberately lives OUTSIDE
// the mc_print template -- function-local statics inside a template are duplicated per
// instantiation, which would give every mc_print<...> variant its own fd AND its own
// mutex (writes would not actually be serialized across instantiations).
static std::mutex g_mc_trace_mtx;
static int g_mc_trace_fd = -1;
static void mc_trace_persist(const std::string& line) {
  // write() on an sdmc: fd goes through the same (unlocked) devoptab/fsdev layer as
  // everything else, so this has to serialize against the overlord's ISO reads too.
  // Taken before g_mc_trace_mtx so the lock order is always fs -> trace-mutex.
  SWITCH_FS_LOCK();
  std::lock_guard<std::mutex> lock(g_mc_trace_mtx);
  if (g_mc_trace_fd < 0) {
    // Per-game install dir (SWITCH_GAME in the root CMakeLists); the jak1 fallback keeps
    // NROs built before per-game selection tracing to the same place they always did.
#ifdef SWITCH_GAME_NAME
    g_mc_trace_fd =
        open("sdmc:/switch/" SWITCH_GAME_NAME "/mc-trace.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
#else
    g_mc_trace_fd = open("sdmc:/switch/jak1/mc-trace.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
    if (g_mc_trace_fd < 0) {
      return;  // can't trace to disk; stdout + the lg log still got the line
    }
  }
  // NOTE: no fsync. write() hands the data to the console's FS service, which is a
  // separate system process, so the line already survives a crash of the game process.
  // fsync would only add protection against full power loss, at the cost of a FAT32
  // flush per log line -- which measurably stalls boot and saving.
  write(g_mc_trace_fd, line.data(), line.size());
}

// The memory card layer is an active debugging frontier of the Switch port -- log every
// step so save failures on-device can be diagnosed from the SD card without a debugger.
// Output goes to stdout (redirected to sdmc:/gk_stdout.txt, for live nxlink/gdb
// sessions), the rotating on-disk log (data/log/*.log) AND a dedicated append-only
// trace file. The extra file exists because both other sinks ate the evidence of the
// 2026-09-11 on-device save failure: gk_stdout.txt is truncated by every boot (it is
// freopen'd with "w" in main), and the rotating lg log is asynchronous, so its queue
// was dropped when the console crashed minutes later. mc-trace.txt is appended forever,
// so it survives both.
static constexpr bool memcard_debug = true;
#else
static constexpr bool memcard_debug = false;
#endif

using McCallbackFunc = void (*)(s32);

McCallbackFunc callback;

static s32 language;
static MemoryCardOperation op;
// instead of two memory cards we just simulate the 4 save files (8 banks).
static MemoryCardFile mc_files[4];
// keep track of latest file selected. this is only used in an auto-save mode thats not used
static int mc_last_file = -1;

// a random value we will use as the memory card "handle" for the pc port, which has no memcards.
constexpr u32 PC_MEM_CARD_HANDLE = 0x6C616F67;

constexpr u32 MEM_CARD_MAGIC = 0x12345678;

struct McHeader {
  u32 save_count;
  u32 checksum;
  u32 magic;
  u8 preview_data[64];
  u8 data[944];
  u32 save_count2;
};
static_assert(sizeof(McHeader) == 0x400, "McHeader size");

static McHeader header;

// these are the return value for sceMcGetInfo.
static s32 p1, p2, p3, p4;
using namespace ee;

template <typename... Args>
void mc_print(const std::string& str, Args&&... args) {
  if (memcard_debug) {
    // FIX 7t: a debug print must never be able to kill the game. On 2026-09-11 this
    // function did exactly that: fmt::print's fwrite to the (unbuffered, SD-backed) stdout
    // came up short during an fsdev race with the ISO thread, fmt threw system_error, and
    // the uncaught exception aborted the process ~14s into the intro. safe_stdout.h fixes
    // the cause; this catch-all makes the failure mode structurally impossible here
    // regardless of what any future sink does.
    // FIX 8b: the whole [MC] formatter is periodic diagnostics -- silenced by the
    // L3+R3+Minus toggle like the other heartbeat-class lines.
#if defined(__SWITCH__)
    if (!switch_diag_enabled()) {
      return;
    }
#endif
    try {
      std::string format_str = str;
      if (format_str.empty() || format_str.back() != '\n') {
        format_str += '\n';
      }
      const auto formatted =
          fmt::format(fmt::runtime(format_str), std::forward<Args>(args)...);
      // stdout for live debugging sessions -- flush every line, buffered output is lost
      // when the game is killed without a clean exit.
      fmt::print("[MC] {}", formatted);
      fflush(stdout);
      // the rotating on-disk log survives those kills, so mirror everything there too.
      lg::info("[MC] {}", formatted);
#if defined(__SWITCH__)
      // and the append-only trace file, which also survives the next boot truncating
      // gk_stdout.txt and any crash dropping the async lg queue (see comment above).
      mc_trace_persist("[MC] " + formatted);
      // FIX 7u: and the live network log, so save failures can be watched as they happen
      // on the development machine instead of requiring an SD card round trip. Strip the
      // trailing newline -- switch_run_logf adds its own.
      {
        std::string live = formatted;
        while (!live.empty() && (live.back() == '\n' || live.back() == '\r')) {
          live.pop_back();
        }
        switch_run_logf("[MC] %s", live.c_str());
      }
#endif
    } catch (...) {
      // Intentionally swallowed: losing a debug line is always preferable to losing the
      // process.
    }
  }
}

/*!
 * Write an entire buffer to a file in bounded chunks, verifying every byte is written.
 *
 * On the Switch a single large fwrite to the SD card has been observed to fail outright
 * (the 64 KiB save payload write fails while the 1 KiB header write on the same FILE
 * succeeds), which truncated bank files to just the header. Keeping each individual
 * write small avoids that, and on failure we log exactly where it stopped, with errno.
 */
static bool mc_write_all(FILE* fd, const void* data, size_t total_bytes) {
  constexpr size_t kMaxChunk = 8192;
  const u8* out = (const u8*)data;
  size_t done = 0;
#if defined(__SWITCH__)
  // FIX 7u -- THE SAVE BUG.
  //
  // Symptom: the 1KiB header wrote fine and then the very first 8KiB payload chunk failed
  // instantly with fwrite()==0, errno=EIO, on all three attempts. Deterministic, so not
  // the fsdev race it was assumed to be.
  //
  // Cause: the difference between the two writes is not size, it is *where the source
  // bytes live*. The header is an ordinary local; the payload is op.data_ptr.c(), i.e. a
  // pointer into GOAL's simulated EE memory. newlib copies a small write into stdio's own
  // buffer and flushes that (normal memory -- fine), but a write at or above the buffer
  // size is handed to the device callback straight from the caller's pointer. fsdev then
  // asks the FS system process to read directly out of the GOAL heap mapping, which is not
  // a valid IPC transfer source, and the service returns EIO.
  //
  // Fix: stage every chunk through an ordinary buffer so fsdev never sees a GOAL pointer.
  // One 8KiB memcpy per chunk is irrelevant next to an SD write.
  static u8 s_bounce[kMaxChunk];
#endif
  while (done < total_bytes) {
    const size_t chunk = std::min(kMaxChunk, total_bytes - done);
#if defined(__SWITCH__)
    memcpy(s_bounce, out + done, chunk);
    const size_t wrote = fwrite(s_bounce, 1, chunk, fd);
#else
    const size_t wrote = fwrite(out + done, 1, chunk, fd);
#endif
    if (wrote != chunk) {
      mc_print("write FAILED at offset {} of {} bytes (fwrite returned {}, errno - {}, "
               "ferror - {})",
               (int)done, (int)total_bytes, (int)wrote, errno, ferror(fd));
      return false;
    }
    done += chunk;
  }
  return true;
}

/*!
 * Read an entire buffer from a file in bounded chunks, verifying every byte is read.
 * See mc_write_all for why large single stdio operations are not trusted on the Switch.
 */
static bool mc_read_all(FILE* fd, void* data, size_t total_bytes) {
  constexpr size_t kMaxChunk = 8192;
  u8* in = (u8*)data;
  size_t done = 0;
#if defined(__SWITCH__)
  // Same reasoning as mc_write_all: a large fread lands directly in the caller's buffer,
  // so reading a save straight into GOAL memory hits the identical EIO. Stage it.
  static u8 s_bounce[kMaxChunk];
#endif
  while (done < total_bytes) {
    const size_t chunk = std::min(kMaxChunk, total_bytes - done);
#if defined(__SWITCH__)
    const size_t got = fread(s_bounce, 1, chunk, fd);
    if (got == chunk) {
      memcpy(in + done, s_bounce, chunk);
    }
#else
    const size_t got = fread(in + done, 1, chunk, fd);
#endif
    if (got != chunk) {
      mc_print("read FAILED at offset {} of {} bytes (fread returned {}, errno - {}, "
               "feof - {}, ferror - {})",
               (int)done, (int)total_bytes, (int)got, errno, feof(fd), ferror(fd));
      return false;
    }
    done += chunk;
  }
  return true;
}

/*!
 * Flush a file's data all the way to the storage device. fclose only empties the
 * userspace stdio buffer -- the on-device writeback may still be pending.
 */
static int mc_sync_file(FILE* fd) {
#ifdef _WIN32
  return _commit(fileno(fd));
#else
  return fsync(fileno(fd));
#endif
}

const char* filename_jak1[12] = {
    "BASCUS-97124AYBABTU!",           "BASCUS-97124AYBABTU!/icon.sys",
    "BASCUS-97124AYBABTU!/icon.ico",  "BASCUS-97124AYBABTU!/BASCUS-97124AYBABTU!",
    "BASCUS-97124AYBABTU!/bank0.bin", "BASCUS-97124AYBABTU!/bank1.bin",
    "BASCUS-97124AYBABTU!/bank2.bin", "BASCUS-97124AYBABTU!/bank3.bin",
    "BASCUS-97124AYBABTU!/bank4.bin", "BASCUS-97124AYBABTU!/bank5.bin",
    "BASCUS-97124AYBABTU!/bank6.bin", "BASCUS-97124AYBABTU!/bank7.bin"};

const char* filename_jak2[12] = {
    "BASCUS-97265AYBABTU!",           "BASCUS-97265AYBABTU!/icon.sys",
    "BASCUS-97265AYBABTU!/icon.ico",  "BASCUS-97265AYBABTU!/BASCUS-97265AYBABTU!",
    "BASCUS-97265AYBABTU!/bank0.bin", "BASCUS-97265AYBABTU!/bank1.bin",
    "BASCUS-97265AYBABTU!/bank2.bin", "BASCUS-97265AYBABTU!/bank3.bin",
    "BASCUS-97265AYBABTU!/bank4.bin", "BASCUS-97265AYBABTU!/bank5.bin",
    "BASCUS-97265AYBABTU!/bank6.bin", "BASCUS-97265AYBABTU!/bank7.bin"};

const char* filename_jak3[12] = {
    "BASCUS-97330AYBABTU!",           "BASCUS-97330AYBABTU!/icon.sys",
    "BASCUS-97330AYBABTU!/icon.ico",  "BASCUS-97330AYBABTU!/BASCUS-97330AYBABTU!",
    "BASCUS-97330AYBABTU!/bank0.bin", "BASCUS-97330AYBABTU!/bank1.bin",
    "BASCUS-97330AYBABTU!/bank2.bin", "BASCUS-97330AYBABTU!/bank3.bin",
    "BASCUS-97330AYBABTU!/bank4.bin", "BASCUS-97330AYBABTU!/bank5.bin",
    "BASCUS-97330AYBABTU!/bank6.bin", "BASCUS-97330AYBABTU!/bank7.bin"};

const char* mc_get_filename_no_dir(GameVersion version, int ndx) {
  const char** filenames = nullptr;
  switch (version) {
    case GameVersion::Jak1:
      filenames = filename_jak1;
      break;
    case GameVersion::Jak2:
      filenames = filename_jak2;
      break;
    case GameVersion::Jak3:
      filenames = filename_jak3;
      break;
  }
  return filenames[ndx];
}

inline fs::path mc_get_filename(GameVersion version, int ndx) {
  return file_util::get_user_memcard_dir(version) / mc_get_filename_no_dir(version, ndx);
}

int mc_get_total_bank_size(GameVersion) {
  return BANK_SIZE[g_game_version] + sizeof(McHeader) * 2;
}

void kmemcard_init_globals() {
  // next = 0;
  language = 0;
  op = {};
  // mc[0] = {};
  // mc[1] = {};
  mc_files[0] = {};
  mc_files[1] = {};
  mc_files[2] = {};
  mc_files[3] = {};
  callback = nullptr;
  p1 = 0;
  p2 = 0;
  p3 = 0;
  p4 = 0;
  // memset(&dirent, 0, sizeof(sceMcTblGetDir));
  memset(&header, 0, sizeof(McHeader));
}

/*!
 * FIX 34 (AI-assisted): raw-buffer variant of mc_checksum for the async
 * memory card worker, which checksums its own staging buffers instead of
 * GOAL memory (the worker must never touch the EE heap).
 */
static u32 mc_checksum_bytes(const u8* data_bytes, s32 size) {
  if (size < 0) {
    size += 3;
  }

  u32 result = 0;
  const u32* data_u32 = (const u32*)data_bytes;
  for (s32 i = 0; i < size / 4; i++) {
    result = result << 1 ^ (s32)result >> 0x1f ^ data_u32[i] ^ MEM_CARD_MAGIC;
  }

  return result ^ 0xedd1e666;
}

/*!
 * A questionable checksum used on memory card data.
 */
u32 mc_checksum(Ptr<u8> data, s32 size) {
  return mc_checksum_bytes(data.c(), size);
}

/*!
 * PC port function that returns whether a given bank ID's file exists or not.
 */
bool file_is_present(int id, int bank = 0) {
  // fs::exists / fs::file_size go straight through newlib's stat into the fsdev layer,
  // which is not thread-safe -- and this runs on the GOAL kernel thread while the
  // overlord streams DGO/STR data off the ISO. The 2026-09-11 "new game -> create save"
  // freeze hung exactly here: this unlocked stat raced an in-flight overlord read and
  // never returned, so the save dialog waited forever on mc-get-slot-info. Everything
  // routed through file_util::* was already serialized by SWITCH_FS_LOCK(); these direct
  // fs:: calls were the remaining hole on that path.
  SWITCH_FS_LOCK();
  auto bankname = mc_get_filename(g_game_version, 4 + id * 2 + bank);
  if (!fs::exists(bankname) ||
      int(fs::file_size(bankname)) < mc_get_total_bank_size(g_game_version)) {
    // file doesn't exist, or size is bad. we do not want to open files that will crash on read!
    return false;
  }
  // avoid file check here tbh. there shouldn't be any saves with a save count of zero anyway.
  // the file check is quite slow and ultimately not very useful.
  return true;

  /*
  // file exists. but let's see if it's an empty one.
  // this prevents the game from reading a bank but classifying it as corrupt data.
  // which a file full of zeros logically is.
  auto fp = file_util::open_file(bankname.c_str(), "rb");

  // we can actually just check if the save count is over zero...
  u32 savecount = 0;
  fread(&savecount, sizeof(u32), 1, fp);
  fclose(fp);
  return savecount > 0;
  */
}

/*!
 * FIX 7w -- frame-rate cleanup.
 *
 * pc_update_card() and the get-status handler are polled by GOAL *every frame*, so their
 * entry/exit traces were ~120 log lines a second, each one an fsync under the global
 * filesystem lock. That is the single largest self-inflicted cost in the port.
 *
 * The traces themselves are still load-bearing when the save path misbehaves (a
 * "begin" with no "done" localises a hang inside the card scan), so they are kept and
 * merely gated. Flip this to true to get them back.
 */
static constexpr bool kMcTracePolling = false;

#define mc_print_poll(...)   \
  do {                       \
    if (kMcTracePolling) {    \
      mc_print(__VA_ARGS__); \
    }                        \
  } while (0)

/*!
 * PC port function to set memcard info. We don't use a memory card, instead just the raw savefiles.
 */
/*!
 * FIX 7x -- THE REAL FRAME-RATE BOTTLENECK.
 *
 * pc_update_card() is polled by GOAL every frame. For every *occupied* save slot it called
 * file_util::read_binary_file() on the whole bank file -- ~66KB -- and up to twice per slot,
 * i.e. as much as half a megabyte of synchronous SD reads per frame, all taken while holding
 * the global SWITCH_FS_LOCK() that the ISO/streaming threads also need.
 *
 * This is why the frame rate got *worse* after saving started working: the expensive branch
 * is guarded by mc_files[file].present, so with no save files on the card it never ran. The
 * moment the player has saves, every frame starts re-reading them.
 *
 * Only two things are actually wanted out of that 66KB: save_count and the 64-byte preview.
 * And they can only change when the file changes, which is rare (a save). So cache them,
 * keyed on the file's size and modification time, and re-read only when that key moves. The
 * per-frame cost drops from ~0.5MB of reads to a couple of stat() calls.
 */
struct McHeaderCache {
  bool valid = false;
  uintmax_t size = 0;
  s64 mtime = 0;
  u32 save_count = 0;
  u8 preview[64] = {};
};

static McHeaderCache g_mc_header_cache[8];  // 4 slots x 2 banks

/*!
 * Return the cached header fields for a bank, re-reading the file only if it changed.
 * Returns nullptr if the file could not be read.
 */
static const McHeaderCache* mc_get_header_cached(int bank_idx, const fs::path& path) {
  SWITCH_FS_LOCK();
  auto& c = g_mc_header_cache[bank_idx];
  std::error_code ec;
  const auto sz = fs::file_size(path, ec);
  if (ec) {
    c.valid = false;
    return nullptr;
  }
  const auto mt = fs::last_write_time(path, ec);
  if (ec) {
    c.valid = false;
    return nullptr;
  }
  const s64 mts = (s64)mt.time_since_epoch().count();
  if (c.valid && c.size == sz && c.mtime == mts) {
    return &c;  // unchanged -- no SD read at all
  }
  const auto bankdata = file_util::read_binary_file(path.string());
  if (bankdata.size() < sizeof(McHeader)) {
    c.valid = false;
    return nullptr;
  }
  const auto* h = reinterpret_cast<const McHeader*>(bankdata.data());
  c.save_count = h->save_count;
  memcpy(c.preview, h->preview_data, 64);
  c.size = sz;
  c.mtime = mts;
  c.valid = true;
  mc_print("header cache refill bank={} save_count={}", bank_idx, (int)c.save_count);
  return &c;
}

void pc_update_card() {
  SWITCH_FS_LOCK();
  mc_print_poll("update-card: begin");
  // int highest_save_count = 0;
  mc_last_file = -1;
  for (s32 file = 0; file < 4; file++) {
    auto bankname = mc_get_filename(g_game_version, 4 + file * 2);
    mc_files[file].present = file_is_present(file);
    if (mc_files[file].present) {
      const auto* h1 = mc_get_header_cached(file * 2, bankname);
      if (!h1) {
        mc_files[file].present = 0;
        continue;
      }
      const McHeaderCache* chosen = h1;
      bool used_second = false;
      if (file_is_present(file, 1)) {
        auto bankname2 = mc_get_filename(g_game_version, 1 + 4 + file * 2);
        const auto* h2 = mc_get_header_cached(file * 2 + 1, bankname2);
        if (h2 && h2->save_count > h1->save_count) {
          // use most recent bank here.
          chosen = h2;
          used_second = true;
        }
      }

      // banks chosen and checked. copy data and set info.
      mc_files[file].last_saved_bank = used_second;
      mc_files[file].most_recent_save_count = chosen->save_count;

      memcpy(mc_files[file].data, chosen->preview, 64);

      // if (mc_files[file].most_recent_save_count > highest_save_count) {
      //  mc_last_file = file;
      //  highest_save_count = mc_files[file].most_recent_save_count;
      // }
    }
  }
  mc_print_poll("update-card: done");
}

// ---------------------------------------------------------------------------
// FIX 35 (AI-assisted): frame-sliced memory card SAVE, without a thread.
//
// History: the original port ran the entire SD-card transaction (open ->
// header -> 128 KiB payload -> footer -> fsync -> close, measured 125-276 ms on
// the console) inline on the GOAL kernel thread, from MC_run(). Jak 2
// auto-saves on every tutorial hint completion, so hint text sequences froze
// the whole game 4-9 frames at a time -- the "dramatic slowdown while tutorial
// text is on screen". FIX 34 tried a worker thread; FIX 34b made it
// exception-proof; FIX 34c proved the console will not give us a runtime
// thread at all (std::thread -> _exit(1); the process has ~4 MB free of its
// 3.2 GB reservation, so there is no room for a stack). FIX 35 removes the
// freeze without any concurrency:
//
//   1. redundant saves are skipped outright (FIX 35a): Jak 2 auto-saves on
//      every tutorial hint with near-identical payloads. The bank is
//      checksummed and if the card already holds those exact bytes, success is
//      reported without touching the SD card (the mc_files bookkeeping still
//      flips banks / bumps save counts exactly like a real save);
//   2. real saves become a state machine driven by MC_run() (FIX 35b), one
//      cheap step per frame: OPEN -> WRITE_HEADER -> WRITE_PAYLOAD in
//      MC_SAVE_SLICE_BYTES slices -> WRITE_FOOTER -> FSYNC -> CLOSE -> APPLY.
//      While it runs op.result stays BUSY, which the GOAL save/load logic
//      already tolerates for seconds -- that is exactly how a real PS2 memory
//      card behaves. A 128 KiB save becomes ~8 frames of 1-2 ms instead of one
//      270 ms freeze.
//
// Every filesystem operation still happens under SWITCH_FS_LOCK() (FIX 7u),
// now scoped to a single step instead of the whole transaction, so the
// overlord's ISO streaming never waits more than one slice. The 3-attempt /
// 100 ms retry is preserved (FIX 33-era transient fsdev failures): a failed
// attempt closes the FILE*, resets to OPEN and waits out the backoff by
// staying BUSY for a few frames -- sleeping would reintroduce the freeze.
//
// The machine runs identically on desktop builds, so the exact save path can
// be validated on the Mac host before any console test (FIX 35 brief, Task 3).
// Loads stay one-shot synchronous calls (they are rare: boot / save-select);
// their bank verification logic in mc_worker_load() is untouched.
// ---------------------------------------------------------------------------

struct McSaveRequest {
  u32 file_idx = 0;
  u32 save_count = 0;          // header save count (was p2)
  u32 bank = 0;                // which bank file to write (was p4)
  u32 checksum = 0;            // FIX 35: checksum of bank_data, for redundant-save skipping
  std::vector<u8> bank_data;   // BANK_SIZE bytes, snapshotted from GOAL memory
  std::vector<u8> preview;     // 64-byte summary, snapshotted from GOAL memory
};

struct McLoadRequest {
  u32 file_idx = 0;
};

struct McAsyncResult {
  McStatusCode status = McStatusCode::OK;
  u32 save_count = 0;          // save: as dispatched / load: of the chosen bank
  u32 bank = 0;
  std::vector<u8> loaded_bank;  // load OK: BANK_SIZE bytes destined for op.data_ptr
  std::vector<u8> preview;      // save OK: 64 bytes for mc_files
};

/*!
 * Worker-side load (FIX 34). Reads both bank files of a save slot into a
 * staging buffer, verifies headers/footers/checksums, and picks the freshest
 * intact bank. Same logic as the old synchronous loader (pc_game_load_open_
 * file); the difference is that the data lands in the result instead of
 * directly in GOAL memory.
 */
static McAsyncResult mc_worker_load(const McLoadRequest& req) {
  McAsyncResult res;
  const size_t read_size = mc_get_total_bank_size(g_game_version);
  // Both banks are staged here (bank 1 stays zero if absent); GOAL memory is
  // only touched by the GOAL thread when the result is applied in MC_run().
  std::vector<u8> staging(2 * read_size, 0);

  auto bank_path = [&](int bank) {
    return mc_get_filename(g_game_version, req.file_idx * 2 + 4 + bank);
  };

  // same fsdev thread-safety story as the save path: an IO failure surfaces as
  // INTERNAL_ERROR and is transient, so retry from a fresh FILE*. Results that
  // come from actually inspecting the loaded data (READ_ERROR, NEW_GAME, ...)
  // are real and are not retried.
  bool io_ok = false;
  bool have_second = false;
  constexpr int kMaxLoadAttempts = 3;
  for (int attempt = 1; attempt <= kMaxLoadAttempts && !io_ok; attempt++) {
    if (attempt > 1) {
      mc_print("load attempt {} failed - retrying", attempt - 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Covers the freads and the mid-function fs::exists (aux-bank check).
    SWITCH_FS_LOCK();
    mc_print("opening save file {}",
             mc_get_filename_no_dir(g_game_version, req.file_idx * 2 + 4));
    auto fd = file_util::open_file(bank_path(0).string().c_str(), "rb");
    if (!fd) {
      continue;
    }
    mc_print("reading save file ({} bytes)...", (int)read_size);
    bool bank0_ok = mc_read_all(fd, staging.data(), read_size);
    fclose(fd);
    if (!bank0_ok) {
      continue;
    }
    // added : check if aux bank exists
    if (fs::exists(bank_path(1))) {
      mc_print("reading next save bank {}",
               mc_get_filename_no_dir(g_game_version, req.file_idx * 2 + 5));
      auto fd2 = file_util::open_file(bank_path(1).string().c_str(), "rb");
      if (!fd2) {
        continue;
      }
      have_second = mc_read_all(fd2, staging.data() + read_size, read_size);
      fclose(fd2);
      if (!have_second) {
        continue;
      }
    }
    io_ok = true;
  }

  if (!io_ok) {
    res.status = McStatusCode::INTERNAL_ERROR;
    return res;
  }

  // let's verify the data.
  const McHeader* headers[2];
  const McHeader* footers[2];
  bool ok[2];

  headers[0] = (const McHeader*)(staging.data());
  footers[0] = (const McHeader*)(staging.data() + sizeof(McHeader) + BANK_SIZE[g_game_version]);
  headers[1] = (const McHeader*)(staging.data() + read_size);
  footers[1] = (const McHeader*)(staging.data() + read_size + sizeof(McHeader) +
                                 BANK_SIZE[g_game_version]);
  ok[0] = true;
  ok[1] = have_second;

  for (int idx = 0; idx < 2; idx++) {
    u32 expected_save_count = headers[idx]->save_count;
    if (headers[idx]->save_count2 == expected_save_count &&
        footers[idx]->save_count == expected_save_count &&
        footers[idx]->save_count2 == expected_save_count) {
      // save count is okay!
      if (headers[idx]->magic == MEM_CARD_MAGIC && footers[idx]->magic == MEM_CARD_MAGIC) {
        // magic numbers okay!
        if (headers[idx]->checksum == footers[idx]->checksum) {
          // checksum
          auto expected_checksum = headers[idx]->checksum;
          if (mc_checksum_bytes((const u8*)(headers[idx] + 1), BANK_SIZE[g_game_version]) !=
              expected_checksum) {
            mc_print("failed checksum");
            ok[idx] = false;
          }
        } else {
          mc_print("corrupted checksum");
          ok[idx] = false;
        }
      } else {
        mc_print("bad magic");
        ok[idx] = false;
      }
    } else {
      mc_print("bad save count");
      ok[idx] = false;
    }
  }

  mc_print("checking loaded banks");

  //
  if (!ok[0] && !ok[1]) {
    // no good data.
    if (headers[0]->save_count == 0 && headers[0]->checksum == 0 &&
        headers[0]->magic == 0 && headers[0]->save_count2 == 0 &&
        headers[1]->save_count == 0 && headers[1]->checksum == 0 &&
        headers[1]->magic == 0 && headers[1]->save_count2 == 0) {
      // this is a fresh file that you tried to load from...
      mc_print("new game result");
      res.status = McStatusCode::NEW_GAME;
    } else {
      mc_print("corrupted data");
      res.status = McStatusCode::READ_ERROR;
    }
    return res;
  }

  // pick the bank
  int bank = 0;

  if (!ok[0] || !ok[1]) {
    if (ok[1]) {
      bank = 1;
    }
  } else {
    bank = headers[0]->save_count <= headers[1]->save_count;
  }

  mc_print(fmt::format("loading bank {}", bank));
  res.status = McStatusCode::OK;
  res.save_count = headers[bank]->save_count;
  res.bank = bank;
  res.loaded_bank.assign(staging.data() + bank * read_size + sizeof(McHeader),
                         staging.data() + bank * read_size + sizeof(McHeader) +
                             BANK_SIZE[g_game_version]);
  mc_print("load succeeded");
  return res;
}

// ---------------------------------------------------------------------------
// FIX 35 (AI-assisted): the frame-sliced save state machine. See the header
// comment above McSaveRequest. One struct, driven one step per MC_run() call
// from the GOAL thread; no thread is ever created (FIX 34b/34c proved the
// console kills the process on std::thread construction - ~4 MB free of the
// 3.2 GB reservation leaves no room for a stack - and a detached worker could
// never be validated there anyway).
// ---------------------------------------------------------------------------
// defined below; the sliced machine applies its result through it
static void mc_apply_async_result(const McAsyncResult& res, bool was_save);

namespace {
// bytes written per MC_run() step. 16 KiB keeps a step at ~1-2 ms on the
// Switch SD stack and finishes a 128 KiB jak2 bank in 8 frames. Named so it
// can be tuned (FIX 35 brief, Task 3b).
constexpr size_t MC_SAVE_SLICE_BYTES = 16 * 1024;
constexpr int MC_SAVE_MAX_ATTEMPTS = 3;

enum class McSaveStep { STEP_OPEN, STEP_WRITE_HEADER, STEP_WRITE_PAYLOAD, STEP_WRITE_FOOTER, STEP_FSYNC, STEP_CLOSE };

struct McSlicedSave {
  bool active = false;
  McSaveStep step = McSaveStep::STEP_OPEN;
  McSaveRequest req;
  McHeader hd;
  FILE* fd = nullptr;
  size_t written = 0;
  int attempt = 1;
  std::chrono::steady_clock::time_point retry_after{};
};
McSlicedSave g_mc_save;

// checksum of the last payload known to be on the card, per save slot (FIX 35a)
u32 g_mc_saved_checksum[4] = {};
bool g_mc_saved_checksum_valid[4] = {};

const char* mc_save_step_name(McSaveStep step) {
  switch (step) {
    case McSaveStep::STEP_OPEN:
      return "open";
    case McSaveStep::STEP_WRITE_HEADER:
      return "header";
    case McSaveStep::STEP_WRITE_PAYLOAD:
      return "payload";
    case McSaveStep::STEP_WRITE_FOOTER:
      return "footer";
    case McSaveStep::STEP_FSYNC:
      return "fsync";
    case McSaveStep::STEP_CLOSE:
      return "close";
  }
  return "?";
}

/*!
 * Park the final result in GOAL-visible state (op.result / mc_files) and
 * deactivate the machine. Called from the CLOSE step on success and from the
 * retry helper after all attempts failed.
 */
void mc_sliced_save_apply(McStatusCode status) {
  const McSaveRequest& req = g_mc_save.req;
  if (status == McStatusCode::OK) {
    g_mc_saved_checksum[req.file_idx] = req.checksum;
    g_mc_saved_checksum_valid[req.file_idx] = true;
  }
  McAsyncResult res;
  res.status = status;
  res.save_count = req.save_count;
  res.bank = req.bank;
  res.preview = req.preview;
  g_mc_save.active = false;
  mc_apply_async_result(res, true);
}

/*!
 * A step failed: close the FILE*, burn an attempt and restart from OPEN, or
 * give up after MC_SAVE_MAX_ATTEMPTS. Must run with no FS lock held.
 */
void mc_sliced_save_retry(const char* what) {
  if (g_mc_save.fd) {
    SWITCH_FS_LOCK();
    fclose(g_mc_save.fd);
    g_mc_save.fd = nullptr;
  }
  if (g_mc_save.attempt >= MC_SAVE_MAX_ATTEMPTS) {
    mc_print("sliced save: {} failed on attempt {} - giving up", what, g_mc_save.attempt);
    mc_sliced_save_apply(McStatusCode::INTERNAL_ERROR);
    return;
  }
  mc_print("sliced save: {} failed on attempt {} - retrying from open", what,
           g_mc_save.attempt);
  g_mc_save.attempt++;
  g_mc_save.step = McSaveStep::STEP_OPEN;
  g_mc_save.written = 0;
  g_mc_save.retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
}

void mc_sliced_save_start(McSaveRequest&& req) {
  g_mc_save = McSlicedSave{};
  g_mc_save.req = std::move(req);
  // header and footer are identical McHeaders; the checksum covers the payload
  // only (same layout the synchronous save always wrote).
  memset(&g_mc_save.hd, 0, sizeof(McHeader));
  g_mc_save.hd.save_count = g_mc_save.req.save_count;
  g_mc_save.hd.checksum = g_mc_save.req.checksum;
  g_mc_save.hd.magic = MEM_CARD_MAGIC;
  g_mc_save.hd.save_count2 = g_mc_save.req.save_count;
  memcpy(g_mc_save.hd.preview_data, g_mc_save.req.preview.data(), 64);
  g_mc_save.step = McSaveStep::STEP_OPEN;
  g_mc_save.attempt = 1;
  g_mc_save.active = true;
  mc_print("sliced save started: bank {} save count {} ({} bytes in {} KiB slices)",
           (int)g_mc_save.req.bank, (int)g_mc_save.req.save_count,
           (int)BANK_SIZE[g_game_version], (int)(MC_SAVE_SLICE_BYTES / 1024));
}

/*!
 * Advance the frame-sliced save by ONE step. Called once per MC_run() (i.e.
 * once per frame) while the machine is active; between steps op.result stays
 * BUSY, exactly like a real (slow) PS2 memory card transaction.
 *
 * Every filesystem call happens under SWITCH_FS_LOCK() (FIX 7u) scoped to this
 * single step, so the overlord's ISO streaming is never blocked for more than
 * one slice. The lock is recursive, so the mc_print() calls -- which take it
 * themselves -- are fine.
 */
void mc_sliced_save_step() {
  if (!g_mc_save.active) {
    return;
  }
  // retry backoff without sleeping: the old worker thread could afford a
  // 100 ms sleep; on the GOAL thread that would reintroduce the freeze this
  // machine exists to remove, so just stay BUSY for the backoff window.
  if (g_mc_save.attempt > 1 && g_mc_save.step == McSaveStep::STEP_OPEN &&
      std::chrono::steady_clock::now() < g_mc_save.retry_after) {
    return;
  }

  Timer step_timer;
  const size_t bank_size = BANK_SIZE[g_game_version];
  const McSaveStep step = g_mc_save.step;

  switch (step) {
    case McSaveStep::STEP_OPEN: {
      mc_print("open {} for saving",
               mc_get_filename_no_dir(g_game_version,
                                      g_mc_save.req.file_idx * 2 + 4 + g_mc_save.req.bank));
      auto save_path =
          mc_get_filename(g_game_version, g_mc_save.req.file_idx * 2 + 4 + g_mc_save.req.bank);
      file_util::create_dir_if_needed_for_file(save_path.string());
      SWITCH_FS_LOCK();
      g_mc_save.fd = file_util::open_file(save_path.string().c_str(), "wb");
      if (!g_mc_save.fd) {
        mc_print("Error opening file for saving, errno - {}", errno);
        mc_sliced_save_retry("open");
        return;
      }
      g_mc_save.step = McSaveStep::STEP_WRITE_HEADER;
      break;
    }
    case McSaveStep::STEP_WRITE_HEADER: {
      SWITCH_FS_LOCK();
      if (!mc_write_all(g_mc_save.fd, &g_mc_save.hd, sizeof(McHeader))) {
        // cb_savedheader //
        mc_sliced_save_retry("header write");
        return;
      }
      g_mc_save.step = McSaveStep::STEP_WRITE_PAYLOAD;
      g_mc_save.written = 0;
      break;
    }
    case McSaveStep::STEP_WRITE_PAYLOAD: {
      const size_t left = bank_size - g_mc_save.written;
      const size_t chunk = std::min(left, MC_SAVE_SLICE_BYTES);
      SWITCH_FS_LOCK();
      if (!mc_write_all(g_mc_save.fd, g_mc_save.req.bank_data.data() + g_mc_save.written,
                        chunk)) {
        // cb_saveddata //
        mc_sliced_save_retry("payload write");
        return;
      }
      g_mc_save.written += chunk;
      if (g_mc_save.written >= bank_size) {
        g_mc_save.step = McSaveStep::STEP_WRITE_FOOTER;
      }
      break;
    }
    case McSaveStep::STEP_WRITE_FOOTER: {
      SWITCH_FS_LOCK();
      if (!mc_write_all(g_mc_save.fd, &g_mc_save.hd, sizeof(McHeader))) {
        // cb_savedfooter //
        mc_sliced_save_retry("footer write");
        return;
      }
      g_mc_save.step = McSaveStep::STEP_FSYNC;
      break;
    }
    case McSaveStep::STEP_FSYNC: {
      SWITCH_FS_LOCK();
      // make sure everything actually leaves the userspace stdio buffer and
      // reaches the card before we report success.
      fflush(g_mc_save.fd);
      if (mc_sync_file(g_mc_save.fd) != 0) {
        mc_print("WARNING: fsync of save file failed, errno - {}", errno);
        // the old synchronous save only warned here too; fclose decides success
      }
      g_mc_save.step = McSaveStep::STEP_CLOSE;
      break;
    }
    case McSaveStep::STEP_CLOSE: {
      SWITCH_FS_LOCK();
      const bool closed = fclose(g_mc_save.fd) == 0;
      g_mc_save.fd = nullptr;
      if (!closed) {
        mc_print("fclose of save file failed, errno - {}", errno);
        mc_sliced_save_retry("close");
        return;
      }
      // cb_closedsave //
      mc_print("sliced save complete after {} attempt(s)", g_mc_save.attempt);
      mc_sliced_save_apply(McStatusCode::OK);
      return;  // apply() already resolved the machine
    }
  }
  mc_print("sliced save step {} took {:.2f}ms (payload {}%)", mc_save_step_name(step),
           step_timer.getMs(), (int)(100 * g_mc_save.written / bank_size));
}
}  // namespace

/*!
 * Apply a finished save/load to GOAL-visible state. GOAL thread only (called
 * from MC_run() when a sliced save finishes or fails, right after a one-shot
 * load, and immediately for skipped redundant saves).
 */
static void mc_apply_async_result(const McAsyncResult& res, bool was_save) {
  op.operation = MemoryCardOperationKind::NO_OP;
  op.result = res.status;
  if (res.status == McStatusCode::OK) {
    if (was_save) {
      mc_files[op.param2].present = 1;
      mc_files[op.param2].most_recent_save_count = res.save_count;
      mc_files[op.param2].last_saved_bank = res.bank;
      memcpy(mc_files[op.param2].data, res.preview.data(), 64);
    } else {
      // the only GOAL-memory write on this path
      memcpy(op.data_ptr.c(), res.loaded_bank.data(), BANK_SIZE[g_game_version]);
      mc_files[op.param2].most_recent_save_count = res.save_count;
      mc_files[op.param2].last_saved_bank = res.bank;
    }
    mc_last_file = op.param2;
  } else if (!was_save && res.status == McStatusCode::NEW_GAME) {
    // the old synchronous loader also latched the file on a fresh-file result
    mc_last_file = op.param2;
  }
}

/*!
 * FIX 35 (AI-assisted): snapshot the save data out of GOAL memory and either
 * skip the write entirely (redundant payload, FIX 35a) or start the
 * frame-sliced state machine (FIX 35b). Cheap by design (a checksum + two
 * memcpys); everything that can touch the SD card happens one small step per
 * MC_run() call afterwards.
 */
static void mc_dispatch_save_async() {
  u32 save_count = 0;
  u32 bank = 0;
  // cd_reprobe_save // - mc_files is kept fresh by the per-frame MC_get_status
  // polling, so no card scan is needed here (the old synchronous save called
  // pc_update_card(), which would be another FS-locked walk on this thread).
  if (!file_is_present(op.param2)) {
    mc_print("reprobe save: first time!");
    // first time saving!
    save_count = 0;  // save count 0
    bank = 0;        // first bank for file
  } else {
    save_count = mc_files[op.param2].most_recent_save_count + 1;  // increment save count
    bank = mc_files[op.param2].last_saved_bank ^ 1;               // use the other bank
  }

  // reserve 0 as "I never saved" and use 1 instead.
  if (save_count == 0) {
    save_count = 1;
  }

  // FIX 35a (AI-assisted): skip redundant saves. Jak 2 auto-saves on every
  // tutorial hint with near-identical payloads; if the card already holds
  // these exact bytes (checksum of the last payload that made it to disk
  // matches), report success without touching the SD card at all. The
  // mc_files bookkeeping below still flips banks / bumps the save count
  // exactly like a real save, so GOAL sees a normal, successful transaction.
  const u32 payload_checksum = mc_checksum_bytes(op.data_ptr.c(), BANK_SIZE[g_game_version]);
  if (g_mc_saved_checksum_valid[op.param2] &&
      g_mc_saved_checksum[op.param2] == payload_checksum) {
    mc_print("save skipped (unchanged)");
    McAsyncResult res;
    res.status = McStatusCode::OK;
    res.save_count = save_count;
    res.bank = bank;
    res.preview.assign(op.data_ptr2.c(), op.data_ptr2.c() + 64);
    mc_apply_async_result(res, true);
    return;
  }

  // FIX 35b: snapshot the payload (the only GOAL-memory reads on this path)
  // and hand it to the frame-sliced writer, which MC_run() advances one step
  // per frame while op.result stays BUSY.
  McSaveRequest req;
  req.file_idx = op.param2;
  req.save_count = save_count;
  req.bank = bank;
  req.checksum = payload_checksum;
  req.bank_data.assign(op.data_ptr.c(), op.data_ptr.c() + BANK_SIZE[g_game_version]);
  req.preview.assign(op.data_ptr2.c(), op.data_ptr2.c() + 64);
  mc_sliced_save_start(std::move(req));
}

static void mc_dispatch_load_async() {
  // FIX 35: loads stay one-shot and synchronous (they are rare: boot /
  // save-select), and the bank verification logic in mc_worker_load() is
  // untouched. No worker thread exists anymore (FIX 34c) - this runs inline on
  // the GOAL thread exactly like it always did.
  McLoadRequest req;
  req.file_idx = op.param2;
  Timer sync_timer;
  McAsyncResult res = mc_worker_load(req);
  mc_print("synchronous load took {:.2f}ms", sync_timer.getMs());
  mc_apply_async_result(res, false);
}

/*!
 * Run the Memory Card state machine.  This is called once per frame in GOAL.
 * It:
 *  - does nothing if there is an in-progress memory card operation
 *  - if async memory card functions are done, runs their callbacks
 *  - if there is a requested operation, starts running sony functions.
 *  - if there is none of the above, and unknown cards, finds out about them.
 *  - every now and then, recheck cards.
 */
void MC_run() {
  // if we have an in-progress operation, it will have set a callback.
  if (callback) {
    s32 sony_cmd, sony_status;
    // check the status
    s32 status = sceMcSync(1, &sony_cmd, &sony_status);
    McCallbackFunc callback_for_sync = callback;
    if (status == sceMcExecRun) {
      // busy, return.
      return;
    }

    if (status == sceMcExecFinish) {
      // sony function is done. do the callback.
      callback = nullptr;
      (*callback_for_sync)(sony_status);
    } else {
      // sony function is done, but failed.
      callback = nullptr;
      (*callback_for_sync)(0);
    }

    if (callback) {
      // if we got another callback, it means there's another op started by the prev callback.
      // and this case, we want to wait for that operation to finish.
      return;
    }
  }

  // FIX 35: advance the frame-sliced save by one step. While it runs, keep
  // GOAL waiting (op.result stays BUSY, exactly like a real multi-second PS2
  // memcard op); the final step applies the result to GOAL memory and the
  // slot cache itself. This thread is the only one that ever touches
  // op/mc_files, so no locking is needed.
  if (g_mc_save.active) {
    mc_sliced_save_step();
    return;
  }

  // if we got here, there is no in-progress sony function. So start the next one, if we should
  if (op.operation == MemoryCardOperationKind::FORMAT) {
    // format memory card. Not used in PC port, so lets move on.
    return;
  } else if (op.operation == MemoryCardOperationKind::UNFORMAT) {
    // unformat memory card.
    return;
  } else if (op.operation == MemoryCardOperationKind::CREATE_FILE) {
    // create the game file.
    // there's no cards, keep in mind.
    return;
  } else if (op.operation == MemoryCardOperationKind::SAVE) {
    // write game save - frame-sliced by MC_run() (FIX 35).
    mc_dispatch_save_async();
  } else if (op.operation == MemoryCardOperationKind::LOAD) {
    // load game save - one-shot synchronous (FIX 35).
    if (!file_is_present(op.param2)) {
      // tried to load, but there's no save data in the file.
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::NO_MEMORY;
    } else {
      mc_dispatch_load_async();
    }
  }
}

/////////////////////////
// Memory Card Functions
/////////////////////////

// These functions are called from GOAL to start memory card operations.

/*!
 * Set the language or something.
 * Why is this a memory card func?
 */
void MC_set_language(s32 l) {
  printf("Language set to %d\n", l);
  language = l;
}

/*!
 * Set the current memory card operation to FORMAT the given card.
 * Doesn't do anything in the port because we don't use memory cards.
 */
u64 MC_format(s32 /*card_idx*/) {
  mc_print("MC_format requested (stubbed, returning OK)");
  return u64(McStatusCode::OK);
  // u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  // mc_print("requested format");
  // if (can_add) {
  //  mc_print("setting op to format");
  //  op.operation = MemoryCardOperationKind::FORMAT;
  //  op.result = McStatusCode::BUSY;
  //  op.retry_count = 100;
  //  op.param = card_idx;
  //}
  // return can_add;
}

/*!
 * Set the current memory card operation to UNFORMAT the given card.
 * You get the idea.
 */
u64 MC_unformat(s32 /*card_idx*/) {
  mc_print("MC_unformat requested (stubbed, returning OK)");
  return u64(McStatusCode::OK);
  // u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  // mc_print("requested unformat");
  // if (can_add) {
  //  mc_print("setting op to unformat");
  //  op.operation = MemoryCardOperationKind::UNFORMAT;
  //  op.result = McStatusCode::BUSY;
  //  op.retry_count = 100;
  //  op.param = card_idx;
  //}
  // return can_add;
}

/*!
 * Set the current memory card operation to create the save file.
 * The data I believe is just an empty buffer used as temporary storage.
 */
u64 MC_createfile(s32 /*param*/, Ptr<u8> /*data*/) {
  mc_print("MC_createfile requested (stubbed, returning OK)");
  return u64(McStatusCode::OK);
  // u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  // mc_print("requested createfile");
  // if (can_add) {
  //  mc_print("setting op to create file");
  //  op.operation = MemoryCardOperationKind::CREATE_FILE;
  //  op.result = McStatusCode::BUSY;
  //  op.retry_count = 100;
  //  op.param = param;
  //  op.data_ptr = data;
  //}
  // return can_add;
}

/*!
 * Set the current operation to SAVE.
 * The "summary data" is data that will be used when previewing save files (number of orbs etc)
 * TODO put synchronous call here
 */
u64 MC_save(s32 card_idx, s32 file_idx, Ptr<u8> save_data, Ptr<u8> save_summary_data) {
  mc_print("requested save");
  u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  if (can_add) {
    mc_print("setting op to save");
    op.operation = MemoryCardOperationKind::SAVE;
    op.result = McStatusCode::BUSY;
    op.retry_count = 100;
    op.param = card_idx;
    op.param2 = file_idx;
    op.data_ptr = save_data;
    op.data_ptr2 = save_summary_data;
  }
  return can_add;
}

/*!
 * Set the current operation to LOAD.
 * TODO put synchronous call here
 */
u64 MC_load(s32 card_idx, s32 file_idx, Ptr<u8> data) {
  mc_print("requested load");
  u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  if (can_add) {
    mc_print("setting op to load");
    op.operation = MemoryCardOperationKind::LOAD;
    op.result = McStatusCode::BUSY;
    op.retry_count = 100;
    op.param = card_idx;
    op.param2 = file_idx;
    op.data_ptr = data;
  }
  return can_add;
}

/*!
 * Some sort of test function for memory card stuff.
 * This is exported as a GOAL function, but nothing calls it.
 */
void MC_makefile(s32 port, s32 size) {
  sceMcMkdir(port, 0, "/BASCUS-00000XXXXXXXX");
  // wait for operation to complete
  s32 cmd, result, fd;
  sceMcSync(0, &cmd, &result);

  if (result == sceMcResSucceed || result == sceMcResNoEntry) {
    // it worked, or the folder already exists...

    // open file
    sceMcOpen(port, 0, "/BASCUS-00000XXXXXXXX/BASCUS-00000XXXXXXXX", SCE_CREAT | SCE_WRONLY);
    sceMcSync(0, &cmd, &fd);

    if (result < 0) {
      printf("Can't open file on memcard [%d]\n", result);
    } else {
      // write some random crap into the memory card.
      sceMcWrite(fd, Ptr<u8>(0x1000000).c(), size);
      sceMcSync(0, &cmd, &result);
      if (result != size) {
        printf("Only written %d bytes\n", result);
      }
      sceMcClose(fd);
      sceMcSync(0, &cmd, &result);
    }
  } else {
    printf("Can\'t create garbage folder [%d]\n", result);
  }
}

/*!
 * Get the result of the currently executing (or most recently executed) command
 */
u32 MC_check_result() {
  return (u32)op.result;
}

/*!
 * Update the info for the given slot.
 * You can call this at any time.
 * The slot includes the four save slots (8 banks), and a few other files.
 */
void MC_get_status(s32 /*slot*/, Ptr<mc_slot_info> info) {
  // slot is ignored, so you'll get the same thing regardless of what slot you pick
  // NOTE: the entry/done traces below are load-bearing diagnostics -- if the game ever
  // freezes in the save dialog again, mc-trace.txt ending with "get-status: begin"
  // (or "update-card: begin" without its "done") pinpoints the hang inside the card
  // scan, while a complete pair proves the freeze is on the GOAL side instead.
  mc_print_poll("get-status: begin");

  info->handle = 0;
  info->known = 0;
  info->formatted = 0;
  info->initted = 0;
  for (s32 i = 0; i < 4; i++) {
    info->files[i].present = 0;
  }
  info->last_file = 0xffffffff;
  info->mem_required = SAVE_SIZE[g_game_version];
  info->mem_actual = 0;

  pc_update_card();
  info->known = 1;
  info->handle = PC_MEM_CARD_HANDLE;
  info->formatted = 1;
  info->mem_actual = SAVE_SIZE[g_game_version];  // idk TODO does this matter?
  info->initted = 1;
  // copy over the preview data.
  for (s32 file = 0; file < 4; file++) {
    info->files[file].present = mc_files[file].present;
    for (s32 i = 0; i < 64; i++) {  // actually a loop over u32's
      info->files[file].data[i] = mc_files[file].data[i];
    }
  }
  info->last_file = mc_last_file;
  mc_print_poll("get-status: done");
}
