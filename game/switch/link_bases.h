#pragma once

/*!
 * @file link_bases.h
 * Resident record of every linked object's FINAL code range. (AI-assisted)
 *
 * FIX 27 (Switch port): gk_boot_log.txt gets "[klink] obj=<name> base=EE+0x… size=…" per
 * linked object, but switch_boot_log() latches itself off at "boot complete" -- which
 * happens right after the title screen. Level code links AFTER that, so the SD boot log
 * cannot resolve the level-object offsets that the 2026-09-17/18 crash triage needs.
 *
 * This table is the always-on record: the linker appends one entry per link (ring buffer,
 * newest wins in find()), and the exception handler (FIX 28) dumps and uses it at crash
 * time, so a crash in level code names its object without any host-side boot matching.
 *
 * Deliberately light includes (no runtime.h/kernel headers): game/switch/platform.cpp
 * includes this too, and it must not drag in the u128-ordering problem.
 */

#include <atomic>
#include <stdint.h>
#include <string.h>

struct SwitchLinkBaseRecord {
  uint32_t base;  // EE offset of the final code block (after work_v2 moves)
  uint32_t size;
  char name[40];
};

inline SwitchLinkBaseRecord* switch_link_bases_table() {
  static SwitchLinkBaseRecord table[1024];
  return table;
}

inline std::atomic<uint32_t>& switch_link_bases_total() {
  static std::atomic<uint32_t> total{0};
  return total;
}

inline uint32_t switch_link_bases_count() {
  uint32_t total = switch_link_bases_total().load(std::memory_order_relaxed);
  return total < 1024 ? total : 1024;
}

/*!
 * Record one linked object. Called from the linker (single EE kernel thread in practice);
 * find() tolerates the benign race if that ever changes -- worst case a torn name on the
 * entry being written, never a wild pointer, because base/size/name are all inline data.
 */
inline void switch_link_bases_record(const char* name, uint32_t base, uint32_t size) {
  if (!name || !base || !size) {
    return;
  }
  uint32_t idx = switch_link_bases_total().fetch_add(1, std::memory_order_relaxed);
  SwitchLinkBaseRecord& r = switch_link_bases_table()[idx & 1023];
  r.base = base;
  r.size = size;
  strncpy(r.name, name, sizeof(r.name) - 1);
  r.name[sizeof(r.name) - 1] = '\0';
}

/*!
 * Newest-first lookup: objects can be re-linked (level reload), so a later record for an
 * overlapping range is the one that is live.
 */
inline const SwitchLinkBaseRecord* switch_link_bases_find(uint32_t ee_addr) {
  uint32_t total = switch_link_bases_total().load(std::memory_order_relaxed);
  uint32_t first = total > 1024 ? total - 1024 : 0;
  for (uint32_t i = total; i-- > first;) {
    const SwitchLinkBaseRecord& r = switch_link_bases_table()[i & 1023];
    if (ee_addr >= r.base && ee_addr - r.base < r.size) {
      return &r;
    }
  }
  return nullptr;
}
