// T104b (AI-assisted): Switch-only glad override install for the dkgl buffer/VAO/state
// subset. Copies the dk_gl_census.cpp install pattern verbatim (glad_gl* pointer
// assignment through reinterpret_cast) -- but dkgl wraps REAL functions, not no-op
// census stubs. Call order contract: dk_gl_census installs first (boot),
// dkgl::install_glad() runs later (T107 init) and OVERWRITES only this subset.
// Textures/programs overrides land in T105; everything else stays a census stub.
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

#if defined(__SWITCH__)

#include "third-party/glad/include/glad/glad.h"

// run_log.h cannot be included next to <switch.h> (u128 collision); same hand
// declaration as dk_gl_census.cpp / dk_texture_gpu.cpp.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkgl {
namespace {

void GLAPIENTRY dkgl_glGenBuffers(GLsizei n, GLuint* buffers) { gen_buffers(n, buffers); }
void GLAPIENTRY dkgl_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
  delete_buffers(n, buffers);
}
void GLAPIENTRY dkgl_glBindBuffer(GLenum target, GLuint buffer) { bind_buffer(target, buffer); }
void GLAPIENTRY dkgl_glBufferData(GLenum target, GLsizeiptr size, const void* data,
                                  GLenum usage) {
  buffer_data(target, (u64)size, data, usage);
}
void GLAPIENTRY dkgl_glGenVertexArrays(GLsizei n, GLuint* arrays) { gen_vertex_arrays(n, arrays); }
void GLAPIENTRY dkgl_glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
  delete_vertex_arrays(n, arrays);
}
void GLAPIENTRY dkgl_glBindVertexArray(GLuint array) { bind_vertex_array(array); }
void GLAPIENTRY dkgl_glEnableVertexAttribArray(GLuint index) {
  enable_vertex_attrib_array(index);
}
void GLAPIENTRY dkgl_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                           GLboolean normalized, GLsizei stride,
                                           const void* pointer) {
  vertex_attrib_pointer(index, size, type, normalized != GL_FALSE, stride, pointer);
}
void GLAPIENTRY dkgl_glVertexAttribIPointer(GLuint index, GLint size, GLenum type,
                                            GLsizei stride, const void* pointer) {
  vertex_attrib_i_pointer(index, size, type, stride, pointer);
}
// dkgl review: FramebufferTexturePairContext saves + restores GL_VIEWPORT and
// GL_FRAMEBUFFER_BINDING; the census no-op left them uninitialized.
void GLAPIENTRY dkgl_glGetIntegerv(GLenum pname, GLint* data) { get_integerv(pname, data); }
// T111: Generic2
void GLAPIENTRY dkgl_glUniform1ui(GLint loc, GLuint v) { uniform1ui(loc, v); }
void GLAPIENTRY dkgl_glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) {
  uniform3f(loc, x, y, z);
}
void GLAPIENTRY dkgl_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose,
                                        const GLfloat* value) {
  uniform_matrix4fv(loc, count, transpose != GL_FALSE, value);
}
void GLAPIENTRY dkgl_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  blend_color(r, g, b, a);
}
void GLAPIENTRY dkgl_glEnable(GLenum cap) { enable((u32)cap); }
void GLAPIENTRY dkgl_glDisable(GLenum cap) { disable((u32)cap); }
GLboolean GLAPIENTRY dkgl_glIsEnabled(GLenum cap) {
  return is_enabled((u32)cap) ? GL_TRUE : GL_FALSE;
}
void GLAPIENTRY dkgl_glBlendFunc(GLenum sfactor, GLenum dfactor) {
  blend_func(sfactor, dfactor);
}
void GLAPIENTRY dkgl_glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB,
                                         GLenum sfactorAlpha, GLenum dfactorAlpha) {
  blend_func_separate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
}
void GLAPIENTRY dkgl_glBlendEquation(GLenum mode) { blend_equation(mode); }
void GLAPIENTRY dkgl_glDepthMask(GLboolean flag) { depth_mask(flag != GL_FALSE); }
void GLAPIENTRY dkgl_glDepthFunc(GLenum func) { depth_func(func); }
void GLAPIENTRY dkgl_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  viewport(x, y, width, height);
}
void GLAPIENTRY dkgl_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
  clear_color(red, green, blue, alpha);
}
void GLAPIENTRY dkgl_glClear(GLbitfield mask) { clear((u32)mask); }
void GLAPIENTRY dkgl_glPrimitiveRestartIndex(GLuint index) { primitive_restart_index(index); }
void GLAPIENTRY dkgl_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  draw_arrays(mode, first, count);
}
void GLAPIENTRY dkgl_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                    const void* indices) {
  draw_elements(mode, count, type, indices);
}

// ---- T105: textures / FBO / programs ------------------------------------------
void GLAPIENTRY dkgl_glGenTextures(GLsizei n, GLuint* textures) { gen_textures(n, textures); }
void GLAPIENTRY dkgl_glDeleteTextures(GLsizei n, const GLuint* textures) {
  delete_textures(n, textures);
}
void GLAPIENTRY dkgl_glActiveTexture(GLenum texture) { active_texture((u32)texture); }
void GLAPIENTRY dkgl_glBindTexture(GLenum target, GLuint texture) {
  (void)target;  // TEXTURE_2D only, in the ocean
  bind_texture(texture);
}
void GLAPIENTRY dkgl_glTexParameteri(GLenum target, GLenum pname, GLint param) {
  (void)target;
  tex_parameteri(pname, param);
}
void GLAPIENTRY dkgl_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                                  GLsizei width, GLsizei height, GLint border,
                                  GLenum format, GLenum type, const void* pixels) {
  (void)target;
  (void)border;
  tex_image_2d(level, internalformat, width, height, format, type, pixels);  // T112: level
}
void GLAPIENTRY dkgl_glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
  gen_framebuffers(n, framebuffers);
}
void GLAPIENTRY dkgl_glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
  delete_framebuffers(n, framebuffers);
}
void GLAPIENTRY dkgl_glBindFramebuffer(GLenum target, GLuint framebuffer) {
  bind_framebuffer_target(target, framebuffer);  // T112: DepthCue binds READ/DRAW apart
}
// T112 (AI-assisted): DepthCue copies the frame into its sample texture
void GLAPIENTRY dkgl_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0,
                                       GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                                       GLenum filter) {
  blit_framebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
void GLAPIENTRY dkgl_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                            GLenum textarget, GLuint texture, GLint level) {
  (void)textarget;  // TEXTURE_2D only
  framebuffer_texture_2d(target, attachment, texture, level);
}
void GLAPIENTRY dkgl_glDrawBuffers(GLsizei n, const GLenum* bufs) {
  draw_buffers(n, bufs);
}
GLenum GLAPIENTRY dkgl_glCheckFramebufferStatus(GLenum target) {
  (void)target;
  return check_framebuffer_status();
}
GLuint GLAPIENTRY dkgl_glCreateShader(GLenum type) { return create_shader(type); }
void GLAPIENTRY dkgl_glShaderSource(GLuint shader, GLsizei count,
                                    const GLchar* const* string, const GLint* length) {
  (void)count;
  (void)length;  // lengths ignored (nullptr in the ocean): the source is unused
  shader_source(shader, string ? string[0] : nullptr);
}
void GLAPIENTRY dkgl_glCompileShader(GLuint shader) { compile_shader(shader); }
GLuint GLAPIENTRY dkgl_glCreateProgram() { return create_program(); }
void GLAPIENTRY dkgl_glAttachShader(GLuint program, GLuint shader) {
  attach_shader(program, shader);
}
void GLAPIENTRY dkgl_glLinkProgram(GLuint program) { link_program(program); }
void GLAPIENTRY dkgl_glDeleteShader(GLuint shader) { delete_shader(shader); }
void GLAPIENTRY dkgl_glUseProgram(GLuint program) { use_program(program); }
void GLAPIENTRY dkgl_glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
  get_shaderiv(shader, pname, params);
}
void GLAPIENTRY dkgl_glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
  get_programiv(program, pname, params);
}
void GLAPIENTRY dkgl_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length,
                                        GLchar* infoLog) {
  get_shader_info_log(shader, bufSize, length, infoLog);
}
GLint GLAPIENTRY dkgl_glGetUniformLocation(GLuint program, const GLchar* name) {
  return get_uniform_location(program, name);
}
void GLAPIENTRY dkgl_glUniform1i(GLint location, GLint v) { uniform1i(location, v); }
void GLAPIENTRY dkgl_glUniform1f(GLint location, GLfloat v) { uniform1f(location, v); }
void GLAPIENTRY dkgl_glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2,
                                 GLfloat v3) {
  uniform4f(location, v0, v1, v2, v3);
}
GLuint GLAPIENTRY dkgl_glGetUniformBlockIndex(GLuint program, const GLchar* name) {
  return get_uniform_block_index(program, name);
}
void GLAPIENTRY dkgl_glUniformBlockBinding(GLuint program, GLuint block, GLuint binding) {
  uniform_block_binding(program, block, binding);
}
GLenum GLAPIENTRY dkgl_glGetError() { return get_error(); }
// T112/T113 (AI-assisted): DepthCue u_color; ShadowRenderer stencil volumes + colour mask
void GLAPIENTRY dkgl_glUniform4fv(GLint location, GLsizei count, const GLfloat* v) {
  uniform4fv(location, count, v);
}
void GLAPIENTRY dkgl_glStencilFunc(GLenum func, GLint ref, GLuint mask) {
  stencil_func(func, ref, mask);
}
void GLAPIENTRY dkgl_glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) {
  stencil_op(sfail, dpfail, dppass);
}
void GLAPIENTRY dkgl_glStencilMask(GLuint mask) { stencil_mask(mask); }
void GLAPIENTRY dkgl_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  color_mask(r != GL_FALSE, g != GL_FALSE, b != GL_FALSE, a != GL_FALSE);
}
// Far ocean (GL DirectRenderer, AI-assisted). Its Switch ring upload maps the buffer and
// fences the segments. dkgl renames every upload, so nothing is ever overwritten while the
// GPU reads it: no map (-> glBufferSubData fallback), no fences, nothing to wait for.
void GLAPIENTRY dkgl_glColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b,
                                  GLboolean a) {
  if (buf == 0) {
    color_mask(r != GL_FALSE, g != GL_FALSE, b != GL_FALSE, a != GL_FALSE);
  }
}
void GLAPIENTRY dkgl_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                     const void* data) {
  buffer_sub_data(target, (u64)offset, (u64)size, data);
}
// TODO #5 (AI-assisted): Merc2 binds its bone matrices per draw
void GLAPIENTRY dkgl_glBindBufferRange(GLenum target, GLuint index, GLuint buffer,
                                       GLintptr offset, GLsizeiptr size) {
  bind_buffer_range(target, index, buffer, (u64)offset, (u64)size);
}
void GLAPIENTRY dkgl_glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
  bind_buffer_range(target, index, buffer, 0, DK_UNIFORM_BUF_MAX_SIZE);
}
void* GLAPIENTRY dkgl_glMapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield) {
  return nullptr;
}
GLboolean GLAPIENTRY dkgl_glUnmapBuffer(GLenum) { return GL_TRUE; }
GLsync GLAPIENTRY dkgl_glFenceSync(GLenum, GLbitfield) { return nullptr; }
GLenum GLAPIENTRY dkgl_glClientWaitSync(GLsync, GLbitfield, GLuint64) {
  return GL_ALREADY_SIGNALED;
}
void GLAPIENTRY dkgl_glDeleteSync(GLsync) {}
void GLAPIENTRY dkgl_glFinish() {}

}  // namespace

void install_glad() {
  glad_glGenBuffers =
      reinterpret_cast<PFNGLGENBUFFERSPROC>(reinterpret_cast<void*>(&dkgl_glGenBuffers));
  glad_glDeleteBuffers =
      reinterpret_cast<PFNGLDELETEBUFFERSPROC>(reinterpret_cast<void*>(&dkgl_glDeleteBuffers));
  glad_glBindBuffer =
      reinterpret_cast<PFNGLBINDBUFFERPROC>(reinterpret_cast<void*>(&dkgl_glBindBuffer));
  glad_glBufferData =
      reinterpret_cast<PFNGLBUFFERDATAPROC>(reinterpret_cast<void*>(&dkgl_glBufferData));
  glad_glGenVertexArrays =
      reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(reinterpret_cast<void*>(&dkgl_glGenVertexArrays));
  glad_glDeleteVertexArrays = reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(
      reinterpret_cast<void*>(&dkgl_glDeleteVertexArrays));
  glad_glBindVertexArray =
      reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(reinterpret_cast<void*>(&dkgl_glBindVertexArray));
  glad_glEnableVertexAttribArray = reinterpret_cast<PFNGLENABLEVERTEXATTRIBARRAYPROC>(
      reinterpret_cast<void*>(&dkgl_glEnableVertexAttribArray));
  glad_glVertexAttribPointer = reinterpret_cast<PFNGLVERTEXATTRIBPOINTERPROC>(
      reinterpret_cast<void*>(&dkgl_glVertexAttribPointer));
  glad_glVertexAttribIPointer = reinterpret_cast<PFNGLVERTEXATTRIBIPOINTERPROC>(
      reinterpret_cast<void*>(&dkgl_glVertexAttribIPointer));
  glad_glEnable = reinterpret_cast<PFNGLENABLEPROC>(reinterpret_cast<void*>(&dkgl_glEnable));
  glad_glDisable = reinterpret_cast<PFNGLDISABLEPROC>(reinterpret_cast<void*>(&dkgl_glDisable));
  glad_glIsEnabled =
      reinterpret_cast<PFNGLISENABLEDPROC>(reinterpret_cast<void*>(&dkgl_glIsEnabled));
  glad_glBlendFunc =
      reinterpret_cast<PFNGLBLENDFUNCPROC>(reinterpret_cast<void*>(&dkgl_glBlendFunc));
  glad_glBlendFuncSeparate = reinterpret_cast<PFNGLBLENDFUNCSEPARATEPROC>(
      reinterpret_cast<void*>(&dkgl_glBlendFuncSeparate));
  glad_glBlendEquation =
      reinterpret_cast<PFNGLBLENDEQUATIONPROC>(reinterpret_cast<void*>(&dkgl_glBlendEquation));
  glad_glDepthMask =
      reinterpret_cast<PFNGLDEPTHMASKPROC>(reinterpret_cast<void*>(&dkgl_glDepthMask));
  glad_glDepthFunc =
      reinterpret_cast<PFNGLDEPTHFUNCPROC>(reinterpret_cast<void*>(&dkgl_glDepthFunc));
  glad_glViewport =
      reinterpret_cast<PFNGLVIEWPORTPROC>(reinterpret_cast<void*>(&dkgl_glViewport));
  glad_glClearColor =
      reinterpret_cast<PFNGLCLEARCOLORPROC>(reinterpret_cast<void*>(&dkgl_glClearColor));
  glad_glClear = reinterpret_cast<PFNGLCLEARPROC>(reinterpret_cast<void*>(&dkgl_glClear));
  glad_glPrimitiveRestartIndex = reinterpret_cast<PFNGLPRIMITIVERESTARTINDEXPROC>(
      reinterpret_cast<void*>(&dkgl_glPrimitiveRestartIndex));
  glad_glDrawArrays =
      reinterpret_cast<PFNGLDRAWARRAYSPROC>(reinterpret_cast<void*>(&dkgl_glDrawArrays));
  glad_glDrawElements =
      reinterpret_cast<PFNGLDRAWELEMENTSPROC>(reinterpret_cast<void*>(&dkgl_glDrawElements));
  // ---- T105: textures / FBO / programs (30 more) -----------------------------
  glad_glGenTextures =
      reinterpret_cast<PFNGLGENTEXTURESPROC>(reinterpret_cast<void*>(&dkgl_glGenTextures));
  glad_glDeleteTextures =
      reinterpret_cast<PFNGLDELETETEXTURESPROC>(reinterpret_cast<void*>(&dkgl_glDeleteTextures));
  glad_glActiveTexture = reinterpret_cast<PFNGLACTIVETEXTUREPROC>(
      reinterpret_cast<void*>(&dkgl_glActiveTexture));
  glad_glBindTexture =
      reinterpret_cast<PFNGLBINDTEXTUREPROC>(reinterpret_cast<void*>(&dkgl_glBindTexture));
  glad_glTexParameteri =
      reinterpret_cast<PFNGLTEXPARAMETERIPROC>(reinterpret_cast<void*>(&dkgl_glTexParameteri));
  glad_glTexImage2D =
      reinterpret_cast<PFNGLTEXIMAGE2DPROC>(reinterpret_cast<void*>(&dkgl_glTexImage2D));
  glad_glGenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(
      reinterpret_cast<void*>(&dkgl_glGenFramebuffers));
  glad_glDeleteFramebuffers = reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(
      reinterpret_cast<void*>(&dkgl_glDeleteFramebuffers));
  glad_glBindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(
      reinterpret_cast<void*>(&dkgl_glBindFramebuffer));
  glad_glFramebufferTexture2D = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(
      reinterpret_cast<void*>(&dkgl_glFramebufferTexture2D));
  glad_glDrawBuffers =
      reinterpret_cast<PFNGLDRAWBUFFERSPROC>(reinterpret_cast<void*>(&dkgl_glDrawBuffers));
  glad_glCheckFramebufferStatus = reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(
      reinterpret_cast<void*>(&dkgl_glCheckFramebufferStatus));
  glad_glCreateShader =
      reinterpret_cast<PFNGLCREATESHADERPROC>(reinterpret_cast<void*>(&dkgl_glCreateShader));
  glad_glShaderSource =
      reinterpret_cast<PFNGLSHADERSOURCEPROC>(reinterpret_cast<void*>(&dkgl_glShaderSource));
  glad_glCompileShader =
      reinterpret_cast<PFNGLCOMPILESHADERPROC>(reinterpret_cast<void*>(&dkgl_glCompileShader));
  glad_glCreateProgram =
      reinterpret_cast<PFNGLCREATEPROGRAMPROC>(reinterpret_cast<void*>(&dkgl_glCreateProgram));
  glad_glAttachShader =
      reinterpret_cast<PFNGLATTACHSHADERPROC>(reinterpret_cast<void*>(&dkgl_glAttachShader));
  glad_glLinkProgram =
      reinterpret_cast<PFNGLLINKPROGRAMPROC>(reinterpret_cast<void*>(&dkgl_glLinkProgram));
  glad_glDeleteShader =
      reinterpret_cast<PFNGLDELETESHADERPROC>(reinterpret_cast<void*>(&dkgl_glDeleteShader));
  glad_glUseProgram =
      reinterpret_cast<PFNGLUSEPROGRAMPROC>(reinterpret_cast<void*>(&dkgl_glUseProgram));
  glad_glGetShaderiv =
      reinterpret_cast<PFNGLGETSHADERIVPROC>(reinterpret_cast<void*>(&dkgl_glGetShaderiv));
  glad_glGetProgramiv =
      reinterpret_cast<PFNGLGETPROGRAMIVPROC>(reinterpret_cast<void*>(&dkgl_glGetProgramiv));
  glad_glGetShaderInfoLog = reinterpret_cast<PFNGLGETSHADERINFOLOGPROC>(
      reinterpret_cast<void*>(&dkgl_glGetShaderInfoLog));
  glad_glGetUniformLocation = reinterpret_cast<PFNGLGETUNIFORMLOCATIONPROC>(
      reinterpret_cast<void*>(&dkgl_glGetUniformLocation));
  glad_glUniform1i =
      reinterpret_cast<PFNGLUNIFORM1IPROC>(reinterpret_cast<void*>(&dkgl_glUniform1i));
  glad_glUniform1f =
      reinterpret_cast<PFNGLUNIFORM1FPROC>(reinterpret_cast<void*>(&dkgl_glUniform1f));
  glad_glUniform4f =
      reinterpret_cast<PFNGLUNIFORM4FPROC>(reinterpret_cast<void*>(&dkgl_glUniform4f));
  glad_glGetUniformBlockIndex = reinterpret_cast<PFNGLGETUNIFORMBLOCKINDEXPROC>(
      reinterpret_cast<void*>(&dkgl_glGetUniformBlockIndex));
  glad_glUniformBlockBinding = reinterpret_cast<PFNGLUNIFORMBLOCKBINDINGPROC>(
      reinterpret_cast<void*>(&dkgl_glUniformBlockBinding));
  glad_glGetError = reinterpret_cast<PFNGLGETERRORPROC>(reinterpret_cast<void*>(&dkgl_glGetError));
  glad_glGetIntegerv =
      reinterpret_cast<PFNGLGETINTEGERVPROC>(reinterpret_cast<void*>(&dkgl_glGetIntegerv));
  glad_glUniform1ui =
      reinterpret_cast<PFNGLUNIFORM1UIPROC>(reinterpret_cast<void*>(&dkgl_glUniform1ui));
  glad_glUniform3f =
      reinterpret_cast<PFNGLUNIFORM3FPROC>(reinterpret_cast<void*>(&dkgl_glUniform3f));
  glad_glUniformMatrix4fv = reinterpret_cast<PFNGLUNIFORMMATRIX4FVPROC>(
      reinterpret_cast<void*>(&dkgl_glUniformMatrix4fv));
  glad_glBlendColor =
      reinterpret_cast<PFNGLBLENDCOLORPROC>(reinterpret_cast<void*>(&dkgl_glBlendColor));
  // ---- T112/T113: depth-cue + shadow (6 more) -------------------------------------
  glad_glBlitFramebuffer = reinterpret_cast<PFNGLBLITFRAMEBUFFERPROC>(
      reinterpret_cast<void*>(&dkgl_glBlitFramebuffer));
  glad_glUniform4fv =
      reinterpret_cast<PFNGLUNIFORM4FVPROC>(reinterpret_cast<void*>(&dkgl_glUniform4fv));
  glad_glStencilFunc =
      reinterpret_cast<PFNGLSTENCILFUNCPROC>(reinterpret_cast<void*>(&dkgl_glStencilFunc));
  glad_glStencilOp =
      reinterpret_cast<PFNGLSTENCILOPPROC>(reinterpret_cast<void*>(&dkgl_glStencilOp));
  glad_glStencilMask =
      reinterpret_cast<PFNGLSTENCILMASKPROC>(reinterpret_cast<void*>(&dkgl_glStencilMask));
  glad_glColorMask =
      reinterpret_cast<PFNGLCOLORMASKPROC>(reinterpret_cast<void*>(&dkgl_glColorMask));
  glad_glColorMaski =
      reinterpret_cast<PFNGLCOLORMASKIPROC>(reinterpret_cast<void*>(&dkgl_glColorMaski));
  glad_glBufferSubData =
      reinterpret_cast<PFNGLBUFFERSUBDATAPROC>(reinterpret_cast<void*>(&dkgl_glBufferSubData));
  glad_glBindBufferRange = reinterpret_cast<PFNGLBINDBUFFERRANGEPROC>(
      reinterpret_cast<void*>(&dkgl_glBindBufferRange));
  glad_glBindBufferBase = reinterpret_cast<PFNGLBINDBUFFERBASEPROC>(
      reinterpret_cast<void*>(&dkgl_glBindBufferBase));
  glad_glMapBufferRange =
      reinterpret_cast<PFNGLMAPBUFFERRANGEPROC>(reinterpret_cast<void*>(&dkgl_glMapBufferRange));
  glad_glUnmapBuffer =
      reinterpret_cast<PFNGLUNMAPBUFFERPROC>(reinterpret_cast<void*>(&dkgl_glUnmapBuffer));
  glad_glFenceSync =
      reinterpret_cast<PFNGLFENCESYNCPROC>(reinterpret_cast<void*>(&dkgl_glFenceSync));
  glad_glClientWaitSync =
      reinterpret_cast<PFNGLCLIENTWAITSYNCPROC>(reinterpret_cast<void*>(&dkgl_glClientWaitSync));
  glad_glDeleteSync =
      reinterpret_cast<PFNGLDELETESYNCPROC>(reinterpret_cast<void*>(&dkgl_glDeleteSync));
  glad_glFinish = reinterpret_cast<PFNGLFINISHPROC>(reinterpret_cast<void*>(&dkgl_glFinish));
  // spec text says "(23)"/"(6)"; T104b's subset has 24 entries, T105 adds 30, the review
  // adds glGetIntegerv -> 55.
  switch_run_logf("[dkgl] glad overrides: buffers/vao/state (24) + textures/programs (30) + "
                  "glGetIntegerv + Generic2 (4) + depth-cue/shadow (6) + far ocean (8) = %d", 73);
}

}  // namespace dkgl

#endif  // __SWITCH__