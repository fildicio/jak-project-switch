#pragma once

// jak2+ per-proto visibility for the native tie/shrub recorders (AI-assisted).
// The game sends, after the TfragPcPortData packet, a list of zero-terminated proto names that
// must be hidden this frame (GL: TieProtoVisibility::update / Shrub update_vis_mask). Without it
// the dk path drew every hidden proto: wasted GPU time and objects the game had turned off.

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace dkprotovis {

struct Cache {
  std::vector<u8> flags;  // 1 = visible, indexed by proto_idx
  std::unordered_map<std::string, std::vector<u32>> name_to_idx;
  uint64_t load_id = UINT64_MAX;
  bool all_visible = true;
};

// Copy of the mask packet (the DMA buffer is not guaranteed to outlive the capture).
inline void copy_mask(const u8* data, u32 size, std::vector<u8>* out) {
  out->assign(data, data + size);
}

// Rebuild `c` for `names` when the level changed, then apply `mask`. Returns c.flags.
inline const std::vector<u8>& update(Cache* c,
                                     uint64_t load_id,
                                     const std::vector<std::string>& names,
                                     const std::vector<u8>& mask) {
  if (c->load_id != load_id || c->flags.size() != names.size()) {
    c->load_id = load_id;
    c->flags.assign(names.size(), 1);
    c->name_to_idx.clear();
    for (u32 i = 0; i < names.size(); i++) {
      c->name_to_idx[names[i]].push_back(i);
    }
    c->all_visible = true;
  }
  if (!c->all_visible) {
    std::fill(c->flags.begin(), c->flags.end(), 1);
    c->all_visible = true;
  }
  size_t i = 0;
  while (i < mask.size()) {
    const size_t start = i;
    while (i < mask.size() && mask[i] != 0) {
      i++;
    }
    if (i > start) {
      const auto it = c->name_to_idx.find(std::string((const char*)mask.data() + start, i - start));
      if (it != c->name_to_idx.end()) {
        c->all_visible = false;
        for (u32 x : it->second) {
          c->flags[x] = 0;
        }
      }
    }
    i++;
  }
  return c->flags;
}

}  // namespace dkprotovis
