#include "LoaderStages.h"

#include "Loader.h"

#include "common/global_profiler/GlobalProfiler.h"

// ---------------------------------------------------------------------------
// Per-frame loader budget and upload chunk sizes.
//
// Desktop: generous values stream a level in over a handful of frames.
// Switch (Tegra X1): the budget is only checked *between* uploads, and a full
// 32768-vertex chunk is exactly 1 MB of PreloadedVertex - a single
// glBufferSubData of that size can stall the frame for 20+ ms (FIX 9 in
// SWITCH_PORT_SESSION_NOTES.md). Use smaller chunks and a tighter time budget
// there; levels stream in over more frames instead of hitching.
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
constexpr float LOAD_BUDGET = 2.f;           // ms
constexpr u32 STAGE_VERT_CHUNK = 8192;       // verts (~256 KB for PreloadedVertex)
constexpr u32 STAGE_INDEX_CHUNK = 8192 * 8;  // u32 indices (~256 KB)
constexpr u32 MAX_STAGE_UPLOAD_KB = 512;
// FIX 34a: back to the values the 660s-clean FIX 33a build shipped with.
[[maybe_unused]] constexpr int MAX_TEX_BYTES_PER_FRAME = 256 * 1024;
#else
constexpr float LOAD_BUDGET = 4.5f;           // ms
constexpr u32 STAGE_VERT_CHUNK = 32768;       // verts (1 MB for PreloadedVertex)
constexpr u32 STAGE_INDEX_CHUNK = 32768 * 8;  // u32 indices (1 MB)
constexpr u32 MAX_STAGE_UPLOAD_KB = 2048;
[[maybe_unused]] constexpr int MAX_TEX_BYTES_PER_FRAME = 1024 * 1024;
#endif

// ---------------------------------------------------------------------------
// FIX 33 (AI-assisted): band size for chunked texture uploads. Uploading a
// whole texture (glTexImage2D with data + mipmap generation) is atomic and
// big textures (256x256+) stalled frames for 5-45 ms each on the Switch.
// Storage is allocated with a null upload first, then pixel rows stream in
// via glTexSubImage2D bands so each frame stays inside the load budget. The
// mipmap chain is generated once, after the base level is complete.
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
constexpr int TEX_BAND_BYTES = 128 * 1024;
#else
constexpr int TEX_BAND_BYTES = 512 * 1024;
#endif

#ifdef __SWITCH__
// FIX 34 (AI-assisted): per-texture upload/mipgen instrumentation, so the split
// between "glTexImage2D cost" and "glGenerateMipmap cost" is visible in
// gk_stdout.txt on the console. If mipgen dominates after the GL_UNSIGNED_BYTE
// swap, the next step is CPU box-filtered mipmaps on the loader thread.
static double g_tex_upload_ms = 0.0;
static double g_tex_mipgen_ms = 0.0;
static int g_tex_uploaded = 0;
#endif

/*!
 * Upload a texture to the GPU, and give it to the pool.
 */
u64 add_texture(TexturePool& pool, const tfrag3::Texture& tex, bool is_common) {
  GLuint gl_tex;
  glActiveTexture(GL_TEXTURE0);
  glGenTextures(1, &gl_tex);
  glBindTexture(GL_TEXTURE_2D, gl_tex);
#ifdef __SWITCH__
  Timer tex_upload_timer;
  tex_upload_timer.start();
#endif
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA,
               GL_UNSIGNED_INT_8_8_8_8_REV, tex.data.data());
#ifdef __SWITCH__
  g_tex_upload_ms += tex_upload_timer.getMs();
  Timer tex_mip_timer;
  tex_mip_timer.start();
#endif
  glGenerateMipmap(GL_TEXTURE_2D);
#ifdef __SWITCH__
  g_tex_mipgen_ms += tex_mip_timer.getMs();
  g_tex_uploaded++;
#endif
  float aniso = 0.0f;
  glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
  if (tex.load_to_pool) {
    TextureInput in;
    in.debug_page_name = tex.debug_tpage_name;
    in.debug_name = tex.debug_name;
    in.w = tex.w;
    in.h = tex.h;
    in.gpu_texture = gl_tex;
    in.common = is_common;
    in.id = PcTextureId::from_combo_id(tex.combo_id);
    in.src_data = (const u8*)tex.data.data();
    pool.give_texture(in);
  }

  return gl_tex;
}

// ---------------------------------------------------------------------------
// FIX 33a (AI-assisted): the first FIX 33 build crashed on the Switch during
// the boot blackout load: nouveau's glTexSubImage2D client-copy faulted
// reading exactly one 128 KB band (TEX_BAND_BYTES) from an unmapped page
// (gk_fatal esr=0x92000007, memcpy len 0x20000). The identical band code
// boots fine on the host (macOS GL), so the band path interacts badly with
// Mesa/nouveau's partial-upload staging - not worth debugging remotely.
// Decision: on Switch, load textures with the original atomic add_texture()
// path that ran for months (per-frame byte caps keep the frame hitches
// bounded); desktop keeps the banded streaming. The actual FIX 33 crash fix
// (purge-before-load + buffer pooling + eviction) is untouched.
// ---------------------------------------------------------------------------
static void check_tex_invariant(const tfrag3::Texture& tex) {
  // The extractor promises data.size() == w*h (u32s). If a file ever
  // violates that, say so loudly instead of reading out of bounds.
  if ((u64)tex.w * tex.h != tex.data.size()) {
    fmt::print("[loader] TEXTURE SIZE MISMATCH: '{}' ({}x{} = {} px) has {} u32 of data\n",
               tex.debug_name, tex.w, tex.h, (u64)tex.w * tex.h, tex.data.size());
  }
}

class TextureLoaderStage : public LoaderStage {
 public:
  TextureLoaderStage() : LoaderStage("texture") {}
  bool run(Timer& timer, LoaderInput& data) override {
    LevelData& ld = *data.lev_data;
    const auto& all_textures = ld.level->textures;
#ifdef __SWITCH__
    // FIX 33a: original atomic upload path - proven on Tegra/nouveau.
    if (ld.textures.empty() && !all_textures.empty()) {
      // start of a new level: reset the FIX 34 accumulators so boot/common
      // uploads don't pollute this level's numbers
      g_tex_upload_ms = 0.0;
      g_tex_mipgen_ms = 0.0;
      g_tex_uploaded = 0;
    }
    int bytes_this_run = 0;
    int tex_this_run = 0;
    if (ld.textures.size() < all_textures.size()) {
      std::unique_lock<std::mutex> tpool_lock(data.tex_pool->mutex());
      while (ld.textures.size() < all_textures.size()) {
        const tfrag3::Texture& tex = all_textures[ld.textures.size()];
        check_tex_invariant(tex);
        ld.textures.push_back(add_texture(*data.tex_pool, tex, false));
        bytes_this_run += tex.w * tex.h * 4;
        tex_this_run++;
        if (tex_this_run > 20) {
          break;
        }
        if (bytes_this_run > MAX_TEX_BYTES_PER_FRAME || timer.getMs() > LOAD_BUDGET) {
          break;
        }
      }
    }
    const bool finished = ld.textures.size() == all_textures.size();
    if (finished && !all_textures.empty() && g_tex_uploaded > 0 && !m_logged_stats) {
      // FIX 34: where did the texture staging time actually go?
      fmt::print("[loader] tex stage: {} textures, upload {:.1f}ms, mipgen {:.1f}ms\n",
                 g_tex_uploaded, g_tex_upload_ms, g_tex_mipgen_ms);
      m_logged_stats = true;
    }
    return finished;
#else
    while (ld.textures.size() < all_textures.size()) {
      const tfrag3::Texture& tex = all_textures[ld.textures.size()];
      check_tex_invariant(tex);
      if (!m_cur_allocated) {
        // allocate storage + params now; pixels stream in via row bands
        // below (FIX 33: no more atomic full-texture glTexImage2D uploads).
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glGenTextures(1, &m_cur_tex);
        glBindTexture(GL_TEXTURE_2D, m_cur_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        float aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
        m_cur_allocated = true;
        m_cur_row = 0;
      }

      // FIX 33a: never trust w*h more than the data we actually have.
      const int row_bytes = tex.w * 4;
      const int valid_rows =
          row_bytes > 0 ? (int)std::min<u64>((u64)tex.h, tex.data.size() / tex.w) : 0;
      while (m_cur_row < valid_rows) {
        const int rows =
            std::min<int>(valid_rows - m_cur_row, std::max(1, TEX_BAND_BYTES / row_bytes));
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, m_cur_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, m_cur_row, tex.w, rows, GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        tex.data.data() + (size_t)m_cur_row * row_bytes);
        m_cur_row += rows;
        if (timer.getMs() > LOAD_BUDGET) {
          return false;  // out of budget mid-texture; resume next frame
        }
      }
      if (m_cur_row < tex.h) {
        // invariant was violated: the missing rows stay uninitialized, but
        // we must still finish the texture so the stage can move on.
        m_cur_row = tex.h;
      }

      // base level complete -> build the mipmap chain once.
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_cur_tex);
      glGenerateMipmap(GL_TEXTURE_2D);
      if (tex.load_to_pool) {
        TextureInput in;
        in.debug_page_name = tex.debug_tpage_name;
        in.debug_name = tex.debug_name;
        in.w = tex.w;
        in.h = tex.h;
        in.gpu_texture = m_cur_tex;
        in.common = false;
        in.id = PcTextureId::from_combo_id(tex.combo_id);
        in.src_data = (const u8*)tex.data.data();
        std::unique_lock<std::mutex> tpool_lock(data.tex_pool->mutex());
        data.tex_pool->give_texture(in);
      }
      ld.textures.push_back(m_cur_tex);
      m_cur_tex = 0;
      m_cur_allocated = false;
      if (timer.getMs() > LOAD_BUDGET && ld.textures.size() < all_textures.size()) {
        return false;
      }
    }
    return true;
#endif
  }

  void reset() override {
    if (m_cur_allocated && m_cur_tex != 0) {
      // defensive: the stage should never be reset mid-texture, but if it
      // ever happens, don't leak the half-uploaded texture.
      glDeleteTextures(1, &m_cur_tex);
    }
    m_cur_tex = 0;
    m_cur_allocated = false;
    m_cur_row = 0;
    m_logged_stats = false;
  }

 private:
  GLuint m_cur_tex = 0;
  bool m_cur_allocated = false;
  int m_cur_row = 0;
  bool m_logged_stats = false;
};

class TfragLoadStage : public LoaderStage {
 public:
  TfragLoadStage() : LoaderStage("tfrag") {}
  bool run(Timer& timer, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (data.lev_data->level->tfrag_trees.front().empty()) {
      m_done = true;
      return true;
    }

    if (!m_opengl_created) {
      for (int geo = 0; geo < tfrag3::TFRAG_GEOS; geo++) {
        auto& in_trees = data.lev_data->level->tfrag_trees[geo];
        for (auto& in_tree : in_trees) {
          // FIX 33: pooled buffers (see GpuBufferPool.h).
          GLuint& tree_out = data.lev_data->tfrag_vertex_data[geo].emplace_back();
          tree_out = data.buffers->acquire(
              GL_ARRAY_BUFFER,
              (GLsizeiptr)in_tree.unpacked.vertices.size() * sizeof(tfrag3::PreloadedVertex));
        }
      }
      m_opengl_created = true;
      return false;
    }

    constexpr u32 CHUNK_SIZE = STAGE_VERT_CHUNK;
    u32 uploaded_bytes = 0;
    [[maybe_unused]] u32 unique_buffers = 0;

    while (true) {
      bool complete_tree;

      if (data.lev_data->level->tfrag_trees[m_next_geo].empty()) {
        complete_tree = true;
      } else {
        const auto& tree = data.lev_data->level->tfrag_trees[m_next_geo][m_next_tree];
        u32 end_vert_in_tree = tree.unpacked.vertices.size();
        // the number of vertices we'd need to finish the tree right now
        size_t num_verts_left_in_tree = end_vert_in_tree - m_next_vert;
        size_t start_vert_for_chunk;
        size_t end_vert_for_chunk;

        if (num_verts_left_in_tree > CHUNK_SIZE) {
          complete_tree = false;
          // should only do partial
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = start_vert_for_chunk + CHUNK_SIZE;
          m_next_vert += CHUNK_SIZE;
        } else {
          // should do all!
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = end_vert_in_tree;
          complete_tree = true;
        }

        glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->tfrag_vertex_data[m_next_geo][m_next_tree]);
        u32 upload_size =
            (end_vert_for_chunk - start_vert_for_chunk) * sizeof(tfrag3::PreloadedVertex);
        glBufferSubData(GL_ARRAY_BUFFER, start_vert_for_chunk * sizeof(tfrag3::PreloadedVertex),
                        upload_size, tree.unpacked.vertices.data() + start_vert_for_chunk);
        uploaded_bytes += upload_size;
      }

      if (complete_tree) {
        unique_buffers++;
        // and move on to next tree
        m_next_vert = 0;
        m_next_tree++;
        if (m_next_tree >= data.lev_data->level->tfrag_trees[m_next_geo].size()) {
          m_next_tree = 0;
          m_next_geo++;
          if (m_next_geo >= tfrag3::TFRAG_GEOS) {
            m_next_tree = true;
            m_next_tree = 0;
            m_next_geo = 0;
            m_next_vert = 0;
            m_done = true;
            return true;
          }
        }
        return false;
      }

      if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
        return false;
      }
    }
  }

  void reset() override {
    m_done = false;
    m_opengl_created = false;
    m_next_geo = 0;
    m_next_tree = 0;
    m_next_vert = 0;
  }

 private:
  bool m_done = false;
  bool m_opengl_created = false;
  u32 m_next_geo = 0;
  u32 m_next_tree = 0;
  u32 m_next_vert = 0;
};

class ShrubLoadStage : public LoaderStage {
 public:
  ShrubLoadStage() : LoaderStage("shrub") {}
  bool run(Timer& timer, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (data.lev_data->level->shrub_trees.empty()) {
      m_done = true;
      return true;
    }

    if (!m_opengl_created) {
      for (auto& in_tree : data.lev_data->level->shrub_trees) {
        // FIX 33: pooled buffers (see GpuBufferPool.h).
        GLuint& tree_out = data.lev_data->shrub_vertex_data.emplace_back();
        tree_out = data.buffers->acquire(
            GL_ARRAY_BUFFER,
            (GLsizeiptr)in_tree.unpacked.vertices.size() * sizeof(tfrag3::ShrubGpuVertex));
      }
      m_opengl_created = true;
      return false;
    }

    constexpr u32 CHUNK_SIZE = STAGE_VERT_CHUNK;
    u32 uploaded_bytes = 0;

    while (true) {
      const auto& tree = data.lev_data->level->shrub_trees[m_next_tree];
      u32 end_vert_in_tree = tree.unpacked.vertices.size();
      // the number of vertices we'd need to finish the tree right now
      size_t num_verts_left_in_tree = end_vert_in_tree - m_next_vert;
      size_t start_vert_for_chunk;
      size_t end_vert_for_chunk;

      bool complete_tree;

      if (num_verts_left_in_tree > CHUNK_SIZE) {
        complete_tree = false;
        // should only do partial
        start_vert_for_chunk = m_next_vert;
        end_vert_for_chunk = start_vert_for_chunk + CHUNK_SIZE;
        m_next_vert += CHUNK_SIZE;
      } else {
        // should do all!
        start_vert_for_chunk = m_next_vert;
        end_vert_for_chunk = end_vert_in_tree;
        complete_tree = true;
      }

      glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->shrub_vertex_data[m_next_tree]);
      u32 upload_size =
          (end_vert_for_chunk - start_vert_for_chunk) * sizeof(tfrag3::ShrubGpuVertex);
      glBufferSubData(GL_ARRAY_BUFFER, start_vert_for_chunk * sizeof(tfrag3::ShrubGpuVertex),
                      upload_size, tree.unpacked.vertices.data() + start_vert_for_chunk);
      uploaded_bytes += upload_size;

      if (complete_tree) {
        // and move on to next tree
        m_next_vert = 0;
        m_next_tree++;
        if (m_next_tree >= data.lev_data->level->shrub_trees.size()) {
          m_done = true;
          return true;
        }
      }

      if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 128) > 2048) {
        return false;
      }
    }
  }

  void reset() override {
    m_done = false;
    m_opengl_created = false;
    m_next_tree = 0;
    m_next_vert = 0;
  }

 private:
  bool m_done = false;
  bool m_opengl_created = false;
  u32 m_next_tree = 0;
  u32 m_next_vert = 0;
};

class TieLoadStage : public LoaderStage {
 public:
  TieLoadStage() : LoaderStage("tie") {}
  bool run(Timer& timer, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (data.lev_data->level->tie_trees.front().empty()) {
      m_done = true;
      return true;
    }

    if (!m_opengl_created) {
      auto evt = scoped_prof("tie-opengl-create");
      for (int geo = 0; geo < tfrag3::TIE_GEOS; geo++) {
        auto& in_trees = data.lev_data->level->tie_trees[geo];
        for (auto& in_tree : in_trees) {
          // FIX 33: pooled buffers (see GpuBufferPool.h).
          LevelData::TieOpenGL& tree_out = data.lev_data->tie_data[geo].emplace_back();
          tree_out.vertex_buffer = data.buffers->acquire(
              GL_ARRAY_BUFFER,
              (GLsizeiptr)in_tree.unpacked.vertices.size() * sizeof(tfrag3::PreloadedVertex));
          tree_out.index_buffer = data.buffers->acquire(
              GL_ELEMENT_ARRAY_BUFFER,
              (GLsizeiptr)in_tree.unpacked.indices.size() * sizeof(u32));
        }
      }
      m_opengl_created = true;
      return false;
    }

    if (!m_verts_done) {
      auto evt = scoped_prof("tie-verts");
      constexpr u32 CHUNK_SIZE = STAGE_VERT_CHUNK;
      u32 uploaded_bytes = 0;

      while (true) {
        const auto& tree = data.lev_data->level->tie_trees[m_next_geo][m_next_tree];
        u32 end_vert_in_tree = tree.unpacked.vertices.size();
        // the number of vertices we'd need to finish the tree right now
        size_t num_verts_left_in_tree = end_vert_in_tree - m_next_vert;
        size_t start_vert_for_chunk;
        size_t end_vert_for_chunk;

        bool complete_tree;

        if (num_verts_left_in_tree > CHUNK_SIZE) {
          complete_tree = false;
          // should only do partial
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = start_vert_for_chunk + CHUNK_SIZE;
          m_next_vert += CHUNK_SIZE;
        } else {
          // should do all!
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = end_vert_in_tree;
          complete_tree = true;
        }

        glBindBuffer(GL_ARRAY_BUFFER,
                     data.lev_data->tie_data[m_next_geo][m_next_tree].vertex_buffer);
        u32 upload_size =
            (end_vert_for_chunk - start_vert_for_chunk) * sizeof(tfrag3::PreloadedVertex);
        {
          auto bsd = scoped_prof(fmt::format("buffer-{}k", upload_size / 1024).c_str());
          glBufferSubData(GL_ARRAY_BUFFER, start_vert_for_chunk * sizeof(tfrag3::PreloadedVertex),
                          upload_size, tree.unpacked.vertices.data() + start_vert_for_chunk);
        }

        uploaded_bytes += upload_size;

        if (complete_tree) {
          // and move on to next tree
          m_next_vert = 0;
          m_next_tree++;
          if (m_next_tree >= data.lev_data->level->tie_trees[m_next_geo].size()) {
            m_next_tree = 0;
            m_next_geo++;
            while (m_next_geo < tfrag3::TIE_GEOS &&
                   data.lev_data->level->tie_trees[m_next_geo].empty()) {
              m_next_geo++;
            }
            if (m_next_geo >= tfrag3::TIE_GEOS) {
              m_verts_done = true;
              m_next_tree = 0;
              m_next_geo = 0;
              m_next_vert = 0;
              return false;
            }
          }
        }

        if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
          return false;
        }
      }
    }

    if (!m_wind_indices_done) {
      auto evt = scoped_prof("tie-wind");
      bool abort = false;
      for (; m_next_geo < tfrag3::TIE_GEOS; m_next_geo++) {
        auto& geo_trees = data.lev_data->level->tie_trees[m_next_geo];
        for (; m_next_tree < geo_trees.size(); m_next_tree++) {
          if (abort) {
            return false;
          }
          auto& in_tree = geo_trees[m_next_tree];
          auto& out_tree = data.lev_data->tie_data[m_next_geo][m_next_tree];
          size_t wind_idx_buffer_len = 0;
          for (auto& draw : in_tree.instanced_wind_draws) {
            wind_idx_buffer_len += draw.vertex_index_stream.size();
          }
          if (wind_idx_buffer_len > 0) {
            out_tree.has_wind = true;
            out_tree.wind_indices = data.buffers->acquire(
                GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)wind_idx_buffer_len * sizeof(u32));
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out_tree.wind_indices);
            std::vector<u32> temp;
            temp.resize(wind_idx_buffer_len);
            u32 off = 0;
            for (auto& draw : in_tree.instanced_wind_draws) {
              memcpy(temp.data() + off, draw.vertex_index_stream.data(),
                     draw.vertex_index_stream.size() * sizeof(u32));
              off += draw.vertex_index_stream.size();
            }

            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, wind_idx_buffer_len * sizeof(u32),
                            temp.data());
            abort = true;
          }
        }
        m_next_tree = 0;
      }

      m_wind_indices_done = true;
      m_next_geo = 0;
      m_next_vert = 0;
      m_next_tree = 0;

      if (timer.getMs() > LOAD_BUDGET) {
        return false;
      }
    }

    if (!m_indices_done) {
      auto evt = scoped_prof("tie-ind");
      constexpr u32 CHUNK_SIZE = STAGE_INDEX_CHUNK;
      u32 uploaded_bytes = 0;

      while (true) {
        const auto& tree = data.lev_data->level->tie_trees[m_next_geo][m_next_tree];
        u32 end_ind_in_tree = tree.unpacked.indices.size();
        // the number of indices we'd need to finish the tree right now
        size_t num_inds_left_in_tree = end_ind_in_tree - m_next_vert;
        size_t start_ind_for_chunk;
        size_t end_ind_for_chunk;

        bool complete_tree;

        if (num_inds_left_in_tree > CHUNK_SIZE) {
          complete_tree = false;
          // should only do partial
          start_ind_for_chunk = m_next_vert;
          end_ind_for_chunk = start_ind_for_chunk + CHUNK_SIZE;
          m_next_vert += CHUNK_SIZE;
        } else {
          // should do all!
          start_ind_for_chunk = m_next_vert;
          end_ind_for_chunk = end_ind_in_tree;
          complete_tree = true;
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                     data.lev_data->tie_data[m_next_geo][m_next_tree].index_buffer);
        u32 upload_size = (end_ind_for_chunk - start_ind_for_chunk) * sizeof(u32);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, start_ind_for_chunk * sizeof(u32), upload_size,
                        tree.unpacked.indices.data() + start_ind_for_chunk);
        uploaded_bytes += upload_size;

        if (complete_tree) {
          // and move on to next tree
          m_next_vert = 0;
          m_next_tree++;
          if (m_next_tree >= data.lev_data->level->tie_trees[m_next_geo].size()) {
            m_next_tree = 0;
            m_next_geo++;
            while (m_next_geo < tfrag3::TIE_GEOS &&
                   data.lev_data->level->tie_trees[m_next_geo].empty()) {
              m_next_geo++;
            }
            if (m_next_geo >= tfrag3::TIE_GEOS) {
              m_indices_done = true;
              m_next_tree = 0;
              m_next_geo = 0;
              m_next_vert = 0;
              m_done = true;
              return true;
            }
          }
        }

        if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
          return false;
        }
      }
    }

    return false;
  }

  void reset() override {
    m_done = false;
    m_opengl_created = false;
    m_next_geo = 0;
    m_next_tree = 0;
    m_next_vert = 0;
    m_verts_done = false;
    m_indices_done = false;
    m_wind_indices_done = false;
  }

 private:
  bool m_done = false;
  bool m_opengl_created = false;
  bool m_verts_done = false;
  bool m_indices_done = false;
  bool m_wind_indices_done = false;
  u32 m_next_geo = 0;
  u32 m_next_tree = 0;
  u32 m_next_vert = 0;
};

class CollideLoaderStage : public LoaderStage {
 public:
  CollideLoaderStage() : LoaderStage("collide") {}
  bool run(Timer& /*timer*/, LoaderInput& data) override {
    if (m_done) {
      return true;
    }
    if (!m_opengl_created) {
      // FIX 33: pooled buffers (see GpuBufferPool.h).
      data.lev_data->collide_vertices = data.buffers->acquire(
          GL_ARRAY_BUFFER, (GLsizeiptr)data.lev_data->level->collision.vertices.size() *
                               sizeof(tfrag3::CollisionMesh::Vertex));
      m_opengl_created = true;
      return false;
    }

    u32 start = m_vtx;
    u32 end =
        std::min((u32)data.lev_data->level->collision.vertices.size(), start + STAGE_VERT_CHUNK);
    glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->collide_vertices);
    glBufferSubData(GL_ARRAY_BUFFER, start * sizeof(tfrag3::CollisionMesh::Vertex),
                    (end - start) * sizeof(tfrag3::CollisionMesh::Vertex),
                    data.lev_data->level->collision.vertices.data() + start);
    m_vtx = end;

    if (m_vtx == data.lev_data->level->collision.vertices.size()) {
      m_done = true;
      return true;
    } else {
      return false;
    }
  }
  void reset() override {
    m_opengl_created = false;
    m_vtx = 0;
    m_done = false;
  }

 private:
  bool m_opengl_created = false;
  u32 m_vtx = 0;
  bool m_done = false;
};

class StallLoaderStage : public LoaderStage {
 public:
  StallLoaderStage() : LoaderStage("stall") {}
  bool run(Timer&, LoaderInput& /*data*/) override {
    m_count++;
    if (m_count > 10) {
      return true;
    }
    return false;
  }

  void reset() override { m_count = 0; }

 private:
  int m_count = 0;
};

class HfragLoaderStage : public LoaderStage {
 public:
  HfragLoaderStage() : LoaderStage("hfrag") {}
  void reset() override {
    m_done = false;
    m_opengl = false;
    m_vtx_uploaded = false;
    m_idx = 0;
  }

  bool run(Timer&, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (!m_opengl) {
      // FIX 33: pooled buffers (see GpuBufferPool.h).
      data.lev_data->hfrag_indices = data.buffers->acquire(
          GL_ELEMENT_ARRAY_BUFFER,
          (GLsizeiptr)data.lev_data->level->hfrag.indices.size() * sizeof(u32));

      data.lev_data->hfrag_vertices = data.buffers->acquire(
          GL_ARRAY_BUFFER, (GLsizeiptr)data.lev_data->level->hfrag.vertices.size() *
                               sizeof(tfrag3::HfragmentVertex));
      m_opengl = true;
    }

    if (!m_vtx_uploaded) {
      u32 start = m_idx;
      m_idx = std::min(start + STAGE_VERT_CHUNK,
                      (u32)data.lev_data->level->hfrag.indices.size());
      glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->hfrag_indices);
      glBufferSubData(GL_ARRAY_BUFFER, start * sizeof(u32), (m_idx - start) * sizeof(u32),
                      data.lev_data->level->hfrag.indices.data() + start);
      if (m_idx != data.lev_data->level->hfrag.indices.size()) {
        return false;
      } else {
        m_idx = 0;
        m_vtx_uploaded = true;
      }
    }

    u32 start = m_idx;
    m_idx = std::min(start + STAGE_VERT_CHUNK, (u32)data.lev_data->level->hfrag.vertices.size());
    glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->hfrag_vertices);
    glBufferSubData(GL_ARRAY_BUFFER, start * sizeof(tfrag3::HfragmentVertex),
                    (m_idx - start) * sizeof(tfrag3::HfragmentVertex),
                    data.lev_data->level->hfrag.vertices.data() + start);

    if (m_idx != data.lev_data->level->hfrag.vertices.size()) {
      return false;
    } else {
      m_done = true;
      return true;
    }
    return true;
  }

 private:
  bool m_done = false;
  bool m_opengl = false;
  bool m_vtx_uploaded = false;
  u32 m_idx = 0;
};

MercLoaderStage::MercLoaderStage() : LoaderStage("merc") {}
void MercLoaderStage::reset() {
  m_done = false;
  m_opengl = false;
  m_vtx_uploaded = false;
  m_idx = 0;
}

bool MercLoaderStage::run(Timer& /*timer*/, LoaderInput& data) {
  if (m_done) {
    return true;
  }

  if (!m_opengl) {
    // FIX 33: pooled buffers (see GpuBufferPool.h).
    data.lev_data->merc_indices = data.buffers->acquire(
        GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)data.lev_data->level->merc_data.indices.size() * sizeof(u32));

    data.lev_data->merc_vertices = data.buffers->acquire(
        GL_ARRAY_BUFFER,
        (GLsizeiptr)data.lev_data->level->merc_data.vertices.size() * sizeof(tfrag3::MercVertex));
    m_opengl = true;
  }

  if (!m_vtx_uploaded) {
    u32 start = m_idx;
    m_idx = std::min(start + STAGE_VERT_CHUNK,
                    (u32)data.lev_data->level->merc_data.indices.size());
    glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->merc_indices);
    glBufferSubData(GL_ARRAY_BUFFER, start * sizeof(u32), (m_idx - start) * sizeof(u32),
                    data.lev_data->level->merc_data.indices.data() + start);
    if (m_idx != data.lev_data->level->merc_data.indices.size()) {
      return false;
    } else {
      m_idx = 0;
      m_vtx_uploaded = true;
    }
  }

  u32 start = m_idx;
  m_idx = std::min(start + STAGE_VERT_CHUNK, (u32)data.lev_data->level->merc_data.vertices.size());
  glBindBuffer(GL_ARRAY_BUFFER, data.lev_data->merc_vertices);
  glBufferSubData(GL_ARRAY_BUFFER, start * sizeof(tfrag3::MercVertex),
                  (m_idx - start) * sizeof(tfrag3::MercVertex),
                  data.lev_data->level->merc_data.vertices.data() + start);

  if (m_idx != data.lev_data->level->merc_data.vertices.size()) {
    return false;
  } else {
    m_done = true;
    for (auto& model : data.lev_data->level->merc_data.models) {
      data.lev_data->merc_model_lookup[model.name] = &model;
      (*data.mercs)[model.name].push_back({&model, data.lev_data->load_id, data.lev_data});
    }
    return true;
  }
  return true;
}

std::vector<std::unique_ptr<LoaderStage>> make_loader_stages() {
  std::vector<std::unique_ptr<LoaderStage>> ret;
  ret.push_back(std::make_unique<TieLoadStage>());
  ret.push_back(std::make_unique<TextureLoaderStage>());
  ret.push_back(std::make_unique<TfragLoadStage>());
  ret.push_back(std::make_unique<ShrubLoadStage>());
  ret.push_back(std::make_unique<CollideLoaderStage>());
  ret.push_back(std::make_unique<MercLoaderStage>());
  ret.push_back(std::make_unique<HfragLoaderStage>());
  ret.push_back(std::make_unique<StallLoaderStage>());
  return ret;
}
