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
#include <thread>

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
 * A questionable checksum used on memory card data.
 */
u32 mc_checksum(Ptr<u8> data, s32 size) {
  if (size < 0) {
    size += 3;
  }

  u32 result = 0;
  u32* data_u32 = (u32*)data.c();
  for (s32 i = 0; i < size / 4; i++) {
    result = result << 1 ^ (s32)result >> 0x1f ^ data_u32[i] ^ MEM_CARD_MAGIC;
  }

  return result ^ 0xedd1e666;
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

/*!
 * PC port function to save a file. This does the whole saving at once, synchronously.
 */
void pc_game_save_synch() {
  Timer mc_timer;
  mc_timer.start();
  pc_update_card();
  auto path = mc_get_filename(g_game_version, 0);
  file_util::create_dir_if_needed_for_file(path.string());

  // cd_reprobe_save //
  if (!file_is_present(op.param2)) {
    mc_print("reprobe save: first time!");
    // first time saving!
    p2 = 0;  // save count 0
    p4 = 0;  // first bank for file
  } else {
    p2 = mc_files[op.param2].most_recent_save_count + 1;  // increment save count
    p4 = mc_files[op.param2].last_saved_bank ^ 1;         // use the other bank
  }

  // reserve 0 as "I never saved" and use 1 instead.
  if (p2 == 0) {
    p2 = 1;
  }

  // file*2 + p4 is the bank (2 banks per file, p4 is 0 or 1 to select the bank)
  // 4 is the first bank file
  mc_print("open {} for saving", mc_get_filename_no_dir(g_game_version, op.param2 * 2 + 4 + p4));
  auto save_path = mc_get_filename(g_game_version, op.param2 * 2 + 4 + p4);
  file_util::create_dir_if_needed_for_file(save_path.string());
  bool saved_ok = false;
  // The overlord thread streams files off the ISO (level geometry, STR audio/video)
  // while the GOAL kernel thread runs the save. newlib's fsdev layer is not
  // thread-safe on this toolchain (see game/switch/boot_log.h), so a write can fail
  // transiently -- the 2026-09-11 on-device failure wrote the 1 KiB header fine and
  // then failed the very first 8 KiB payload chunk while LoadISOFileChunkToEE was
  // running. Each retry starts from a completely fresh FILE*, which re-enters the
  // stdio layer from a clean state.
  constexpr int kMaxSaveAttempts = 3;
  for (int attempt = 1; attempt <= kMaxSaveAttempts && !saved_ok; attempt++) {
    if (attempt > 1) {
      mc_print("save attempt {} failed - retrying", attempt - 1);
      // give any concurrent file I/O a moment to drain before re-entering fsdev
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // FIX 7u -- THE SAVE BUG. This whole attempt (open -> header -> payload -> footer ->
    // fsync -> close) must be serialised against every other fsdev user, exactly like the
    // *load* path already is in pc_game_load_open_file() below. It was not, which is the
    // documented failure above: the overlord's LoadISOFileChunkToEE runs on another thread
    // and fake_iso.cpp DOES take this lock, so an unlocked save interleaves with it inside
    // newlib's non-thread-safe fsdev layer and a write comes up short. The 3-attempt retry
    // added earlier does not help, because every retry races too.
    //
    // Same root cause as the intro crash fixed in 7t (an unlocked fsdev write racing the
    // ISO thread); only the victim differs. Scoped to one iteration so the 100ms backoff
    // above never runs while holding the lock. The lock is recursive, so the mc_print()
    // calls below -- which take it themselves -- are fine.
    SWITCH_FS_LOCK();
    auto fd = file_util::open_file(save_path.string().c_str(), "wb");    if (!fd) {
      mc_print("Error opening file for saving, errno - {}", errno);
      continue;
    }
    mc_print("save file opened (attempt {}), writing header...", attempt);
    memset(&header, 0, sizeof(McHeader));
    header.save_count = p2;
    header.checksum = mc_checksum(op.data_ptr, BANK_SIZE[g_game_version]);
    header.magic = MEM_CARD_MAGIC;
    header.save_count2 = p2;
    memcpy(header.preview_data, op.data_ptr2.c(), 64);

    if (!mc_write_all(fd, &header, sizeof(McHeader))) {
      // cb_savedheader //
      fclose(fd);
      continue;
    }
    mc_print("save file writing main data ({} bytes)", (int)BANK_SIZE[g_game_version]);
    if (!mc_write_all(fd, op.data_ptr.c(), BANK_SIZE[g_game_version])) {
      // cb_saveddata //
      fclose(fd);
      continue;
    }
    mc_print("save file writing footer");
    if (!mc_write_all(fd, &header, sizeof(McHeader))) {
      // cb_savedfooter //
      fclose(fd);
      continue;
    }
    // make sure everything actually leaves the userspace stdio buffer and
    // reaches the card before we report success.
    fflush(fd);
    if (mc_sync_file(fd) != 0) {
      mc_print("WARNING: fsync of save file failed, errno - {}", errno);
    }
    if (fclose(fd) == 0) {
      // cb_closedsave //
      saved_ok = true;
    } else {
      mc_print("fclose of save file failed, errno - {}", errno);
    }
  }

  if (saved_ok) {
    mc_print("All done with saving!!");
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::OK;
    mc_files[op.param2].present = 1;
    mc_files[op.param2].most_recent_save_count = p2;
    mc_files[op.param2].last_saved_bank = p4;
    memcpy(mc_files[op.param2].data, op.data_ptr2.c(), 64);
    mc_last_file = op.param2;
  } else {
    mc_print("giving up on saving after {} attempts", (int)kMaxSaveAttempts);
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::INTERNAL_ERROR;
  }

  mc_print("synchronous save took {:.2f}ms", mc_timer.getMs());
}

void pc_game_load_open_file(FILE* fd) {
  // Covers the freads and the mid-function fs::exists (aux-bank check) below; it is
  // called recursively, which the recursive mutex handles.
  SWITCH_FS_LOCK();
  if (fd) {
    // cb_openedload //
    size_t read_size = mc_get_total_bank_size(g_game_version);
    mc_print("reading save file ({} bytes)...", (int)read_size);
    if (mc_read_all(fd, op.data_ptr.c() + p2 * read_size, read_size)) {
      // cb_readload //
      mc_print("closing save file..");
      if (fclose(fd) == 0) {
        // cb_closedload //
        // added : check if aux bank exists
        if (p2 < 1 && fs::exists(mc_get_filename(g_game_version, op.param2 * 2 + 4 + p2 + 1))) {
          p2++;
          mc_print("reading next save bank {}",
                   mc_get_filename_no_dir(g_game_version, op.param2 * 2 + 4 + p2));
          auto new_bankname = mc_get_filename(g_game_version, op.param2 * 2 + 4 + p2);
          auto new_fd = file_util::open_file(new_bankname.string().c_str(), "rb");
          pc_game_load_open_file(new_fd);
        } else {
          // let's verify the data.
          McHeader* headers[2];
          McHeader* footers[2];
          bool ok[2];

          headers[0] = (McHeader*)(op.data_ptr.c());
          footers[0] = (McHeader*)(op.data_ptr.c() + sizeof(McHeader) + BANK_SIZE[g_game_version]);
          headers[1] = (McHeader*)(op.data_ptr.c() + mc_get_total_bank_size(g_game_version));
          footers[1] = (McHeader*)(op.data_ptr.c() + mc_get_total_bank_size(g_game_version) +
                                   sizeof(McHeader) + BANK_SIZE[g_game_version]);
          // static_assert(mc_get_total_bank_size(g_game_version) * 2 == 0x21000, "save layout");
          ok[0] = true;
          ok[1] = p2 == 1;

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
                  if (mc_checksum(make_u8_ptr(headers[idx] + 1), BANK_SIZE[g_game_version]) !=
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
              op.operation = MemoryCardOperationKind::NO_OP;
              op.result = McStatusCode::NEW_GAME;
              mc_last_file = op.param2;
            } else {
              mc_print("corrupted data");
              op.operation = MemoryCardOperationKind::NO_OP;
              op.result = McStatusCode::READ_ERROR;
            }
          } else {
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
            u32 current_save_count = headers[bank]->save_count;
            memmove(
                op.data_ptr.c(),
                op.data_ptr.c() + bank * mc_get_total_bank_size(g_game_version) + sizeof(McHeader),
                BANK_SIZE[g_game_version]);
            mc_last_file = op.param2;
            mc_files[op.param2].most_recent_save_count = current_save_count;
            mc_files[op.param2].last_saved_bank = bank;
            op.operation = MemoryCardOperationKind::NO_OP;
            op.result = McStatusCode::OK;
            mc_print("load succeeded");
          }
        }
      } else {
        op.operation = MemoryCardOperationKind::NO_OP;
        op.result = McStatusCode::INTERNAL_ERROR;
      }
    } else {
      fclose(fd);
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::INTERNAL_ERROR;
    }
  } else {
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::INTERNAL_ERROR;
  }
}

/*!
 * PC port function to load a file. This does the whole loading at once, synchronously.
 */
void pc_game_load_synch() {
  Timer mc_timer;
  mc_timer.start();
  pc_update_card();

  // cb_reprobe_load //
  mc_print("opening save file {}", mc_get_filename_no_dir(g_game_version, op.param2 * 2 + 4));

  auto path = mc_get_filename(g_game_version, op.param2 * 2 + 4);
  // same fsdev thread-safety story as the save path: an IO failure surfaces as
  // INTERNAL_ERROR and is transient, so retry from a fresh FILE*. Results that come
  // from actually inspecting the loaded data (READ_ERROR, NEW_GAME, ...) are real
  // and are not retried.
  constexpr int kMaxLoadAttempts = 3;
  for (int attempt = 1; attempt <= kMaxLoadAttempts; attempt++) {
    if (attempt > 1) {
      mc_print("load attempt {} failed - retrying", attempt - 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    p2 = 0;  // pc_game_load_open_file advances this to the second bank as it goes
    auto fd = file_util::open_file(path.string().c_str(), "rb");
    pc_game_load_open_file(fd);
    if (op.result != McStatusCode::INTERNAL_ERROR) {
      break;
    }
  }

  mc_print("synchronous load took {:.2f}ms\n", mc_timer.getMs());
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
    // write game save.
    // there's no cards, keep in mind.
    pc_game_save_synch();
    // allow some number of errors.
    op.retry_count--;
    if (op.retry_count == 0) {
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::INTERNAL_ERROR;
    }
  } else if (op.operation == MemoryCardOperationKind::LOAD) {
    // load game save.
    // potato.
    if (!file_is_present(op.param2)) {
      // tried to load, but there's no save data in the file.
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::NO_MEMORY;
    } else {
      pc_game_load_synch();
      op.retry_count--;
      if (op.retry_count == 0) {
        op.operation = MemoryCardOperationKind::NO_OP;
        op.result = McStatusCode::INTERNAL_ERROR;
      }
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
