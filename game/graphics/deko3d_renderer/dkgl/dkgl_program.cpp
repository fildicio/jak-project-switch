// T105 (AI-assisted): dkgl programs + uniforms.
// Program identity comes from the brief-sanctioned NAME HINT (set just before shader
// creation by the GL Shader/ShaderLibrary wrappers) -- the GLSL source itself is never
// needed because the .dksh binaries come from the T103 generated set, selected by name
// at draw time. Uniforms write a CPU shadow laid out like ONE std140 UBO slice
// (vert block at 0, frag block at vert.block_size) that T106 uploads verbatim.
#include <cstdio>
#include <cstring>

#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

namespace dkgl {
namespace {

constexpr u32 kMaxShaders = 128;
constexpr u32 kMaxPrograms = 128;
constexpr u32 kUniformShadowBytes = 256;

struct ShaderObj {
  bool live = false;
  u32 stage = 0;  // gl::VERTEX_SHADER / gl::FRAGMENT_SHADER
  char name[64] = {};
};
struct ProgramObj {
  bool live = false;
  char name[64] = {};
  const ProgramUniformTable* table = nullptr;  // null = inert (name not in T103 set)
  u32 vsh = 0, fsh = 0;
  u8 uniform_shadow[kUniformShadowBytes] = {};
  bool uniform_dirty = false;
  bool ever_used = false;
};

ShaderObj g_shaders[kMaxShaders];
ProgramObj g_programs[kMaxPrograms];
u32 g_next_shader = 0, g_next_program = 0;
char g_pending_hint[64] = {};
bool g_logged_inert = false;    // one-time log for "name not in table"
bool g_logged_sampler = false;  // one-time log for ignored sampler uniforms

ShaderObj* shader(u32 id) {
  if (id == 0 || id > kMaxShaders) {
    return nullptr;
  }
  ShaderObj* s = &g_shaders[id - 1];
  return s->live ? s : nullptr;
}
ProgramObj* program(u32 id) {
  if (id == 0 || id > kMaxPrograms) {
    return nullptr;
  }
  ProgramObj* p = &g_programs[id - 1];
  return p->live ? p : nullptr;
}

// Write `bytes` into the combined shadow at EVERY stage slot carrying the same NAME
// (GL exposes one location per name; if a uniform is in both stages, one glUniform*
// updates both copies). vert slot -> offset, frag slot -> vert.block_size + offset.
void write_uniform(int location, const void* bytes, u32 n) {
  ProgramObj* p = program(bound_program());
  if (!p || !p->table || location < 0) {
    return;
  }
  if ((u32)location & 0x8000'0000u) {
    // sampler pseudo-location (tex_T<digit>): binding is explicit in the shaders
    if (!g_logged_sampler) {
      std::fprintf(stderr, "[dkgl] sampler uniform ignored (binding is explicit)\n");
      g_logged_sampler = true;
    }
    return;
  }
  const StageUniformTable& vt = p->table->vert;
  const StageUniformTable& ft = p->table->frag;
  const u32 loc = (u32)location;
  const UniformSlot* hit = nullptr;
  if (loc < vt.count) {
    hit = &vt.slots[loc];
  } else if (loc - vt.count < ft.count) {
    hit = &ft.slots[loc - vt.count];
  }
  if (!hit || !hit->name) {
    return;
  }
  for (u32 i = 0; i < vt.count; ++i) {
    if (vt.slots[i].name && std::strcmp(vt.slots[i].name, hit->name) == 0) {
      std::memcpy(p->uniform_shadow + vt.slots[i].offset, bytes, n);
    }
  }
  for (u32 i = 0; i < ft.count; ++i) {
    if (ft.slots[i].name && std::strcmp(ft.slots[i].name, hit->name) == 0) {
      std::memcpy(p->uniform_shadow + vt.block_size + ft.slots[i].offset, bytes, n);
    }
  }
  p->uniform_dirty = true;
}

}  // namespace

// ---- shader/program objects -----------------------------------------------------
void hint_program_name(const char* name) {
  if (!name) {
    g_pending_hint[0] = '\0';
    return;
  }
  std::strncpy(g_pending_hint, name, sizeof(g_pending_hint) - 1);
  g_pending_hint[sizeof(g_pending_hint) - 1] = '\0';
}
u32 create_shader(u32 stage) {
  for (u32 attempt = 0; attempt < kMaxShaders; ++attempt) {
    const u32 slot = g_next_shader;
    g_next_shader = (g_next_shader + 1) % kMaxShaders;
    if (!g_shaders[slot].live) {
      g_shaders[slot] = ShaderObj{};
      g_shaders[slot].live = true;
      g_shaders[slot].stage = stage;
      std::strncpy(g_shaders[slot].name, g_pending_hint, sizeof(g_shaders[slot].name) - 1);
      return slot + 1;
    }
  }
  return 0;
}
void shader_source(u32, const char*) {
  // source is not needed: identity comes from the name hint (T103 .dksh by name)
}
void compile_shader(u32) {}
u32 create_program() {
  for (u32 attempt = 0; attempt < kMaxPrograms; ++attempt) {
    const u32 slot = g_next_program;
    g_next_program = (g_next_program + 1) % kMaxPrograms;
    if (!g_programs[slot].live) {
      g_programs[slot] = ProgramObj{};
      g_programs[slot].live = true;
      return slot + 1;
    }
  }
  return 0;
}
void attach_shader(u32 program_id, u32 shader_id) {
  ProgramObj* p = program(program_id);
  ShaderObj* s = shader(shader_id);
  if (!p || !s) {
    return;
  }
  if (s->stage == gl::VERTEX_SHADER) {
    p->vsh = shader_id;
  } else {
    p->fsh = shader_id;
  }
}
void link_program(u32 program_id) {
  ProgramObj* p = program(program_id);
  if (!p) {
    return;
  }
  const ShaderObj* v = shader(p->vsh);
  std::strncpy(p->name, v ? v->name : "", sizeof(p->name) - 1);
  p->name[sizeof(p->name) - 1] = '\0';
  p->table = nullptr;
  for (const ProgramUniformTable& t : kProgramUniformTables) {
    if (std::strcmp(t.program_name, p->name) == 0) {
      p->table = &t;
      break;
    }
  }
  if (!p->table && p->name[0] != '\0' && !g_logged_inert) {
    std::fprintf(stderr, "[dkgl] program '%s' not in table (inert)\n", p->name);
    g_logged_inert = true;
  }
  std::memset(p->uniform_shadow, 0, sizeof(p->uniform_shadow));
  p->uniform_dirty = true;
}
void delete_shader(u32 shader_id) {
  if (ShaderObj* s = shader(shader_id)) {
    *s = ShaderObj{};
  }
}
void get_shaderiv(u32, u32, int* params) {
  if (params) {
    *params = 1;  // GL_TRUE: the GL Shader ctor only reads COMPILE_STATUS
  }
}
void get_programiv(u32, u32, int* params) {
  if (params) {
    *params = 1;  // GL_TRUE: LINK_STATUS
  }
}
void get_shader_info_log(u32, int max, int* len, char* log) {
  if (len) {
    *len = 0;
  }
  if (max > 0 && log) {
    log[0] = '\0';
  }
}

// ---- uniforms -------------------------------------------------------------------
int get_uniform_location(u32 program_id, const char* name) {
  ProgramObj* p = program(program_id);
  if (!p || !p->table || !name) {
    return -1;  // inert programs expose no locations
  }
  // sampler pseudo-location: tex_T<0-7> (unit == binding == digit)
  if (std::strncmp(name, "tex_T", 5) == 0 && name[5] >= '0' && name[5] <= '7' &&
      name[6] == '\0') {
    return (int)(0x8000'0000u | (u32)(name[5] - '0'));
  }
  // combined view: vert slots first, then frag
  const StageUniformTable& vt = p->table->vert;
  const StageUniformTable& ft = p->table->frag;
  for (u32 i = 0; i < vt.count; ++i) {
    if (vt.slots[i].name && std::strcmp(vt.slots[i].name, name) == 0) {
      return (int)i;
    }
  }
  for (u32 i = 0; i < ft.count; ++i) {
    if (ft.slots[i].name && std::strcmp(ft.slots[i].name, name) == 0) {
      return (int)(vt.count + i);
    }
  }
  return -1;
}
void uniform1i(int location, int v) {
  write_uniform(location, &v, 4);
}
void uniform1f(int location, float v) {
  write_uniform(location, &v, 4);
}
void uniform4f(int location, float x, float y, float z, float w) {
  const float v[4] = {x, y, z, w};
  write_uniform(location, v, 16);
}
int get_uniform_block_index(u32, const char*) {
  return -1;
}
void uniform_block_binding(u32, u32, u32) {}
u32 get_error() {
  return gl::NO_ERROR;
}

// ---- T106 accessors -------------------------------------------------------------
const ProgramUniformTable* program_table(u32 program_id) {
  ProgramObj* p = program(program_id);
  return p ? p->table : nullptr;
}
const u8* program_uniform_shadow(u32 program_id) {
  ProgramObj* p = program(program_id);
  return p ? p->uniform_shadow : nullptr;
}
bool program_uniform_dirty(u32 program_id) {
  ProgramObj* p = program(program_id);
  return p && p->uniform_dirty;
}
void program_uniform_clear_dirty(u32 program_id) {
  if (ProgramObj* p = program(program_id)) {
    p->uniform_dirty = false;
  }
}
void mark_program_used(u32 program_id) {
  if (ProgramObj* p = program(program_id)) {
    p->ever_used = true;
  }
}

}  // namespace dkgl
