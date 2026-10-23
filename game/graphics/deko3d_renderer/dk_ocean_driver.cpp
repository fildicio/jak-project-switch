/*
 * dk_ocean_driver.cpp - deko3d ocean drivers, CPU half (T95, AI-assisted)
 *
 * Port of the GL DMA walks:
 *   - OceanNear.cpp   render_jak1 / render_jak2  (bucket 63)
 *   - OceanMidAndFar.cpp render_jak1 (jak1 path only here) + handle_ocean_far
 *     + handle_ocean_mid  (bucket 4)
 *   - OceanMid.cpp    run / run_jak2 (the VU1 mid driver loop)
 * The VU1 programs themselves are the T94 verbatim copies (dk_ocean_near.cpp /
 * dk_ocean_mid.cpp); the decode is the T93 CommonRecorder.
 *
 * Documented deltas from the GL files (everything else is line-for-line):
 *   - render_state->next_bucket -> the next_bucket parameter
 *   - ScopedProfilerNode / prof usage dropped (dk has its own counters)
 *   - m_texture_renderer (per-renderer GL OceanTexture) -> the shared
 *     TextureGen s_texture (GL re-registers the same tbp 8160 after each gen,
 *     so last-write-wins is preserved)
 *   - m_direct.render_gif(...) -> dkdirect::render_ocean_far_gif(...) (the
 *     dkdirect capture walk owns the GIF decode; skipped when the direct pass
 *     is off, the DMA is still consumed)
 *   - flush_near/flush_mid at the walk ends -> nothing here: the decoded
 *     arrays live in the recorders; dk_ocean_flush.cpp draws them (mid's
 *     reverse_indices CPU half still runs at capture time)
 *   - the jak2-only OceanEnvmap pass is not ported (jak1 scope; the jak2 walk
 *     would consume its DMA section - render_jak2 below keeps the GL reads)
 */

#include "game/graphics/deko3d_renderer/dk_ocean_driver.h"

#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "game/graphics/deko3d_renderer/dk_ocean_mid.h"
#include "game/graphics/deko3d_renderer/dk_ocean_near.h"

#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include "game/graphics/deko3d_renderer/DirectDk.h"
#define DK_OCEAN_DIRECT 1
#else
#define DK_OCEAN_DIRECT 0
#endif

#if defined(__SWITCH__)
// FIX 110 pattern (dk_ocean_common.cpp): the console run log, declared locally so
// headers stay plain C++.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

namespace dkocean {
namespace {

Near s_near;
Mid s_mid;
TextureGen s_texture;
bool s_texture_init = false;
LookupFn s_lookup = nullptr;
DriverStats s_stats;
bool s_far_enabled = false;

// GL OceanNear.cpp 24-27, verbatim.
bool is_near_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 2 && tag.kind == DmaTag::Kind::CNT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::DIRECT;
}

// GL OceanMid.cpp 5-8, verbatim. FIX 111: Mid::run / Mid::run_jak2 originally used
// the outer-chain is_end_tag below, so the mid walk never saw ITS end tag (a
// NOP+DIRECT CNT qwc==2 packet), fell into the catch-all else and killed the
// process on the first jungle ocean-mid chain (2026-10-24 console crash).
bool is_mid_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 2 && tag.kind == DmaTag::Kind::CNT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::DIRECT;
}

// GL OceanMidAndFar.cpp 179-182, verbatim.
bool is_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 0 && tag.kind == DmaTag::Kind::NEXT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::NOP;
}

}  // namespace

void init_driver() {
  if (!s_texture_init) {
    s_texture.init();
    s_texture_init = true;
  }
}

// ---- GL OceanNear::render_jak1 (OceanNear.cpp 52-137), deltas above ----
void Near::render_jak1(DmaFollower& dma, TextureGen& texture_renderer, u32 next_bucket) {
  // jump to bucket
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  // see if bucket is empty or not
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == next_bucket);
    return;
  }

  {
    // TODO(GL): this looks the same as the previous ocean renderer to me... why do it again?
    texture_renderer.handle_ocean_texture_jak1(dma);
  }

  if (dma.current_tag().qwc != 2) {
    lg::error("abort OceanNear::render!");
    while (dma.current_tag_offset() != next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // direct setup
  {
    m_recorder.init_for_near();
    auto setup = dma.read_and_advance();
    ASSERT(setup.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(setup.vifcode1().kind == VifCode::Kind::DIRECT);
    ASSERT(setup.size_bytes == 32);
  }

  // oofset and base
  {
    auto ob = dma.read_and_advance();
    ASSERT(ob.size_bytes == 0);
    auto base = ob.vifcode0();
    auto off = ob.vifcode1();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(off.kind == VifCode::Kind::OFFSET);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    ASSERT(off.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  while (!is_near_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();

    if (v0.kind == VifCode::Kind::STCYCL && v1.kind == VifCode::Kind::UNPACK_V4_32) {
      ASSERT(v0.immediate == 0x404);
      auto up = VifCodeUnpack(v1);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::STMOD) {
      ASSERT(v1.immediate == 0);
      switch (v0.immediate) {
        case 0:
          run_call0_vu2c();
          break;
        case 39:
          run_call39_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown ocean near call: {}", v0.immediate));
      }
    }
  }

  while (dma.current_tag_offset() != next_bucket) {
    dma.read_and_advance();
  }

  // GL: m_common_ocean_renderer.flush_near(render_state, prof) - the dk flush
  // is recorded by dk_ocean_flush.cpp from the recorder's arrays.
  s_stats.near_walks++;
}

// ---- GL OceanNear::render_jak2 (OceanNear.cpp 139-219), same deltas ----
void Near::render_jak2(DmaFollower& dma, TextureGen& texture_renderer, u32 next_bucket) {
  // jump to bucket
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.size_bytes == 0);

  // see if bucket is empty or not
  if (dma.current_tag_offset() == next_bucket) {
    return;
  }

  {
    texture_renderer.handle_ocean_texture_jak1(dma);  // jak1 path (see file comment)
  }

  if (dma.current_tag().qwc != 2) {
    lg::error("abort OceanNear::render!");
    while (dma.current_tag_offset() != next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // direct setup
  {
    m_recorder.init_for_near();
    auto setup = dma.read_and_advance();
    ASSERT(setup.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(setup.vifcode1().kind == VifCode::Kind::DIRECT);
    ASSERT(setup.size_bytes == 32);
  }

  // offset and base
  {
    auto ob = dma.read_and_advance();
    ASSERT(ob.size_bytes == 0);
    auto base = ob.vifcode0();
    auto off = ob.vifcode1();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(off.kind == VifCode::Kind::OFFSET);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    ASSERT(off.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  while (!is_near_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();

    if (v0.kind == VifCode::Kind::STCYCL && v1.kind == VifCode::Kind::UNPACK_V4_32) {
      ASSERT(v0.immediate == 0x404);
      auto up = VifCodeUnpack(v1);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::STMOD) {
      ASSERT(v1.immediate == 0);
      switch (v0.immediate) {
        case 0:
          run_call0_vu2c_jak2();
          break;
        case 39:
          run_call39_vu2c_jak2();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown ocean near call: {}", v0.immediate));
      }
    }
  }

  while (dma.current_tag_offset() != next_bucket) {
    dma.read_and_advance();
  }

  s_stats.near_walks++;
}
void Mid::run(DmaFollower& dma) {
  m_recorder.init_for_mid();
  // first is setting base and offset
  {
    auto base_offset_tag = dma.read_and_advance();
    ASSERT(base_offset_tag.size_bytes == 0);
    auto base = base_offset_tag.vifcode0();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    auto offset = base_offset_tag.vifcode1();
    ASSERT(offset.kind == VifCode::Kind::OFFSET);
    ASSERT(offset.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  // next is constants
  {
    auto constants = dma.read_and_advance();
    ASSERT(constants.size_bytes == sizeof(Constants));
    ASSERT(constants.vifcode0().kind == VifCode::Kind::STCYCL);  // for whatever reason they do this
    auto unpack = constants.vifcode1();
    ASSERT(VifCodeUnpack(unpack).addr_qw == Vu1Data::CONSTANTS);
    memcpy(&m_constants, constants.data, sizeof(Constants));
    memcpy(m_vu_data + Vu1Data::CONSTANTS, &m_constants, sizeof(Constants));
  }

  // next is call 0
  {
    auto call0 = dma.read_and_advance();
    ASSERT(call0.vifcode0().kind == VifCode::Kind::STCYCL);
    auto c = call0.vifcode1();
    ASSERT(c.kind == VifCode::Kind::MSCALF);
    ASSERT(c.immediate == 0);
    run_call0();
  }

  while (!is_mid_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      // ASSERT(up.use_tops_flag);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
      ASSERT(16 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        memcpy(m_vu_data + addr + i, temp, 16);
      }
      ASSERT(4 * v1.num == data.size_bytes);

    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x204 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        // cl = 4
        // wl = 2
        u32 addr_off = 4 * (i / 2) + i % 2;
        memcpy(m_vu_data + addr + addr_off, temp, 16);
      }
      ASSERT(8 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::MSCALF) {
      switch (v1.immediate) {
        case 46:
          run_call46_vu2c();
          break;
        case 73:
          run_call73_vu2c();
          break;
        case 107:
          run_call107_vu2c();
          break;
        case 275:
          run_call275_vu2c();
          break;
        default:
          lg::warn("unknown call1: {}", v1.immediate);
      }
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::FLUSHA) {
      switch (v0.immediate) {
        case 41:
          run_call41_vu2c();
          break;
        case 43:
          run_call43_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown call2: {}", v0.immediate));
      }
    } else {
      // Unknown packet. GL ASSERTs here, but a fatal assert _exit(1)s the whole
      // game on the console (FIX 111): count it, log throttled, and abort this
      // mid walk. handle_ocean_mid's skip loop then walks the chain to its real
      // outer end tag, so the frame survives with this mid pass missing.
      s_stats.mid_aborts++;
      static u32 s_mid_abort_logged = 0;
      if (s_mid_abort_logged < 10) {
        s_mid_abort_logged++;
#if defined(__SWITCH__)
        switch_run_logf("[dk] ocean mid abort on %s %s", data.vifcode0().print().c_str(),
                        data.vifcode1().print().c_str());
#else
        lg::error("ocean mid abort on {} {}", data.vifcode0().print(),
                  data.vifcode1().print());
#endif
      }
      break;
    }
  }
  // GL flush_mid CPU half: reverse the envmap indices; the draw itself is
  // recorded by dk_ocean_flush.cpp (documented T95 delta).
  m_recorder.reverse_bucket_indices(1);
  s_stats.mid_runs++;
}

void Mid::run_jak2(DmaFollower& dma) {
  m_recorder.init_for_mid();
  // first is setting base and offset
  {
    auto base_offset_tag = dma.read_and_advance();
    ASSERT(base_offset_tag.size_bytes == 0);
    auto base = base_offset_tag.vifcode0();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    auto offset = base_offset_tag.vifcode1();
    ASSERT(offset.kind == VifCode::Kind::OFFSET);
    ASSERT(offset.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  // next is constants
  {
    auto constants = dma.read_and_advance();
    ASSERT(constants.size_bytes == sizeof(Constants));
    ASSERT(constants.vifcode0().kind == VifCode::Kind::STCYCL);  // for whatever reason they do this
    auto unpack = constants.vifcode1();
    ASSERT(VifCodeUnpack(unpack).addr_qw == Vu1Data::CONSTANTS);
    memcpy(&m_constants, constants.data, sizeof(Constants));
    memcpy(m_vu_data + Vu1Data::CONSTANTS, &m_constants, sizeof(Constants));
  }

  // next is call 0
  {
    auto call0 = dma.read_and_advance();
    ASSERT(call0.vifcode0().kind == VifCode::Kind::STCYCL);
    auto c = call0.vifcode1();
    ASSERT(c.kind == VifCode::Kind::MSCALF);
    ASSERT(c.immediate == 0);
    run_call0();
  }

  while (!is_mid_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      // ASSERT(up.use_tops_flag);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
      ASSERT(16 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        memcpy(m_vu_data + addr + i, temp, 16);
      }
      ASSERT(4 * v1.num == data.size_bytes);

    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x204 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        // cl = 4
        // wl = 2
        u32 addr_off = 4 * (i / 2) + i % 2;
        memcpy(m_vu_data + addr + addr_off, temp, 16);
      }
      ASSERT(8 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::MSCALF) {
      switch (v1.immediate) {
        case 46:
          run_call46_vu2c();
          break;
        case 73:
          run_call73_vu2c_jak2();
          break;
        case 107:
          run_call107_vu2c_jak2();
          break;
        case 275:
          run_call275_vu2c_jak2();
          break;
        default:
          lg::warn("unknown call1: {}", v1.immediate);
      }
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::FLUSHA) {
      switch (v0.immediate) {
        case 41:
          run_call41_vu2c();
          break;
        case 43:
          run_call43_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown call2: {}", v0.immediate));
      }
    } else {
      // Unknown packet, jak2 walk: graceful abort like the jak1 loop above (FIX 111).
      s_stats.mid_aborts++;
      static u32 s_mid_abort_logged = 0;
      if (s_mid_abort_logged < 10) {
        s_mid_abort_logged++;
#if defined(__SWITCH__)
        switch_run_logf("[dk] ocean mid (jak2) abort on %s %s", data.vifcode0().print().c_str(),
                        data.vifcode1().print().c_str());
#else
        lg::error("ocean mid (jak2) abort on {} {}", data.vifcode0().print(),
                  data.vifcode1().print());
#endif
      }
      break;
    }
  }
  // GL flush_mid CPU half (see run above): reverse envmap indices; the draw is
  // recorded by dk_ocean_flush.cpp (documented T95 delta).
  m_recorder.reverse_bucket_indices(1);
  s_stats.mid_runs++;
}

namespace {

// ---- GL OceanMidAndFar::handle_ocean_far (154-177); render_gif rerouted ----
void handle_ocean_far(DmaFollower& dma) {
  auto init_data = dma.read_and_advance();
  ASSERT(init_data.size_bytes == 160);
  u8 init_data_buffer[160];
  memcpy(init_data_buffer, init_data.data, 160);

  // this is a bit of a hack, but it patches the ta0 to 0 in
  // (set! (-> (the-as (pointer gs-texa) s4-0) 8) (new 'static 'gs-texa :ta0 #x80 :ta1 #x80))
  // TODO figure out if we actually have do something here.
  u8 val = 0;
  memcpy(init_data_buffer + 80, &val, 1);
#if DK_OCEAN_DIRECT
  if (s_far_enabled) {
    dkdirect::render_ocean_far_gif(init_data_buffer, 160);
  }
#endif
  s_stats.far_packets++;

  while (dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP) {
    auto data = dma.read_and_advance();
    ASSERT(data.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(data.vifcode1().kind == VifCode::Kind::DIRECT);
    ASSERT(data.size_bytes / 16 == data.vifcode1().immediate);
#if DK_OCEAN_DIRECT
    if (s_far_enabled) {
      dkdirect::render_ocean_far_gif(data.data, data.size_bytes);
    }
#endif
    s_stats.far_packets++;
  }
}

// ---- GL OceanMidAndFar::handle_ocean_mid (183-203); jak1 run only ----
void handle_ocean_mid(DmaFollower& dma) {
  if (dma.current_tag_vifcode0().kind == VifCode::Kind::BASE) {
    s_mid.run(dma);
  } else {
    // not drawing
    return;
  }

  while (!is_end_tag(dma.current_tag(), dma.current_tag_vifcode0(), dma.current_tag_vifcode1())) {
    dma.read_and_advance();
  }
}

// ---- GL OceanMidAndFar::render_jak1 (52-93); deltas in the file header ----
void render_mid_far(DmaFollower& dma, u32 next_bucket) {
  // jump to bucket
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  // see if bucket is empty or not
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == next_bucket);
    s_stats.mid_far_empty++;
    return;
  }
#if DK_OCEAN_DIRECT
  dkdirect::ocean_far_begin();  // GL m_direct.reset_state()
#endif

  {
    s_texture.handle_ocean_texture_jak1(dma);
  }

  handle_ocean_far(dma);
  // GL m_direct.flush_pending(render_state, prof) + set_mipmap(true): the far
  // batches close here (the mip flag is moot on the single-level dk path).
#if DK_OCEAN_DIRECT
  dkdirect::ocean_far_end();
#endif

  handle_ocean_mid(dma);

  auto final_next = dma.read_and_advance();
  ASSERT(final_next.vifcode0().kind == VifCode::Kind::NOP &&
         final_next.vifcode1().kind == VifCode::Kind::NOP && final_next.size_bytes == 0);
  for (int i = 0; i < 4; i++) {
    dma.read_and_advance();
  }
  ASSERT(dma.current_tag_offset() == next_bucket);

  // GL m_direct.flush_pending + set_mipmap(false): nothing was added since the
  // flush above, so this is a no-op on the dk path.
  s_stats.mid_far_walks++;
}

}  // namespace

void capture_chain(const void* data, uint32_t start_offset, LookupFn lookup, bool far_enabled) {
  s_lookup = lookup;
  s_far_enabled = far_enabled;
  init_driver();
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  s_stats.frames++;

  {
    // GL bucket 4 (OCEAN_MID_AND_FAR): texture -> far -> mid
    const uint32_t slot = buckets_base + 16 * kJak1MidFarBucket;
    DmaFollower dma(data, slot);
    render_mid_far(dma, slot + 16);
  }
  {
    // GL bucket 63 (OCEAN_NEAR)
    const uint32_t slot = buckets_base + 16 * kJak1NearBucket;
    DmaFollower dma(data, slot);
    s_near.render_jak1(dma, s_texture, slot + 16);
  }
}

const DriverStats& driver_stats() {
  return s_stats;
}

CommonRecorder& near_recorder() {
  return s_near.recorder();
}

CommonRecorder& mid_recorder() {
  return s_mid.recorder();
}

TextureGen& texture_gen() {
  return s_texture;
}

LookupFn lookup() {
  return s_lookup;
}

}  // namespace dkocean
