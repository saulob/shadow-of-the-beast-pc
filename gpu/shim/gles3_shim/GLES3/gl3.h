// Stands in for <GLES3/gl3.h> in the one file that includes it: imgui's backend calls OpenGL ES by name, and these
// names go through pointers SDL resolves from ANGLE, so the executable links no OpenGL ES library.
#pragma once

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>

// Desktop features the ES3 build must not see: the backend tests these macros, not its target.
#undef GL_CLIP_ORIGIN
#undef GL_POLYGON_MODE

#define SOTB_IMGUI_GLES3_FUNCTIONS(X)                                                               \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                       \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                           \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                                         \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                 \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                                                     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                                     \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                             \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                           \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                     \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                     \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                     \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                       \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                     \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                     \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                                   \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                       \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                                           \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                                       \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)                               \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                 \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                           \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers)                                                         \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                 \
    X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation)                                             \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                       \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                               \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                         \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                           \
    X(PFNGLGETVERTEXATTRIBIVPROC, glGetVertexAttribiv)                                             \
    X(PFNGLGETVERTEXATTRIBPOINTERVPROC, glGetVertexAttribPointerv)                                 \
    X(PFNGLISPROGRAMPROC, glIsProgram)                                                             \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                         \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                                             \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                       \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                             \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                               \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                           \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define SOTB_IMGUI_GLES3_CORE_FUNCTIONS(X)                                                          \
    X(glActiveTexture)                                                                             \
    X(glBindTexture)                                                                               \
    X(glDeleteTextures)                                                                            \
    X(glDisable)                                                                                   \
    X(glDrawElements)                                                                              \
    X(glEnable)                                                                                    \
    X(glGenTextures)                                                                               \
    X(glGetError)                                                                                  \
    X(glGetIntegerv)                                                                               \
    X(glGetString)                                                                                 \
    X(glIsEnabled)                                                                                 \
    X(glPixelStorei)                                                                               \
    X(glScissor)                                                                                   \
    X(glTexImage2D)                                                                                \
    X(glTexParameteri)                                                                             \
    X(glTexSubImage2D)                                                                             \
    X(glViewport)

#define SOTB_DECLARE(type, name) static type sotb_##name;
SOTB_IMGUI_GLES3_FUNCTIONS(SOTB_DECLARE)
#undef SOTB_DECLARE
#define SOTB_DECLARE(name) static decltype(&::name) sotb_##name;
SOTB_IMGUI_GLES3_CORE_FUNCTIONS(SOTB_DECLARE)
#undef SOTB_DECLARE

static bool LoadGles3ForImGui() {
#define SOTB_RESOLVE(type, name)                                                                    \
    sotb_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                             \
    if (!sotb_##name) {                                                                             \
        return false;                                                                              \
    }
#define SOTB_RESOLVE_CORE(name)                                                                     \
    sotb_##name = reinterpret_cast<decltype(sotb_##name)>(SDL_GL_GetProcAddress(#name));             \
    if (!sotb_##name) {                                                                             \
        return false;                                                                              \
    }
    SOTB_IMGUI_GLES3_FUNCTIONS(SOTB_RESOLVE)
    SOTB_IMGUI_GLES3_CORE_FUNCTIONS(SOTB_RESOLVE_CORE)
#undef SOTB_RESOLVE
#undef SOTB_RESOLVE_CORE
    return true;
}

#define glActiveTexture sotb_glActiveTexture
#define glAttachShader sotb_glAttachShader
#define glBindBuffer sotb_glBindBuffer
#define glBindSampler sotb_glBindSampler
#define glBindTexture sotb_glBindTexture
#define glBindVertexArray sotb_glBindVertexArray
#define glBlendEquation sotb_glBlendEquation
#define glBlendEquationSeparate sotb_glBlendEquationSeparate
#define glBlendFuncSeparate sotb_glBlendFuncSeparate
#define glBufferData sotb_glBufferData
#define glBufferSubData sotb_glBufferSubData
#define glCompileShader sotb_glCompileShader
#define glCreateProgram sotb_glCreateProgram
#define glCreateShader sotb_glCreateShader
#define glDeleteBuffers sotb_glDeleteBuffers
#define glDeleteProgram sotb_glDeleteProgram
#define glDeleteSamplers sotb_glDeleteSamplers
#define glDeleteShader sotb_glDeleteShader
#define glDeleteTextures sotb_glDeleteTextures
#define glDeleteVertexArrays sotb_glDeleteVertexArrays
#define glDetachShader sotb_glDetachShader
#define glDisable sotb_glDisable
#define glDisableVertexAttribArray sotb_glDisableVertexAttribArray
#define glDrawElements sotb_glDrawElements
#define glEnable sotb_glEnable
#define glEnableVertexAttribArray sotb_glEnableVertexAttribArray
#define glGenBuffers sotb_glGenBuffers
#define glGenSamplers sotb_glGenSamplers
#define glGenTextures sotb_glGenTextures
#define glGenVertexArrays sotb_glGenVertexArrays
#define glGetAttribLocation sotb_glGetAttribLocation
#define glGetError sotb_glGetError
#define glGetIntegerv sotb_glGetIntegerv
#define glGetProgramInfoLog sotb_glGetProgramInfoLog
#define glGetProgramiv sotb_glGetProgramiv
#define glGetShaderInfoLog sotb_glGetShaderInfoLog
#define glGetShaderiv sotb_glGetShaderiv
#define glGetString sotb_glGetString
#define glGetStringi sotb_glGetStringi
#define glGetUniformLocation sotb_glGetUniformLocation
#define glGetVertexAttribPointerv sotb_glGetVertexAttribPointerv
#define glGetVertexAttribiv sotb_glGetVertexAttribiv
#define glIsEnabled sotb_glIsEnabled
#define glIsProgram sotb_glIsProgram
#define glLinkProgram sotb_glLinkProgram
#define glPixelStorei sotb_glPixelStorei
#define glSamplerParameteri sotb_glSamplerParameteri
#define glScissor sotb_glScissor
#define glShaderSource sotb_glShaderSource
#define glTexImage2D sotb_glTexImage2D
#define glTexParameteri sotb_glTexParameteri
#define glTexSubImage2D sotb_glTexSubImage2D
#define glUniform1i sotb_glUniform1i
#define glUniformMatrix4fv sotb_glUniformMatrix4fv
#define glUseProgram sotb_glUseProgram
#define glVertexAttribPointer sotb_glVertexAttribPointer
#define glViewport sotb_glViewport
