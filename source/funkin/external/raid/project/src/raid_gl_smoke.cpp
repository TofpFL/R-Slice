// Phase 4 smoke test — instanced rendering inside Lime's existing
// OpenGL context. Loads every GL entry point at runtime through
// wglGetProcAddress/opengl32.dll, so no GL import lib or headers are
// required by the link. Saves and restores all GL state it touches;
// intended to be called from FlxG.signals.postDraw (last GL work of
// the frame, context current on the main thread).
#include "raid_gl_smoke.hpp"

#include <windows.h>

// No GL headers: stock gl.h only covers GL 1.1 and would collide with
// the runtime-loaded function pointers. Define the subset we use.
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLubyte;
typedef void GLvoid;

#define GL_NO_ERROR 0
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_EXTENSIONS 0x1F03

// Hardware limit pnames (queried for diagnostics/capability sizing only)
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#define GL_MAX_SHADER_STORAGE_BLOCK_BINDINGS 0x90DD
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- constants/types that stock gl.h may not define -------------------
#ifndef GLchar
#define GLchar char
#endif
#ifndef GLbitfield
#define GLbitfield unsigned int
#endif
#ifndef GLuint64
#define GLuint64 unsigned long long
#endif
#ifndef GLsizeiptr
#define GLsizeiptr ptrdiff_t
#endif
#ifndef GLintptr
#define GLintptr ptrdiff_t
#endif
#ifndef GLsync
#define GLsync void*
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CAA
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_BOX 0x0C10
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_LINEAR 0x2601
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_DEPTH_TEST
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_POLYGON_OFFSET_FILL 0x8037
#endif
#ifndef GL_DEPTH_FUNC
#define GL_DEPTH_FUNC 0x0B74
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_COLOR_WRITEMASK 0x0C23
#endif
#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#endif
#ifndef GL_CULL_FACE_MODE
#define GL_CULL_FACE_MODE 0x0B45
#define GL_FRONT_FACE 0x0B46
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_TRIANGLES
#define GL_TRIANGLES 0x0004
#define GL_BYTE 0x1400
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002
#endif

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// GL function loader
// ---------------------------------------------------------------------------

HMODULE g_glDll = nullptr;
typedef PROC(WINAPI* wglGetProcAddress_t)(LPCSTR);
wglGetProcAddress_t g_wglGetProcAddress = nullptr;

void* glproc(const char* name) {
    void* p = nullptr;
    if (g_wglGetProcAddress) p = (void*)g_wglGetProcAddress(name);
    // wglGetProcAddress returns 1/2/3/-1 on failure.
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)(LONG_PTR)-1)
        p = g_glDll ? (void*)GetProcAddress(g_glDll, name) : nullptr;
    return p;
}

#define GLFN(ret, name, params) \
    typedef ret(APIENTRY* name##_t) params; \
    static name##_t name = nullptr;

// GL 1.x / core entry points we call directly.
GLFN(const GLubyte*, glGetString, (GLenum))
GLFN(const GLubyte*, glGetStringi, (GLenum, GLuint))
GLFN(void, glGetIntegerv, (GLenum, GLint*))
GLFN(void, glGetBooleanv, (GLenum, GLboolean*))
GLFN(GLenum, glGetError, ())
GLFN(void, glFinish, ())
GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLFN(void, glEnable, (GLenum))
GLFN(void, glDisable, (GLenum))
GLFN(void, glBlendFunc, (GLenum, GLenum))
GLFN(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))
GLFN(void, glBlendEquationSeparate, (GLenum, GLenum))
GLFN(void, glDepthFunc, (GLenum))
GLFN(void, glDepthMask, (GLboolean))
GLFN(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
GLFN(void, glCullFace, (GLenum))
GLFN(void, glFrontFace, (GLenum))
GLFN(void, glScissor, (GLint, GLint, GLsizei, GLsizei))
GLFN(void, glPixelStorei, (GLenum, GLint))
GLFN(void, glActiveTexture, (GLenum))
GLFN(void, glGenTextures, (GLsizei, GLuint*))
GLFN(void, glBindTexture, (GLenum, GLuint))
GLFN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*))
GLFN(void, glTexParameteri, (GLenum, GLenum, GLint))
GLFN(void, glDeleteTextures, (GLsizei, const GLuint*))
GLFN(void, glGenBuffers, (GLsizei, GLuint*))
GLFN(void, glBindBuffer, (GLenum, GLuint))
GLFN(void, glBufferData, (GLenum, GLsizeiptr, const GLvoid*, GLenum))
GLFN(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const GLvoid*))
GLFN(void, glDeleteBuffers, (GLsizei, const GLuint*))
GLFN(void, glGenVertexArrays, (GLsizei, GLuint*))
GLFN(void, glBindVertexArray, (GLuint))
GLFN(void, glDeleteVertexArrays, (GLsizei, const GLuint*))
GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(void, glDeleteShader, (GLuint))
GLFN(GLuint, glCreateProgram, ())
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(void, glUseProgram, (GLuint))
GLFN(void, glDeleteProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar*))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform1f, (GLint, GLfloat))
GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLFN(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform4fv, (GLint, GLsizei, const GLfloat*))
GLFN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*))
GLFN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
GLFN(void, glEnableVertexAttribArray, (GLuint))
GLFN(void, glVertexAttribDivisor, (GLuint, GLuint))
GLFN(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLFN(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))
GLFN(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*))
GLFN(void, glBindFramebuffer, (GLenum, GLuint))
// Advanced probes — pointers may stay null.
GLFN(void, glDrawArraysIndirect, (GLenum, GLenum, const void*))
GLFN(void, glMultiDrawArraysIndirect, (GLenum, GLenum, const void*, GLsizei, GLsizei))
GLFN(void, glDispatchCompute, (GLuint, GLuint, GLuint))
GLFN(void, glBufferStorage, (GLenum, GLsizeiptr, const void*, GLbitfield))
GLFN(void*, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))
GLFN(void, glUnmapBuffer, (GLenum))
GLFN(void, glMemoryBarrier, (GLbitfield))
GLFN(void, glBindBufferBase, (GLenum, GLuint, GLuint))
GLFN(void, glGetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void*))
GLFN(GLsync, glFenceSync, (GLenum, GLbitfield))
GLFN(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64))
GLFN(void, glDeleteSync, (GLsync))
GLFN(void, glBindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))
GLFN(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))

bool g_loaded = false;
bool g_loaderOk = false;
HGLRC(WINAPI* p_wglGetCurrentContext)() = nullptr;

bool loadGL() {
    if (g_loaded) return g_loaderOk;
    g_loaded = true;
    g_glDll = GetModuleHandleA("opengl32.dll");
    if (!g_glDll) return false;
    g_wglGetProcAddress = (wglGetProcAddress_t)GetProcAddress(g_glDll, "wglGetProcAddress");
    p_wglGetCurrentContext = (HGLRC(WINAPI*)())GetProcAddress(g_glDll, "wglGetCurrentContext");

#define LOAD_REQ(n) n = (n##_t)glproc(#n);
    LOAD_REQ(glGetString); LOAD_REQ(glGetStringi); LOAD_REQ(glGetIntegerv);
    LOAD_REQ(glGetBooleanv); LOAD_REQ(glGetError); LOAD_REQ(glFinish);
    LOAD_REQ(glViewport); LOAD_REQ(glEnable); LOAD_REQ(glDisable);
    LOAD_REQ(glBlendFunc); LOAD_REQ(glBlendFuncSeparate); LOAD_REQ(glBlendEquationSeparate);
    LOAD_REQ(glDepthFunc); LOAD_REQ(glDepthMask); LOAD_REQ(glColorMask);
    LOAD_REQ(glCullFace); LOAD_REQ(glFrontFace); LOAD_REQ(glScissor);
    LOAD_REQ(glPixelStorei); LOAD_REQ(glActiveTexture);
    LOAD_REQ(glGenTextures); LOAD_REQ(glBindTexture); LOAD_REQ(glTexImage2D);
    LOAD_REQ(glTexParameteri); LOAD_REQ(glDeleteTextures);
    LOAD_REQ(glGenBuffers); LOAD_REQ(glBindBuffer); LOAD_REQ(glBufferData);
    LOAD_REQ(glBufferSubData); LOAD_REQ(glDeleteBuffers);
    LOAD_REQ(glGenVertexArrays); LOAD_REQ(glBindVertexArray); LOAD_REQ(glDeleteVertexArrays);
    LOAD_REQ(glCreateShader); LOAD_REQ(glShaderSource); LOAD_REQ(glCompileShader);
    LOAD_REQ(glGetShaderiv); LOAD_REQ(glGetShaderInfoLog); LOAD_REQ(glDeleteShader);
    LOAD_REQ(glCreateProgram); LOAD_REQ(glAttachShader); LOAD_REQ(glLinkProgram);
    LOAD_REQ(glGetProgramiv); LOAD_REQ(glGetProgramInfoLog); LOAD_REQ(glUseProgram);
    LOAD_REQ(glDeleteProgram); LOAD_REQ(glGetUniformLocation);
    LOAD_REQ(glUniform1i); LOAD_REQ(glUniform1f); LOAD_REQ(glUniform2f);
    LOAD_REQ(glUniform3f); LOAD_REQ(glUniform4f); LOAD_REQ(glUniform4fv);
    LOAD_REQ(glVertexAttribPointer); LOAD_REQ(glBindAttribLocation);
    LOAD_REQ(glEnableVertexAttribArray);
    LOAD_REQ(glVertexAttribDivisor);
    LOAD_REQ(glDrawArrays); LOAD_REQ(glDrawArraysInstanced); LOAD_REQ(glReadPixels);
    LOAD_REQ(glBindFramebuffer);
#undef LOAD_REQ

#define LOAD_OPT(n) n = (n##_t)glproc(#n);
    LOAD_OPT(glDrawArraysIndirect); LOAD_OPT(glMultiDrawArraysIndirect);
    LOAD_OPT(glDispatchCompute); LOAD_OPT(glBufferStorage); LOAD_OPT(glMapBufferRange);
    LOAD_OPT(glUnmapBuffer); LOAD_OPT(glMemoryBarrier); LOAD_OPT(glBindBufferBase);
    LOAD_OPT(glGetBufferSubData); LOAD_OPT(glFenceSync); LOAD_OPT(glClientWaitSync);
    LOAD_OPT(glDeleteSync); LOAD_OPT(glBindImageTexture); LOAD_OPT(glTexStorage2D);
#undef LOAD_OPT

    g_loaderOk = glGetString && glGetIntegerv && glViewport && glGenBuffers &&
                 glGenVertexArrays && glCreateShader && glUseProgram &&
                 glVertexAttribPointer && glVertexAttribDivisor && glDrawArraysInstanced;
    return g_loaderOk;
}

// ---------------------------------------------------------------------------
// GL state save / restore
// ---------------------------------------------------------------------------

struct GLState {
    GLint viewport[4];
    GLint scissorBox[4];
    GLboolean scissorTest, blend, depthTest, cullFace, stencilTest, dither;
    GLint blendSrcRgb, blendDstRgb, blendSrcA, blendDstA;
    GLint blendEqRgb, blendEqA;
    GLint depthFunc;
    GLboolean depthMask;
    GLboolean colorMask[4];
    GLint cullFaceMode, frontFace;
    GLint packAlign, unpackAlign;
    GLint arrayBuffer, elementArrayBuffer, vao, program;
    GLint activeTexture;
    GLint texture2d;
    GLint framebuffer, readFramebuffer;
    GLboolean polygonOffsetFill;
};

void saveState(GLState& s) {
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
    glGetBooleanv(GL_SCISSOR_TEST, &s.scissorTest);
    glGetBooleanv(GL_BLEND, &s.blend);
    glGetBooleanv(GL_DEPTH_TEST, &s.depthTest);
    glGetBooleanv(GL_CULL_FACE, &s.cullFace);
    glGetBooleanv(GL_STENCIL_TEST, &s.stencilTest);
    glGetBooleanv(GL_DITHER, &s.dither);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEqA);
    glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    glGetIntegerv(GL_CULL_FACE_MODE, &s.cullFaceMode);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
    glGetIntegerv(GL_PACK_ALIGNMENT, &s.packAlign);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpackAlign);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elementArrayBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture2d);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFramebuffer);
    glGetBooleanv(GL_POLYGON_OFFSET_FILL, &s.polygonOffsetFill);
}

void restoreState(const GLState& s) {
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
    s.scissorTest ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.depthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.cullFace ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.stencilTest ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    s.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glBlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcA, s.blendDstA);
    glBlendEquationSeparate(s.blendEqRgb, s.blendEqA);
    glDepthFunc(s.depthFunc);
    glDepthMask(s.depthMask);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    glCullFace(s.cullFaceMode);
    glFrontFace(s.frontFace);
    glPixelStorei(GL_PACK_ALIGNMENT, s.packAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlign);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s.arrayBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s.elementArrayBuffer);
    glBindVertexArray((GLuint)s.vao);
    glUseProgram((GLuint)s.program);
    glActiveTexture((GLenum)s.activeTexture);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s.texture2d);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s.framebuffer);
    if (s.readFramebuffer != s.framebuffer)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s.readFramebuffer);
    s.polygonOffsetFill ? glEnable(GL_POLYGON_OFFSET_FILL) : glDisable(GL_POLYGON_OFFSET_FILL);
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

const char* kVertSrc = R"GLSL(#version 330
layout(location=0) in vec2 aCorner;   // unit quad corner, [-0.5,0.5]
layout(location=1) in vec2 aUV;
layout(location=2) in vec2 iPos;      // window px, top-left origin
layout(location=3) in vec4 iColor;
layout(location=4) in float iScale;
uniform vec2 uViewport;
uniform float uTime;
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 p = aCorner * iScale;
    p += vec2(sin(uTime * 0.001 + float(gl_InstanceID)) * 2.0,
              cos(uTime * 0.0013 + float(gl_InstanceID) * 0.7) * 2.0);
    vec2 ndc;
    ndc.x = (iPos.x + p.x) / uViewport.x * 2.0 - 1.0;
    ndc.y = 1.0 - (iPos.y + p.y) / uViewport.y * 2.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
    vColor = iColor;
}
)GLSL";

const char* kFragSrc = R"GLSL(#version 330
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    vec4 t = texture(uTex, vUV);
    vec4 c = t * vColor;
    fragColor = vec4(c.rgb * c.a, c.a);  // premultiplied, like OpenFL
}
)GLSL";

// Fallback shaders for pre-GLSL-3.30 contexts/drivers: no layout()
// qualifiers (locations bound via glBindAttribLocation), no
// gl_InstanceID (the smoke wobble is cosmetic — dropped), gl_FragColor
// and texture2D instead of `out`/texture.
const char* kVertFallbackSrc = R"GLSL(#version 120
attribute vec2 aCorner;
attribute vec2 aUV;
attribute vec2 iPos;
attribute vec4 iColor;
attribute float iScale;
uniform vec2 uViewport;
uniform float uTime;
varying vec2 vUV;
varying vec4 vColor;
void main() {
    vec2 p = aCorner * iScale;
    vec2 ndc;
    ndc.x = (iPos.x + p.x) / uViewport.x * 2.0 - 1.0;
    ndc.y = 1.0 - (iPos.y + p.y) / uViewport.y * 2.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
    vColor = iColor;
}
)GLSL";

const char* kFragFallbackSrc = R"GLSL(#version 120
varying vec2 vUV;
varying vec4 vColor;
uniform sampler2D uTex;
void main() {
    vec4 t = texture2D(uTex, vUV);
    vec4 c = t * vColor;
    gl_FragColor = vec4(c.rgb * c.a, c.a);
}
)GLSL";

GLuint compileShader(GLenum type, const char* src, std::string& err) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        err += log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Smoke renderer state
// ---------------------------------------------------------------------------

struct Inst { float x, y, r, g, b, a, scale; };  // 28 bytes

struct Smoke {
    bool initAttempted = false;
    bool ok = false;
    bool verifyDone = false;
    std::string error;
    GLuint vao = 0, quadVbo = 0, instVbo = 0, indirectVbo = 0, prog = 0, tex = 0;
    GLint uViewport = -1, uTime = -1, uTex = -1;
    int maxCount = 0;
    std::vector<Inst> staging;
    // stats
    long frames = 0;
    double totalMs = 0, maxMs = 0, uploadMB = 0;
    long verifyOk = 0, verifyFail = 0;
    char verifyRaw[2][128] = {};
    int glMajor = 0, glMinor = 0;
    int shaderLevel = 0;  // GLSL version of the program actually in use
    bool hasIndirect = false;
    int computeTest = -1;  // -1 untested, 0 fail, 1 pass
};

Smoke g_smoke;

GLuint makeTexture() {
    // 32x32 soft radial sprite, straight alpha.
    const int S = 32;
    std::vector<unsigned char> px(S * S * 4);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float dx = (x + 0.5f) / S - 0.5f, dy = (y + 0.5f) / S - 0.5f;
            float d = std::sqrt(dx * dx + dy * dy) * 2.0f;
            float a = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
            unsigned char* p = &px[(y * S + x) * 4];
            p[0] = (unsigned char)(255 * (0.6f + 0.4f * (1.0f - d)));
            p[1] = 255;
            p[2] = 255;
            p[3] = (unsigned char)(a * 255);
        }
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void fillInstances(std::vector<Inst>& v, int count, int mode, double timeMs,
                   int vw, int vh) {
    // Deterministic grid over the actual viewport, jittered.
    float aspect = (float)vw / (float)vh;
    int cols = (int)std::ceil(std::sqrt((double)count * aspect));
    if (cols < 1) cols = 1;
    int rows = (count + cols - 1) / cols;
    float cw = (float)vw / cols, ch = (float)vh / rows;
    unsigned int seed = 12345u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    v.resize(count);
    for (int i = 0; i < count; i++) {
        int cx = i % cols, cy = i / cols;
        float jx = (rnd() - 0.5f) * cw * 0.6f, jy = (rnd() - 0.5f) * ch * 0.6f;
        float hue = (float)i / (float)count;
        float r, g, b;
        float h6 = hue * 6.0f, f = h6 - std::floor(h6);
        float q = 1.0f - f;
        switch ((int)h6 % 6) {
            case 0: r = 1; g = f; b = 0; break;
            case 1: r = q; g = 1; b = 0; break;
            case 2: r = 0; g = 1; b = f; break;
            case 3: r = 0; g = q; b = 1; break;
            case 4: r = f; g = 0; b = 1; break;
            default: r = 1; g = 0; b = q; break;
        }
        Inst& in = v[i];
        in.x = cx * cw + cw * 0.5f + jx;
        in.y = cy * ch + ch * 0.5f + jy;
        in.r = r; in.g = g; in.b = b; in.a = 0.35f;
        in.scale = 10.0f + rnd() * 18.0f;
        bool isVerify = (i >= count - 2);
        if ((mode == 1 || mode == 2) && !isVerify) {
            float t = (float)(timeMs * 0.001);
            in.x += std::sin(t * (0.3f + 0.01f * i) + i) * 30.0f;
            in.y += std::cos(t * (0.23f + 0.013f * i) + i * 1.7f) * 20.0f;
        }
    }
    // Two opaque verify quads at fixed viewport-relative spots, drawn
    // LAST so no later instance can cover them.
    if (count >= 2) {
        float qs = (float)vh * 0.12f;
        int r = count - 2, gidx = count - 1;
        v[r].x = vw * 0.15f; v[r].y = vh * 0.28f;
        v[r].r = 1; v[r].g = 0; v[r].b = 0; v[r].a = 1.0f; v[r].scale = qs;
        v[gidx].x = vw * 0.85f; v[gidx].y = vh * 0.72f;
        v[gidx].r = 0; v[gidx].g = 1; v[gidx].b = 0; v[gidx].a = 1.0f; v[gidx].scale = qs;
    }
}

bool ensureInit() {
    if (g_smoke.initAttempted) return g_smoke.ok;
    g_smoke.initAttempted = true;
    if (!loadGL()) { g_smoke.error = "gl loader failed"; return false; }
    if (!p_wglGetCurrentContext || !p_wglGetCurrentContext()) {
        g_smoke.error = "no current GL context";
        return false;
    }
    auto fail = [&](const char* m) { g_smoke.error = m; return false; };

    const char* ver = (const char*)glGetString(GL_VERSION);
    if (ver) sscanf_s(ver, "%d.%d", &g_smoke.glMajor, &g_smoke.glMinor);
    g_smoke.hasIndirect = glDrawArraysIndirect != nullptr;

    // Shader compile with portability fallback: try #version 330 first
    // (matches the production RaidGLNotes shader level); on failure retry
    // the #version 120 pair. Attribute locations are bound by name so the
    // layout()-free fallback matches the same 0..4 slots.
    const char* vsSrc = kVertSrc;
    const char* fsSrc = kFragSrc;
    std::string err;
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc, err);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, err);
    if ((!vs || !fs) && err.find("#version") != std::string::npos) {
        // Only retry when the failure looks version-related; a genuine
        // shader bug shouldn't be masked by the fallback.
        if (vs) { glDeleteShader(vs); vs = 0; }
        if (fs) { glDeleteShader(fs); fs = 0; }
        err.clear();
        vsSrc = kVertFallbackSrc; fsSrc = kFragFallbackSrc;
        vs = compileShader(GL_VERTEX_SHADER, vsSrc, err);
        fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, err);
        if (vs && fs) g_smoke.shaderLevel = 120;
    } else if (vs && fs) {
        g_smoke.shaderLevel = 330;
    }
    if (!vs || !fs) return fail(("shader compile: " + err).c_str());
    g_smoke.prog = glCreateProgram();
    // Deterministic attribute slots for both shader variants.
    glBindAttribLocation(g_smoke.prog, 0, "aCorner");
    glBindAttribLocation(g_smoke.prog, 1, "aUV");
    glBindAttribLocation(g_smoke.prog, 2, "iPos");
    glBindAttribLocation(g_smoke.prog, 3, "iColor");
    glBindAttribLocation(g_smoke.prog, 4, "iScale");
    glAttachShader(g_smoke.prog, vs);
    glAttachShader(g_smoke.prog, fs);
    glLinkProgram(g_smoke.prog);
    GLint linked = 0;
    glGetProgramiv(g_smoke.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(g_smoke.prog, sizeof(log), &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        glDeleteShader(vs); glDeleteShader(fs);
        return fail((std::string("program link: ") + log).c_str());
    }
    glDeleteShader(vs); glDeleteShader(fs);
    g_smoke.uViewport = glGetUniformLocation(g_smoke.prog, "uViewport");
    g_smoke.uTime = glGetUniformLocation(g_smoke.prog, "uTime");
    g_smoke.uTex = glGetUniformLocation(g_smoke.prog, "uTex");

    static const float quad[] = {
        -0.5f, -0.5f, 0.f, 0.f,
         0.5f, -0.5f, 1.f, 0.f,
         0.5f,  0.5f, 1.f, 1.f,
        -0.5f, -0.5f, 0.f, 0.f,
         0.5f,  0.5f, 1.f, 1.f,
        -0.5f,  0.5f, 0.f, 1.f,
    };
    glGenVertexArrays(1, &g_smoke.vao);
    glBindVertexArray(g_smoke.vao);
    glGenBuffers(1, &g_smoke.quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_smoke.quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    g_smoke.tex = makeTexture();
    g_smoke.ok = true;
    return true;
}

void ensureInstanceBuffer(int count) {
    if (count <= g_smoke.maxCount && g_smoke.instVbo) return;
    if (g_smoke.instVbo) glDeleteBuffers(1, &g_smoke.instVbo);
    glGenBuffers(1, &g_smoke.instVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_smoke.instVbo);
    GLsizeiptr bytes = (GLsizeiptr)count * (GLsizeiptr)sizeof(Inst);
    glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW);
    g_smoke.maxCount = count;
}

void setupInstanceAttribs() {
    glBindVertexArray(g_smoke.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_smoke.instVbo);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Inst), (const void*)0);
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Inst), (const void*)(2 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Inst), (const void*)(6 * sizeof(float)));
    glVertexAttribDivisor(4, 1);
}

// Trivial compute+SSBO verification: one workgroup writes 1u into a
// shader-storage buffer; we read it back. Returns true on exact match.
bool computeTest() {
    if (g_smoke.computeTest >= 0) return g_smoke.computeTest == 1;
    g_smoke.computeTest = 0;
    if (!glDispatchCompute || !glBindBufferBase || !glGetBufferSubData ||
        !glMemoryBarrier || !glCreateShader || !glCreateProgram)
        return false;
    GLState st;
    saveState(st);
    const char* src = R"GLSL(#version 430
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer SmokeBuf { uint v; } b;
void main() { b.v = 1u; }
)GLSL";
    std::string err;
    GLuint cs = compileShader(GL_COMPUTE_SHADER, src, err);
    if (!cs) { restoreState(st); return false; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    glDeleteShader(cs);
    if (!linked) { glDeleteProgram(prog); restoreState(st); return false; }

    GLuint buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    unsigned int zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_DRAW);

    glUseProgram(prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFinish();
    unsigned int got = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(got), &got);
    glDeleteBuffers(1, &buf);
    glDeleteProgram(prog);
    restoreState(st);
    g_smoke.computeTest = (got == 1u) ? 1 : 0;
    return g_smoke.computeTest == 1;
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// One-time pixel verification: two opaque verify quads must land where
// expected. Reads both y orientations and dumps raw values for debug.
void verifyPixels(int vw, int vh) {
    if (g_smoke.verifyDone || !glReadPixels) return;
    g_smoke.verifyDone = true;
    static const struct { float fx, fy; float r, g, b; } checks[2] = {
        {0.15f, 0.28f, 1, 0, 0}, {0.85f, 0.72f, 0, 1, 0},
    };
    int slot = 0;
    for (auto& c : checks) {
        int px = (int)(c.fx * vw);
        int py = (int)(c.fy * vh);
        GLubyte a[4] = {}, b[4] = {};
        glReadPixels(px, vh - py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);  // y-flipped
        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);       // raw
        bool okA = std::abs(a[0] - (int)(c.r * 255)) < 60 &&
                   std::abs(a[1] - (int)(c.g * 255)) < 60 &&
                   std::abs(a[2] - (int)(c.b * 255)) < 60;
        bool okB = std::abs(b[0] - (int)(c.r * 255)) < 60 &&
                   std::abs(b[1] - (int)(c.g * 255)) < 60 &&
                   std::abs(b[2] - (int)(c.b * 255)) < 60;
        if (okA || okB) g_smoke.verifyOk++; else g_smoke.verifyFail++;
        snprintf(g_smoke.verifyRaw[slot], sizeof(g_smoke.verifyRaw[slot]),
                 "@(%d,%d) flip=[%d,%d,%d,%d] raw=[%d,%d,%d,%d]",
                 px, py, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        slot++;
    }
}

}  // namespace

const char* rs_gl_probe() {
    static std::string report;
    report.clear();
    if (!loadGL()) { report = "gl-probe: loader failed (opengl32.dll?)"; return report.c_str(); }
    char buf[1024];
    const char* ver = glGetString ? (const char*)glGetString(GL_VERSION) : "?";
    const char* ren = glGetString ? (const char*)glGetString(GL_RENDERER) : "?";
    const char* ven = glGetString ? (const char*)glGetString(GL_VENDOR) : "?";
    bool ctx = p_wglGetCurrentContext && p_wglGetCurrentContext();
    int major = 0, minor = 0;
    if (ver) sscanf_s(ver, "%d.%d", &major, &minor);
    snprintf(buf, sizeof(buf),
             "gl-probe: ctx=%s version=%s renderer=%s vendor=%s",
             ctx ? "current" : "NONE", ver ? ver : "?", ren ? ren : "?", ven ? ven : "?");
    report = buf;
    // GLSL shading-language version can differ from the GL context version.
    if (glGetString) {
        const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        if (glsl && *glsl) { report += " glsl="; report += glsl; }
    }
    // Context profile matters for shader compilation: `#version NNN core`
    // is rejected by strict drivers (AMD) under a compatibility context,
    // so shaders must omit the profile keyword. Report it for diagnostics.
    if (ctx && glGetIntegerv && (major > 3 || (major == 3 && minor >= 2))) {
        GLint mask = 0;
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
        const char* pname = (mask & GL_CONTEXT_CORE_PROFILE_BIT) ? "core"
                            : (mask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) ? "compat" : "none";
        report += " profile=";
        report += pname;
    }
    const char* tier = "tier0(GL<=2.1): no instancing";
    if (major >= 4 && minor >= 3) {
        tier = "tier3(GL4.3+): instancing+indirect+compute/SSBO";
    } else if (major >= 4 || glDrawArraysIndirect) {
        tier = "tier2(GL4.x/ext): instancing+indirect";
    } else if (major >= 3 || glDrawArraysInstanced) {
        tier = "tier1(GL3.x): instancing via VAO/divisor";
    }
    report += "\n";
    report += "tier: "; report += tier;
    report += "\nfeatures:";
    auto feat = [&](const char* n, bool ok) { report += " "; report += n; report += ok ? "=1" : "=0"; };
    feat("vao", glGenVertexArrays != nullptr);
    feat("instanced", glDrawArraysInstanced != nullptr);
    feat("divisor", glVertexAttribDivisor != nullptr);
    feat("indirect", glDrawArraysIndirect != nullptr);
    feat("multiIndirect", glMultiDrawArraysIndirect != nullptr);
    feat("compute", glDispatchCompute != nullptr);
    feat("ssbo", glBindBufferBase != nullptr);
    feat("bufferStorage", glBufferStorage != nullptr);
    feat("mapRange", glMapBufferRange != nullptr);
    feat("texStorage", glTexStorage2D != nullptr);
    feat("fenceSync", glFenceSync != nullptr);
    feat("gl_InstanceID", true);
    if (glDispatchCompute)
        feat("computeTest", computeTest());
    if (glGetStringi && glGetIntegerv) {
        GLint num = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &num);
        static const char* wanted[] = {"GL_ARB_compute_shader", "GL_ARB_shader_storage_buffer_object",
                                       "GL_ARB_draw_indirect", "GL_ARB_multi_draw_indirect",
                                       "GL_ARB_instanced_arrays", "GL_ARB_vertex_attrib_divisor",
                                       "GL_ARB_buffer_storage", "GL_ARB_map_buffer_range",
                                       "GL_ARB_texture_storage", "GL_EXT_direct_state_access"};
        for (const char* w : wanted) {
            for (GLint i = 0; i < num; i++) {
                const char* e = (const char*)glGetStringi(GL_EXTENSIONS, i);
                if (e && strcmp(e, w) == 0) { report += " "; report += w; report += "=ext"; break; }
            }
        }
    }
    // Hardware limits relevant to the planned instanced/compute renderer.
    // Queried only for diagnostics — rendering decisions must use caps().
    if (ctx && glGetIntegerv) {
        static const struct { const char* name; GLenum pname; } lims[] = {
            {"maxTexSize", GL_MAX_TEXTURE_SIZE},
            {"maxVertAttribs", GL_MAX_VERTEX_ATTRIBS},
            {"maxSSBOBindings", GL_MAX_SHADER_STORAGE_BLOCK_BINDINGS},
            {"maxComputeInvocations", GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS},
            {"maxUniformBlock", GL_MAX_UNIFORM_BLOCK_SIZE},
        };
        for (const auto& L : lims) {
            GLint v = -1;
            glGetError();
            glGetIntegerv(L.pname, &v);
            if (glGetError() == GL_NO_ERROR && v >= 0) {
                report += " "; report += L.name; report += "=";
                report += std::to_string(v);
            }
        }
    }
    // Actual shader compilation is the strongest driver-compat signal:
    // run the smoke init when a context exists and report which GLSL
    // level linked (330 preferred, 120 fallback) plus any error.
    if (ctx) {
        if (ensureInit()) {
            report += " shaderLevel=";
            report += std::to_string(g_smoke.shaderLevel);
        } else {
            report += " shaderErr=";
            report += g_smoke.error;
        }
    }
    return report.c_str();
}

// rs_gl_limit: programmatic access to the same hardware limits reported
// in rs_gl_probe, for renderers that must size buffers/dispatches at
// runtime. Returns -1 when unsupported or on error.
//   0=MAX_TEXTURE_SIZE 1=MAX_VERTEX_ATTRIBS 2=MAX_SHADER_STORAGE_BLOCK_BINDINGS
//   3=MAX_COMPUTE_WORK_GROUP_INVOCATIONS 4=MAX_UNIFORM_BLOCK_SIZE
double rs_gl_limit(int which) {
    static const GLenum pnames[] = {
        GL_MAX_TEXTURE_SIZE, GL_MAX_VERTEX_ATTRIBS,
        GL_MAX_SHADER_STORAGE_BLOCK_BINDINGS,
        GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, GL_MAX_UNIFORM_BLOCK_SIZE,
    };
    if (which < 0 || which >= (int)(sizeof(pnames) / sizeof(pnames[0]))) return -1.0;
    if (!loadGL() || !glGetIntegerv) return -1.0;
    GLint v = -1;
    glGetError();
    glGetIntegerv(pnames[which], &v);
    if (glGetError() != GL_NO_ERROR) return -1.0;
    return (double)v;
}

double rs_gl_smoke_frame(int count, int mode) {
    GLState st;
    saveState(st);
    bool inited = ensureInit();
    if (!inited) { restoreState(st); return -1.0; }
    if (count < 1) count = 1;
    if (count > 4 * 1024 * 1024) count = 4 * 1024 * 1024;

    double t0 = nowMs();
    glViewport(st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3]);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

    ensureInstanceBuffer(count);
    setupInstanceAttribs();

    // Mode 0 uploads once; dynamic modes refill+upload every frame.
    if (mode != 0 || g_smoke.frames == 0) {
        g_smoke.staging.clear();
        fillInstances(g_smoke.staging, count, mode, t0, st.viewport[2], st.viewport[3]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_smoke.instVbo);
    GLsizeiptr bytes = (GLsizeiptr)count * (GLsizeiptr)sizeof(Inst);
    if (mode == 2) {
        glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW);  // orphan
    }
    if (mode != 0 || g_smoke.frames == 0) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, g_smoke.staging.data());
        g_smoke.uploadMB += (double)bytes / (1024.0 * 1024.0);
    }

    glUseProgram(g_smoke.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_smoke.tex);
    if (g_smoke.uTex >= 0) glUniform1i(g_smoke.uTex, 0);
    glUniform2f(g_smoke.uViewport, (float)st.viewport[2], (float)st.viewport[3]);
    glUniform1f(g_smoke.uTime, (float)t0);

    if (mode == 3 && g_smoke.hasIndirect && glDrawArraysIndirect) {
        if (!g_smoke.indirectVbo) glGenBuffers(1, &g_smoke.indirectVbo);
        struct { GLuint count, primcount, first, baseinstance; } cmd = {6, (GLuint)count, 0, 0};
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_smoke.indirectVbo);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), &cmd, GL_STATIC_DRAW);
        glDrawArraysIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
    }

    if (!g_smoke.verifyDone) verifyPixels(st.viewport[2], st.viewport[3]);
    glFinish();
    double ms = nowMs() - t0;

    restoreState(st);

    g_smoke.frames++;
    g_smoke.totalMs += ms;
    if (ms > g_smoke.maxMs) g_smoke.maxMs = ms;
    return ms;
}

const char* rs_gl_smoke_stats() {
    static std::string s;
    char buf[512];
    double avg = g_smoke.frames ? g_smoke.totalMs / g_smoke.frames : 0.0;
    double mbps = (g_smoke.frames && g_smoke.totalMs > 0)
                      ? g_smoke.uploadMB / (g_smoke.totalMs / 1000.0) : 0.0;
    snprintf(buf, sizeof(buf),
             "gl-smoke: frames=%ld avg=%.3fms max=%.3fms upload=%.1fMB/s verify=%ldok/%ldfail gl=%d.%d err=%s raw0=%s raw1=%s",
             g_smoke.frames, avg, g_smoke.maxMs, mbps,
             g_smoke.verifyOk, g_smoke.verifyFail, g_smoke.glMajor, g_smoke.glMinor,
             g_smoke.error.empty() ? "-" : g_smoke.error.c_str(),
             g_smoke.verifyRaw[0], g_smoke.verifyRaw[1]);
    s = buf;
    return s.c_str();
}

// ---------------------------------------------------------------------------
// Capability bitmask — vendor-neutral feature detection for renderer
// fallback decisions. Bit layout (keep in sync with RaidGLSmoke.hx docs):
//   0 VAO            1 INSTANCED       2 DIVISOR        3 INDIRECT
//   4 MULTI_INDIRECT 5 COMPUTE         6 SSBO           7 BUFFER_STORAGE
//   8 MAP_RANGE      9 TEX_STORAGE    10 FENCE_SYNC    11 COMPUTE_OK
//  12 PERSISTENT_MAP 13 CORE_PROFILE  14 GLSL_3_30
// ---------------------------------------------------------------------------
int rs_gl_caps() {
    uint64_t caps = 0;
    if (!loadGL()) return caps;
    auto set = [&](int bit, bool ok) { if (ok) caps |= (uint64_t)1 << bit; };

    set(0, glGenVertexArrays != nullptr);
    set(1, glDrawArraysInstanced != nullptr);
    set(2, glVertexAttribDivisor != nullptr);
    set(3, glDrawArraysIndirect != nullptr);
    set(4, glMultiDrawArraysIndirect != nullptr);
    set(5, glDispatchCompute != nullptr);
    set(6, glBindBufferBase != nullptr);
    set(7, glBufferStorage != nullptr);
    set(8, glMapBufferRange != nullptr && glUnmapBuffer != nullptr);
    set(9, glTexStorage2D != nullptr);
    set(10, glFenceSync != nullptr && glClientWaitSync != nullptr && glDeleteSync != nullptr);
    set(11, computeTest());
    // Persistent/coherent mapping needs the entry points plus GL 4.4 /
    // ARB_buffer_storage semantics; treat presence as "available" and let
    // the caller fall back on glBufferData+glBufferSubData regardless.
    set(12, glMapBufferRange != nullptr);

    if (glGetString && glGetIntegerv &&
        (!p_wglGetCurrentContext || p_wglGetCurrentContext())) {
        const char* ver = (const char*)glGetString(GL_VERSION);
        int major = 0, minor = 0;
        if (ver) sscanf_s(ver, "%d.%d", &major, &minor);
        set(14, major > 3 || (major == 3 && minor >= 3));
        if (major > 3 || (major == 3 && minor >= 2)) {
            GLint mask = 0;
            glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
            set(13, (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0);
        }
    }
    return caps;
}

// ===========================================================================
// Phase 5D — GPU-side note positioning renderer ("RaidGLNotes" backend).
//
// Replaces the per-frame CPU quad rebuild in RaidGLNotes.hx with a single
// instanced draw per strumline. Each streamed note is one instance carrying
// {time, dir, dead}; the vertex shader computes the scroll position, the
// per-direction quad transform, view->device affine and viewport culling.
// CPU per-frame cost: ~15 uniform sets + 1 draw call. Instance data uploads
// only when notes are added/killed/compacted.
// ===========================================================================

namespace {

// Instance layout: 4 floats = {timeMs, dirIndex, deadFlag, pad} (16 bytes).
struct NoteRenderer {
    bool initAttempted = false;
    bool ok = false;
    std::string error;
    GLuint vao = 0, quadVbo = 0, instVbo = 0, prog = 0, tex = 0;
    int instFloats = 0;      // capacity in floats (multiple of 4)
    std::vector<float> staging;  // mirrors logical instance array
    // uniforms
    GLint uNow = -1, uPxPerMs = -1, uDirSign = -1;
    GLint uStrum = -1, uScroll = -1;      // vec2 each
    GLint uRow0 = -1, uRow1 = -1;         // vec3 affine rows (view -> device px)
    GLint uVp = -1, uCull = -1;           // vec2 viewport, vec4 view-space cull
    GLint uDir = -1, uTex = -1, uDebugSolid = -1;
    float dirData[64];      // 4 dirs x 16 floats
    bool dirSet[4] = {};
    bool hasTex = false;
    int texW = 0, texH = 0;
};

NoteRenderer g_notes[8];

const char* kNoteVertSrc = R"GLSL(#version 330
layout(location=0) in vec2 aCorner;   // unit quad corner in {-0.5,0.5}
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 iInst;     // x=timeMs y=dir z=dead w=pad
uniform float uNow;
uniform float uPxPerMs;
uniform float uDirSign;
uniform vec2 uStrum;
uniform vec2 uScroll;
uniform vec3 uRow0;
uniform vec3 uRow1;
uniform vec2 uVp;
uniform vec4 uCull;
uniform vec4 uDir[16];
out vec2 vUV;
out vec4 vColor;
void main() {
    float t = iInst.x;
    int d = int(iInst.y + 0.5);
    float dead = iInst.z;
    vec4 A = uDir[d * 4 + 0];
    vec4 B = uDir[d * 4 + 1];
    vec4 C = uDir[d * 4 + 2];
    vec4 D = uDir[d * 4 + 3];

    // world -> view (scroll subtracted), same math as CPU path
    float wy = uStrum.y + 28.6 + uPxPerMs * (uNow - t) * uDirSign;
    float wx = uStrum.x + C.x;
    float vx = wx - uScroll.x;
    float vy = wy - uScroll.y;

    // per-direction quad corners in view space
    float x0 = vx - B.z + B.x * (1.0 - A.z);
    float y0 = vy - B.w + B.y * (1.0 - A.w);
    float x1 = x0 + A.z * A.x;
    float y1 = y0 + A.w * A.y;

    vColor = vec4(1.0);
    if (dead > 0.5 || x1 < uCull.x || x0 > uCull.z || y1 < uCull.y || y0 > uCull.w) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);  // clipped
        vUV = vec2(0.0);
        return;
    }

    float cx = mix(x0, x1, aCorner.x + 0.5);
    float cy = mix(y0, y1, aCorner.y + 0.5);
    vUV = vec2(mix(C.y, C.w, aCorner.x + 0.5),
               mix(C.z, D.x, aCorner.y + 0.5));

    // view -> device px (affine probed from the canvas transform) -> NDC
    vec2 dev = vec2(dot(uRow0, vec3(cx, cy, 1.0)),
                    dot(uRow1, vec3(cx, cy, 1.0)));
    gl_Position = vec4(dev.x / uVp.x * 2.0 - 1.0,
                       1.0 - dev.y / uVp.y * 2.0, 0.0, 1.0);
}
)GLSL";

const char* kNoteFragSrc = R"GLSL(#version 330
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
uniform float uDebugSolid;
out vec4 fragColor;
void main() {
    if (uDebugSolid > 0.5) { fragColor = vec4(1.0, 0.0, 0.0, 1.0); return; }
    vec4 t = texture(uTex, vUV);
    // getPixels() returns straight (unmultiplied) RGBA — premultiply here
    // to match OpenFL's premultiplied blend pipeline.
    fragColor = vec4(t.rgb * t.a, t.a);
}
)GLSL";

// GLSL 1.20 fallback: no layout(), no `out`, gl_FragColor/texture2D.
// Attribute locations bound via glBindAttribLocation to the same slots.
const char* kNoteVertFallbackSrc = R"GLSL(#version 120
attribute vec2 aCorner;
attribute vec2 aUV;
attribute vec4 iInst;
uniform float uNow;
uniform float uPxPerMs;
uniform float uDirSign;
uniform vec2 uStrum;
uniform vec2 uScroll;
uniform vec3 uRow0;
uniform vec3 uRow1;
uniform vec2 uVp;
uniform vec4 uCull;
uniform vec4 uDir[16];
varying vec2 vUV;
varying vec4 vColor;
void main() {
    float t = iInst.x;
    int d = int(iInst.y + 0.5);
    float dead = iInst.z;
    vec4 A = uDir[d * 4 + 0];
    vec4 B = uDir[d * 4 + 1];
    vec4 C = uDir[d * 4 + 2];
    vec4 D = uDir[d * 4 + 3];
    float wy = uStrum.y + 28.6 + uPxPerMs * (uNow - t) * uDirSign;
    float wx = uStrum.x + C.x;
    float vx = wx - uScroll.x;
    float vy = wy - uScroll.y;
    float x0 = vx - B.z + B.x * (1.0 - A.z);
    float y0 = vy - B.w + B.y * (1.0 - A.w);
    float x1 = x0 + A.z * A.x;
    float y1 = y0 + A.w * A.y;
    vColor = vec4(1.0);
    if (dead > 0.5 || x1 < uCull.x || x0 > uCull.z || y1 < uCull.y || y0 > uCull.w) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vUV = vec2(0.0);
        return;
    }
    float cx = mix(x0, x1, aCorner.x + 0.5);
    float cy = mix(y0, y1, aCorner.y + 0.5);
    vUV = vec2(mix(C.y, C.w, aCorner.x + 0.5),
               mix(C.z, D.x, aCorner.y + 0.5));
    vec2 dev = vec2(dot(uRow0, vec3(cx, cy, 1.0)),
                    dot(uRow1, vec3(cx, cy, 1.0)));
    gl_Position = vec4(dev.x / uVp.x * 2.0 - 1.0,
                       1.0 - dev.y / uVp.y * 2.0, 0.0, 1.0);
}
)GLSL";

const char* kNoteFragFallbackSrc = R"GLSL(#version 120
varying vec2 vUV;
varying vec4 vColor;
uniform sampler2D uTex;
uniform float uDebugSolid;
void main() {
    if (uDebugSolid > 0.5) { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); return; }
    vec4 t = texture2D(uTex, vUV);
    gl_FragColor = vec4(t.rgb * t.a, t.a);
}
)GLSL";

bool noteEnsureInit(NoteRenderer& r) {
    if (r.initAttempted) return r.ok;
    r.initAttempted = true;
    if (!loadGL()) { r.error = "gl loader failed"; return false; }
    if (!p_wglGetCurrentContext || !p_wglGetCurrentContext()) {
        r.error = "no current GL context";
        return false;
    }
    if (!(glGenVertexArrays && glBindVertexArray && glCreateShader &&
          glUseProgram && glVertexAttribDivisor && glDrawArraysInstanced &&
          glUniform4fv && glUniform4f && glUniform3f)) {
        r.error = "missing required GL entry points";
        return false;
    }
    auto fail = [&](const char* m) { r.error = m; return false; };

    const char* vsSrc = kNoteVertSrc;
    const char* fsSrc = kNoteFragSrc;
    std::string err;
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc, err);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, err);
    if ((!vs || !fs) && err.find("#version") != std::string::npos) {
        if (vs) { glDeleteShader(vs); vs = 0; }
        if (fs) { glDeleteShader(fs); fs = 0; }
        err.clear();
        vsSrc = kNoteVertFallbackSrc; fsSrc = kNoteFragFallbackSrc;
        vs = compileShader(GL_VERTEX_SHADER, vsSrc, err);
        fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, err);
    }
    if (!vs || !fs) return fail(("note shader: " + err).c_str());
    r.prog = glCreateProgram();
    glBindAttribLocation(r.prog, 0, "aCorner");
    glBindAttribLocation(r.prog, 1, "aUV");
    glBindAttribLocation(r.prog, 2, "iInst");
    glAttachShader(r.prog, vs);
    glAttachShader(r.prog, fs);
    glLinkProgram(r.prog);
    GLint linked = 0;
    glGetProgramiv(r.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(r.prog, sizeof(log), &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        glDeleteShader(vs); glDeleteShader(fs);
        return fail((std::string("note link: ") + log).c_str());
    }
    glDeleteShader(vs); glDeleteShader(fs);

    r.uNow = glGetUniformLocation(r.prog, "uNow");
    r.uPxPerMs = glGetUniformLocation(r.prog, "uPxPerMs");
    r.uDirSign = glGetUniformLocation(r.prog, "uDirSign");
    r.uStrum = glGetUniformLocation(r.prog, "uStrum");
    r.uScroll = glGetUniformLocation(r.prog, "uScroll");
    r.uRow0 = glGetUniformLocation(r.prog, "uRow0");
    r.uRow1 = glGetUniformLocation(r.prog, "uRow1");
    r.uVp = glGetUniformLocation(r.prog, "uVp");
    r.uCull = glGetUniformLocation(r.prog, "uCull");
    r.uDir = glGetUniformLocation(r.prog, "uDir");
    r.uTex = glGetUniformLocation(r.prog, "uTex");
    r.uDebugSolid = glGetUniformLocation(r.prog, "uDebugSolid");

    // Static unit quad: corner xy + uv, two triangles.
    static const float quad[] = {
        -0.5f, -0.5f, 0.f, 0.f,
         0.5f, -0.5f, 1.f, 0.f,
         0.5f,  0.5f, 1.f, 1.f,
        -0.5f, -0.5f, 0.f, 0.f,
         0.5f,  0.5f, 1.f, 1.f,
        -0.5f,  0.5f, 0.f, 1.f,
    };
    glGenVertexArrays(1, &r.vao);
    glBindVertexArray(r.vao);
    glGenBuffers(1, &r.quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, r.quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)(2 * sizeof(float)));
    glGenBuffers(1, &r.instVbo);
    glBindBuffer(GL_ARRAY_BUFFER, r.instVbo);
    glBufferData(GL_ARRAY_BUFFER, 4096 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    r.instFloats = 4096;
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0);
    glVertexAttribDivisor(2, 1);
    glBindVertexArray(0);

    r.ok = true;
    return true;
}

void noteEnsureCapacity(NoteRenderer& r, int floatsNeeded) {
    if (floatsNeeded <= r.instFloats) return;
    int cap = r.instFloats;
    while (cap < floatsNeeded) cap *= 2;
    glBindBuffer(GL_ARRAY_BUFFER, r.instVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cap * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    r.instFloats = cap;
    // Re-upload the full staging mirror — glBufferData just orphaned the
    // old storage, so every prior instance would otherwise read as zeros.
    if (!r.staging.empty())
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)r.staging.size() * sizeof(float), r.staging.data());
}

} // namespace

// (definitions below use the same C++ linkage as the rest of this file)

// Returns renderer slot (0..7) or -1 on failure.
int rs_notes_create() {
    if (!loadGL()) return -1;
    for (int i = 0; i < 8; i++) {
        if (!g_notes[i].initAttempted || !g_notes[i].ok) {
            if (noteEnsureInit(g_notes[i])) return i;
            return -1;
        }
    }
    return -1;
}

void rs_notes_destroy(int h) {
    if (h < 0 || h >= 8) return;
    NoteRenderer& r = g_notes[h];
    if (r.vao) glDeleteVertexArrays(1, &r.vao);
    if (r.quadVbo) glDeleteBuffers(1, &r.quadVbo);
    if (r.instVbo) glDeleteBuffers(1, &r.instVbo);
    if (r.prog) glDeleteProgram(r.prog);
    if (r.tex) glDeleteTextures(1, &r.tex);
    r = NoteRenderer();
}

void rs_notes_reset(int h) {
    if (h < 0 || h >= 8) return;
    g_notes[h].staging.clear();
    // Texture and dir constants survive reset (same atlas); clearing
    // hasTex here would permanently disable the GL path because the
    // Haxe side only re-uploads when the bitmap object changes.
    for (int i = 0; i < 4; i++) g_notes[h].dirSet[i] = false;
}

// pixPtr: raw pointer to straight-alpha RGBA pixels as a double bit-pattern
// of the address (Haxe DataPointer). Returns 1 on success.
int rs_notes_setTexture(int h, double pixPtr, int w, int hh) {
    if (h < 0 || h >= 8 || w <= 0 || hh <= 0) return 0;
    NoteRenderer& r = g_notes[h];
    if (!r.ok) return 0;
    GLState st;
    saveState(st);
    if (r.tex) glDeleteTextures(1, &r.tex);
    r.tex = 0;
    glGenTextures(1, &r.tex);
    glBindTexture(GL_TEXTURE_2D, r.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, hh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, (const GLvoid*)(uintptr_t)(unsigned long long)pixPtr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum err = glGetError();
    restoreState(st);
    if (err != GL_NO_ERROR) { r.error = "tex upload failed"; return 0; }
    r.hasTex = true;
    r.texW = w; r.texH = hh;
    return 1;
}

// dir constants: 16 floats packed per direction (see Haxe side for layout).
void rs_notes_setDir(int h, int dir, double v0, double v1, double v2, double v3,
                     double v4, double v5, double v6, double v7,
                     double v8, double v9, double v10, double v11,
                     double v12, double v13, double v14, double v15) {
    if (h < 0 || h >= 8 || dir < 0 || dir >= 4) return;
    NoteRenderer& r = g_notes[h];
    double vals[16] = {v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15};
    for (int i = 0; i < 16; i++) r.dirData[dir * 16 + i] = (float)vals[i];
    r.dirSet[dir] = true;
}

void rs_notes_add(int h, float time, int dir) {
    if (h < 0 || h >= 8) return;
    NoteRenderer& r = g_notes[h];
    r.staging.push_back(time);
    r.staging.push_back((float)dir);
    r.staging.push_back(0.0f);
    r.staging.push_back(0.0f);
    noteEnsureCapacity(r, (int)r.staging.size());
    glBindBuffer(GL_ARRAY_BUFFER, r.instVbo);
    GLsizeiptr off = (GLsizeiptr)(r.staging.size() - 4) * (GLsizeiptr)sizeof(float);
    glBufferSubData(GL_ARRAY_BUFFER, off, 4 * (GLsizeiptr)sizeof(float),
                    r.staging.data() + r.staging.size() - 4);
}

void rs_notes_kill(int h, int index) {
    if (h < 0 || h >= 8 || index < 0) return;
    NoteRenderer& r = g_notes[h];
    size_t fi = (size_t)index * 4 + 2;
    if (fi >= r.staging.size()) return;
    r.staging[fi] = 1.0f;
    glBindBuffer(GL_ARRAY_BUFFER, r.instVbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)fi * (GLintptr)sizeof(float),
                    (GLsizeiptr)sizeof(float), &r.staging[fi]);
}

void rs_notes_removePrefix(int h, int n) {
    if (h < 0 || h >= 8 || n <= 0) return;
    NoteRenderer& r = g_notes[h];
    size_t nf = (size_t)n * 4;
    if (nf >= r.staging.size()) { r.staging.clear(); return; }
    r.staging.erase(r.staging.begin(), r.staging.begin() + nf);
    // Keep the VBO at full capacity and re-upload contents — shrinking it
    // to staging.size() would make later glBufferSubData writes fall
    // outside the buffer while instFloats still claims the old capacity.
    glBindBuffer(GL_ARRAY_BUFFER, r.instVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r.instFloats * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    if (!r.staging.empty())
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)r.staging.size() * sizeof(float), r.staging.data());
}

// Draw one frame. Returns 1 when the batch was submitted, 0 when the
// renderer isn't ready (caller falls back to the CPU path).
int rs_notes_draw(int h,
                  double now, double pxPerMs, double dirSign,
                  double strumX, double strumY, double scrollX, double scrollY,
                  double r00, double r01, double r02,
                  double r10, double r11, double r12,
                  double cullX0, double cullY0, double cullX1, double cullY1) {
    if (h < 0 || h >= 8) return 0;
    NoteRenderer& r = g_notes[h];
    if (!r.ok || !r.hasTex || r.staging.empty()) return 0;

    GLState st;
    saveState(st);

    glUseProgram(r.prog);
    glBindVertexArray(r.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r.instVbo);

    if (r.uDir >= 0) glUniform4fv(r.uDir, 16, r.dirData);

    glUniform1f(r.uNow, (GLfloat)now);
    glUniform1f(r.uPxPerMs, (GLfloat)pxPerMs);
    glUniform1f(r.uDirSign, (GLfloat)dirSign);
    if (r.uStrum >= 0) glUniform2f(r.uStrum, (GLfloat)strumX, (GLfloat)strumY);
    if (r.uScroll >= 0) glUniform2f(r.uScroll, (GLfloat)scrollX, (GLfloat)scrollY);
    if (r.uRow0 >= 0) glUniform3f(r.uRow0, (GLfloat)r00, (GLfloat)r01, (GLfloat)r02);
    if (r.uRow1 >= 0) glUniform3f(r.uRow1, (GLfloat)r10, (GLfloat)r11, (GLfloat)r12);
    if (r.uCull >= 0) glUniform4f(r.uCull, (GLfloat)cullX0, (GLfloat)cullY0,
                                  (GLfloat)cullX1, (GLfloat)cullY1);
    if (r.uTex >= 0) glUniform1i(r.uTex, 0);
    if (r.uDebugSolid >= 0)
        glUniform1f(r.uDebugSolid, getenv("RAID_GL_SOLID") ? 1.0f : 0.0f);

    GLint vp[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (r.uVp >= 0) glUniform2f(r.uVp, (GLfloat)vp[2], (GLfloat)vp[3]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.tex);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);

    GLsizei instCount = (GLsizei)(r.staging.size() / 4);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instCount);

    restoreState(st);
    return 1;
}

// end Phase 5D note renderer
