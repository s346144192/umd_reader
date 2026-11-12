#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <GL/gl.h>

#include <cstddef>

namespace web {
namespace gl {

// 极简 OpenGL 3.3 入口声明。
//
// OpenGL 1.1 的基础函数（glClear、glViewport、glEnable、glBlendFunc、glScissor、
// glReadPixels、glTexImage2D 等）由 opengl32.dll 直接导出，调用时无需经过函数指针。
// 着色器、顶点数组对象、帧缓冲、纹理单元等 2.0 及以上的入口必须通过
// wglGetProcAddress 获取，集中保存在本命名空间的函数指针中，供渲染设备使用。
//
// 所有函数指针统一使用 pfn 前缀，避免与 wingdi.h 中声明的 WGL 函数以及 gl.h 中
// 的基础函数重名。

using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLchar = char;

// 缓冲区对象。
using PfnGenBuffers = void(APIENTRY*)(GLsizei count, GLuint* buffers);
using PfnDeleteBuffers = void(APIENTRY*)(GLsizei count, const GLuint* buffers);
using PfnBindBuffer = void(APIENTRY*)(GLenum target, GLuint buffer);
using PfnBufferData = void(APIENTRY*)(GLenum target, GLsizeiptr size, const void* data,
                                      GLenum usage);
using PfnBufferSubData = void(APIENTRY*)(GLenum target, GLintptr offset, GLsizeiptr size,
                                        const void* data);

// 顶点数组对象与顶点属性。
using PfnGenVertexArrays = void(APIENTRY*)(GLsizei count, GLuint* arrays);
using PfnDeleteVertexArrays = void(APIENTRY*)(GLsizei count, const GLuint* arrays);
using PfnBindVertexArray = void(APIENTRY*)(GLuint array);
using PfnEnableVertexAttribArray = void(APIENTRY*)(GLuint index);
using PfnDisableVertexAttribArray = void(APIENTRY*)(GLuint index);
using PfnVertexAttribPointer = void(APIENTRY*)(GLuint index, GLint size, GLenum type,
                                              GLboolean normalized, GLsizei stride,
                                              const void* pointer);

// 着色器与程序对象。
using PfnCreateShader = GLuint(APIENTRY*)(GLenum type);
using PfnShaderSource = void(APIENTRY*)(GLuint shader, GLsizei count,
                                        const GLchar* const* source, const GLint* length);
using PfnCompileShader = void(APIENTRY*)(GLuint shader);
using PfnGetShaderiv = void(APIENTRY*)(GLuint shader, GLenum name, GLint* value);
using PfnGetShaderInfoLog = void(APIENTRY*)(GLuint shader, GLsizei buffer_size,
                                           GLsizei* length, GLchar* info_log);
using PfnDeleteShader = void(APIENTRY*)(GLuint shader);
using PfnCreateProgram = GLuint(APIENTRY*)();
using PfnAttachShader = void(APIENTRY*)(GLuint program, GLuint shader);
using PfnLinkProgram = void(APIENTRY*)(GLuint program);
using PfnGetProgramiv = void(APIENTRY*)(GLuint program, GLenum name, GLint* value);
using PfnGetProgramInfoLog = void(APIENTRY*)(GLuint program, GLsizei buffer_size,
                                            GLsizei* length, GLchar* info_log);
using PfnUseProgram = void(APIENTRY*)(GLuint program);
using PfnDeleteProgram = void(APIENTRY*)(GLuint program);

// 统一变量。
using PfnGetUniformLocation = GLint(APIENTRY*)(GLuint program, const GLchar* name);
using PfnUniform1i = void(APIENTRY*)(GLint location, GLint value);
using PfnUniform1f = void(APIENTRY*)(GLint location, GLfloat value);
using PfnUniform2f = void(APIENTRY*)(GLint location, GLfloat x, GLfloat y);
using PfnUniform4f = void(APIENTRY*)(GLint location, GLfloat x, GLfloat y, GLfloat z,
                                    GLfloat w);

// 纹理单元。
using PfnActiveTexture = void(APIENTRY*)(GLenum texture);

// 帧缓冲对象。
using PfnGenFramebuffers = void(APIENTRY*)(GLsizei count, GLuint* framebuffers);
using PfnBindFramebuffer = void(APIENTRY*)(GLenum target, GLuint framebuffer);
using PfnFramebufferTexture2D = void(APIENTRY*)(GLenum target, GLenum attachment,
                                               GLenum texture_target, GLuint texture,
                                               GLint level);
using PfnCheckFramebufferStatus = GLenum(APIENTRY*)(GLenum target);
using PfnDeleteFramebuffers = void(APIENTRY*)(GLsizei count, const GLuint* framebuffers);

// 上下文创建扩展（WGL_ARB_create_context）。
using PfnCreateContextAttribsARB = HGLRC(APIENTRY*)(HDC device_context,
                                                    HGLRC shared_context,
                                                    const int* attribute_list);

extern PfnGenBuffers pfnGenBuffers;
extern PfnDeleteBuffers pfnDeleteBuffers;
extern PfnBindBuffer pfnBindBuffer;
extern PfnBufferData pfnBufferData;
extern PfnBufferSubData pfnBufferSubData;

extern PfnGenVertexArrays pfnGenVertexArrays;
extern PfnDeleteVertexArrays pfnDeleteVertexArrays;
extern PfnBindVertexArray pfnBindVertexArray;
extern PfnEnableVertexAttribArray pfnEnableVertexAttribArray;
extern PfnDisableVertexAttribArray pfnDisableVertexAttribArray;
extern PfnVertexAttribPointer pfnVertexAttribPointer;

extern PfnCreateShader pfnCreateShader;
extern PfnShaderSource pfnShaderSource;
extern PfnCompileShader pfnCompileShader;
extern PfnGetShaderiv pfnGetShaderiv;
extern PfnGetShaderInfoLog pfnGetShaderInfoLog;
extern PfnDeleteShader pfnDeleteShader;
extern PfnCreateProgram pfnCreateProgram;
extern PfnAttachShader pfnAttachShader;
extern PfnLinkProgram pfnLinkProgram;
extern PfnGetProgramiv pfnGetProgramiv;
extern PfnGetProgramInfoLog pfnGetProgramInfoLog;
extern PfnUseProgram pfnUseProgram;
extern PfnDeleteProgram pfnDeleteProgram;

extern PfnGetUniformLocation pfnGetUniformLocation;
extern PfnUniform1i pfnUniform1i;
extern PfnUniform1f pfnUniform1f;
extern PfnUniform2f pfnUniform2f;
extern PfnUniform4f pfnUniform4f;

extern PfnActiveTexture pfnActiveTexture;

extern PfnGenFramebuffers pfnGenFramebuffers;
extern PfnBindFramebuffer pfnBindFramebuffer;
extern PfnFramebufferTexture2D pfnFramebufferTexture2D;
extern PfnCheckFramebufferStatus pfnCheckFramebufferStatus;
extern PfnDeleteFramebuffers pfnDeleteFramebuffers;

extern PfnCreateContextAttribsARB pfnCreateContextAttribsARB;

// 加载渲染所需的全部现代 OpenGL 3.3 入口。调用前必须已有当前上下文。
// 返回是否所有关键入口都成功获取；任一关键入口缺失时返回 false。
bool LoadOpenGL33();

// 加载上下文创建扩展 wglCreateContextAttribsARB。调用前必须已有当前上下文。
// 扩展不可用时返回 false，调用方应退化为兼容上下文。
bool LoadWglContextExtensions();

}  // namespace gl
}  // namespace web
