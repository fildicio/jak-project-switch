#include "sif_ee.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/FsLock.h"

#if defined(__SWITCH__)
#include "game/switch/boot_log.h"
#include "game/switch/run_log.h"
#endif

#include "game/runtime.h"
#include "game/system/iop_thread.h"

namespace ee {

namespace {
::IOP* iop;
std::unordered_map<s32, FILE*> sce_fds;
// Descriptor ids used to be derived from sce_fds.size(), which repeats an id as soon as any
// file is closed -- the second open then overwrites the live entry of an already-open file,
// leaking it and handing two callers the same descriptor.
s32 sce_fd_counter = 0;
}  // namespace

void LIBRARY_sceSif_register(::IOP* i) {
  iop = i;
}

void LIBRARY_INIT_sceSif() {
  SWITCH_FS_LOCK();
  iop = nullptr;
  for (auto& kv : sce_fds) {
    fclose(kv.second);
  }
  sce_fds.clear();
  sce_fd_counter = 0;
}
void sceSifInitRpc(unsigned int mode) {
  (void)mode;
}

int sceSifRebootIop(const char* imgfile) {
  (void)imgfile;
  return 1;
}

int sceSifSyncIop() {
  return 1;
}

void sceFsReset() {}

int sceSifLoadModule(const char* name, int arg_size, const char* args) {
  if (!strcmp(name, "cdrom0:\\\\DRIVERS\\\\OVERLORD.IRX;1") ||
      !strcmp(name, "host0:binee/overlord.irx") || !strcmp(name, "host0:bin/overlord.irx")) {
    const char* src = args;
    char* dst = iop->overlord_arg_data;
    int cnt;
    iop->overlord_argv[0] = nullptr;
    for (cnt = 1; src - args < arg_size; cnt++) {
      auto len = strlen(src);
      memcpy(dst, src, len + 1);
      iop->overlord_argv[cnt] = dst;
      dst += len + 1;
      src += len + 1;
    }
    iop->overlord_argc = cnt;

    for (int i = 0; i < cnt; i++) {
      if (iop->overlord_argv[i])
        printf("arg %d : %s\n", i, iop->overlord_argv[i]);
    }
    iop->set_ee_main_mem(g_ee_main_mem);
    iop->send_status(IOP_Status::IOP_OVERLORD_INIT);
    iop->wait_for_overlord_init_finish();
  }

  return 1;
}

s32 sceSifCallRpc(sceSifClientData* bd,
                  u32 fno,
                  u32 mode,
                  void* send,
                  s32 ssize,
                  void* recv,
                  s32 rsize,
                  void* end_func,
                  void* end_para) {
  ASSERT(!end_func);
  ASSERT(!end_para);
  ASSERT(mode == 1);  // async
  iop->kernel.sif_rpc(bd->rpcd.id, fno, mode, send, ssize, recv, rsize);
  iop->signal_run_iop();
  return 0;
}

s32 sceSifCheckStatRpc(sceSifRpcData* bd) {
  iop->signal_run_iop();
  return iop->kernel.sif_busy(bd->id);
}

s32 sceSifBindRpc(sceSifClientData* bd, u32 request, u32 mode) {
  ASSERT(mode == 1);  // async
  bd->rpcd.id = request;
  bd->serve = (sceSifServeData*)1;
  return 0;
}

s32 sceOpen(const char* filename, s32 flag) {
  SWITCH_FS_LOCK();
  FILE* fp = nullptr;
  auto name = file_util::get_file_path({filename});
#if defined(__SWITCH__)
  {
    // fsync'd per line, so it survives a freeze here -- this is the call that died during boot
    // while writing the default PC settings, and the resolved path is what proves whether the
    // device-prefix handling in get_file_path() is doing the right thing.
    std::string trace = "[sceOpen] flag=" + std::to_string(flag) + " in=" + filename +
                        " resolved=" + name + "\n";
    switch_boot_log(trace.c_str());
  }
#endif
  switch (flag) {
    case SCE_RDONLY: {
      fp = file_util::open_file(name.c_str(), "rb");
    } break;

    default: {
      // either append or truncate
      file_util::create_dir_if_needed_for_file(name);
      if (flag & SCE_TRUNC) {
        fp = file_util::open_file(name.c_str(), "w");
      } else {
        fp = file_util::open_file(name.c_str(), "a+");
      }
    } break;
  }
  if (!fp) {
    printf("[SCE] sceOpen(%s) failed.\n", name.c_str());
#if defined(__SWITCH__)
    // FIX 7b: the boot_log trace above is latched off at boot-complete, exactly when
    // save files get touched. Route the same info to the whole-session run log.
    switch_run_logf("[sceOpen] FAILED flag=%d in=%s resolved=%s", flag, filename,
                    name.c_str());
#endif
    return -1;
  }

  s32 fp_idx = ++sce_fd_counter;
  sce_fds[fp_idx] = fp;
#if defined(__SWITCH__)
  // FIX 7u: fp_idx is a monotonically-increasing counter, NOT a POSIX fd, so a climbing
  // number proves nothing. sce_fds.size() is the number actually still open -- if that
  // grows without bound, handles are being leaked and later opens (e.g. creating a save)
  // will start failing.
  switch_run_logf("[sceOpen] ok fd=%d open_handles=%d flag=%d in=%s", fp_idx,
                  (int)sce_fds.size(), flag, filename);
#endif
  return fp_idx;
}

s32 sceMkDir(const char* filename, s32 flag) {
  return -1;
}

s32 sceClose(s32 fd) {
  SWITCH_FS_LOCK();
  if (fd < 0) {
    // todo, what should we really return?
    return 0;
  }

  auto kv = sce_fds.find(fd);
  if (kv != sce_fds.end()) {
    fclose(kv->second);
    sce_fds.erase(fd);
    return 0;
  } else {
    printf("[SCE] sceClose called on invalid fd\n");
    return 0;
  }
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  SWITCH_FS_LOCK();
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    return -1;
  } else {
    return fread(buf, 1, nbyte, kv->second);
  }
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  SWITCH_FS_LOCK();
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    ASSERT(false);
    return -1;
  } else {
    return fwrite(buf, 1, nbyte, kv->second);
  }
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  SWITCH_FS_LOCK();
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    return -1;
  } else {
    switch (where) {
      case SCE_SEEK_CUR:
        fseek(kv->second, offset, SEEK_CUR);
        return ftell(kv->second);
      case SCE_SEEK_END:
        fseek(kv->second, offset, SEEK_END);
        return ftell(kv->second);
      case SCE_SEEK_SET:
        fseek(kv->second, offset, SEEK_SET);
        return ftell(kv->second);
      default:
        ASSERT(false);
        return -1;
    }
  }
}

}  // namespace ee
