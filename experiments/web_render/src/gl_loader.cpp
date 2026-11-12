#include "gl_loader.h"

#include <cstdint>

namespace web {
namespace gl {

namespace {

// 获取入口地址。现代入口优先使用 wglGetProcAddress；若驱动未返回，则回退到
// opengl32.dll 的导出表，兼容部分把 1.2 以上入口直接导出的实现。
void* LoadProcAddress(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    PROC proc = wglGetProcAddress(name);
    void* address = reinterpret_cast<void*>(proc);
    if (address == nullptr) {
        HMODULE module = GetModuleHandleW(L"opengl32.dll");
        if (module != nullptr) {
            FARPROC fallback = GetProcAddress(module, name);
            address = reinterpret_cast<void*>(fallback);
        }
    }

    // 部分驱动对无效入口返回 1、2、3 或 -1 这类哨兵值，需要按空指针处理。
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    if (value == 0u || value == 1u || value == 2u || value == 3u ||
        value == static_cast<std::uintptr_t>(-1)) {
        return nullptr;
    }
    return address;
}

template <typename FunctionPointer>
FunctionPointer LoadEntry(const char* name) {
    return reinterpret_cast<FunctionPointer>(LoadProcAddress(name));
}

}  // namespace

PfnGenBuffers pfnGenBuffers = nullptr;
PfnDeleteBuffers pfnDeleteBuffers = nullptr;
PfnBindBuffer pfnBindBuffer = nullptr;
PfnBufferData pfnBufferData = nullptr;
PfnBufferSubData pfnBufferSubData = nullptr;

PfnGenVertexArrays pfnGenVertexArrays = nullptr;
PfnDeleteVertexArrays pfnDeleteVertexArrays = nullptr;
PfnBindVertexArray pfnBindVertexArray = nullptr;
PfnEnableVertexAttribArray pfnEnableVertexAttribArray = nullptr;
PfnDisableVertexAttribArray pfnDisableVertexAttribArray = nullptr;
PfnVertexAttribPointer pfnVertexAttribPointer = nullptr;

PfnCreateShader pfnCreateShader = nullptr;
PfnShaderSource pfnShaderSource = nullptr;
PfnCompileShader pfnCompileShader = nullptr;
PfnGetShaderiv pfnGetShaderiv = nullptr;
PfnGetShaderInfoLog pfnGetShaderInfoLog = nullptr;
PfnDeleteShader pfnDeleteShader = nullptr;
PfnCreateProgram pfnCreateProgram = nullptr;
PfnAttachShader pfnAttachShader = nullptr;
PfnLinkProgram pfnLinkProgram = nullptr;
PfnGetProgramiv pfnGetProgramiv = nullptr;
PfnGetProgramInfoLog pfnGetProgramInfoLog = nullptr;
PfnUseProgram pfnUseProgram = nullptr;
PfnDeleteProgram pfnDeleteProgram = nullptr;

PfnGetUniformLocation pfnGetUniformLocation = nullptr;
PfnUniform1i pfnUniform1i = nullptr;
PfnUniform1f pfnUniform1f = nullptr;
PfnUniform2f pfnUniform2f = nullptr;
PfnUniform4f pfnUniform4f = nullptr;

PfnActiveTexture pfnActiveTexture = nullptr;

PfnGenFramebuffers pfnGenFramebuffers = nullptr;
PfnBindFramebuffer pfnBindFramebuffer = nullptr;
PfnFramebufferTexture2D pfnFramebufferTexture2D = nullptr;
PfnCheckFramebufferStatus pfnCheckFramebufferStatus = nullptr;
PfnDeleteFramebuffers pfnDeleteFramebuffers = nullptr;

PfnCreateContextAttribsARB pfnCreateContextAttribsARB = nullptr;

bool LoadOpenGL33() {
    pfnGenBuffers = LoadEntry<PfnGenBuffers>("glGenBuffers");
    pfnDeleteBuffers = LoadEntry<PfnDeleteBuffers>("glDeleteBuffers");
    pfnBindBuffer = LoadEntry<PfnBindBuffer>("glBindBuffer");
    pfnBufferData = LoadEntry<PfnBufferData>("glBufferData");
    pfnBufferSubData = LoadEntry<PfnBufferSubData>("glBufferSubData");

    pfnGenVertexArrays = LoadEntry<PfnGenVertexArrays>("glGenVertexArrays");
    pfnDeleteVertexArrays = LoadEntry<PfnDeleteVertexArrays>("glDeleteVertexArrays");
    pfnBindVertexArray = LoadEntry<PfnBindVertexArray>("glBindVertexArray");
    pfnEnableVertexAttribArray =
        LoadEntry<PfnEnableVertexAttribArray>("glEnableVertexAttribArray");
    pfnDisableVertexAttribArray =
        LoadEntry<PfnDisableVertexAttribArray>("glDisableVertexAttribArray");
    pfnVertexAttribPointer = LoadEntry<PfnVertexAttribPointer>("glVertexAttribPointer");

    pfnCreateShader = LoadEntry<PfnCreateShader>("glCreateShader");
    pfnShaderSource = LoadEntry<PfnShaderSource>("glShaderSource");
    pfnCompileShader = LoadEntry<PfnCompileShader>("glCompileShader");
    pfnGetShaderiv = LoadEntry<PfnGetShaderiv>("glGetShaderiv");
    pfnGetShaderInfoLog = LoadEntry<PfnGetShaderInfoLog>("glGetShaderInfoLog");
    pfnDeleteShader = LoadEntry<PfnDeleteShader>("glDeleteShader");
    pfnCreateProgram = LoadEntry<PfnCreateProgram>("glCreateProgram");
    pfnAttachShader = LoadEntry<PfnAttachShader>("glAttachShader");
    pfnLinkProgram = LoadEntry<PfnLinkProgram>("glLinkProgram");
    pfnGetProgramiv = LoadEntry<PfnGetProgramiv>("glGetProgramiv");
    pfnGetProgramInfoLog = LoadEntry<PfnGetProgramInfoLog>("glGetProgramInfoLog");
    pfnUseProgram = LoadEntry<PfnUseProgram>("glUseProgram");
    pfnDeleteProgram = LoadEntry<PfnDeleteProgram>("glDeleteProgram");

    pfnGetUniformLocation = LoadEntry<PfnGetUniformLocation>("glGetUniformLocation");
    pfnUniform1i = LoadEntry<PfnUniform1i>("glUniform1i");
    pfnUniform1f = LoadEntry<PfnUniform1f>("glUniform1f");
    pfnUniform2f = LoadEntry<PfnUniform2f>("glUniform2f");
    pfnUniform4f = LoadEntry<PfnUniform4f>("glUniform4f");

    pfnActiveTexture = LoadEntry<PfnActiveTexture>("glActiveTexture");

    pfnGenFramebuffers = LoadEntry<PfnGenFramebuffers>("glGenFramebuffers");
    pfnBindFramebuffer = LoadEntry<PfnBindFramebuffer>("glBindFramebuffer");
    pfnFramebufferTexture2D =
        LoadEntry<PfnFramebufferTexture2D>("glFramebufferTexture2D");
    pfnCheckFramebufferStatus =
        LoadEntry<PfnCheckFramebufferStatus>("glCheckFramebufferStatus");
    pfnDeleteFramebuffers = LoadEntry<PfnDeleteFramebuffers>("glDeleteFramebuffers");

    return pfnGenBuffers != nullptr && pfnDeleteBuffers != nullptr &&
           pfnBindBuffer != nullptr && pfnBufferData != nullptr &&
           pfnGenVertexArrays != nullptr && pfnDeleteVertexArrays != nullptr &&
           pfnBindVertexArray != nullptr && pfnEnableVertexAttribArray != nullptr &&
           pfnVertexAttribPointer != nullptr && pfnCreateShader != nullptr &&
           pfnShaderSource != nullptr && pfnCompileShader != nullptr &&
           pfnGetShaderiv != nullptr && pfnDeleteShader != nullptr &&
           pfnCreateProgram != nullptr && pfnAttachShader != nullptr &&
           pfnLinkProgram != nullptr && pfnGetProgramiv != nullptr &&
           pfnUseProgram != nullptr && pfnDeleteProgram != nullptr &&
           pfnGetUniformLocation != nullptr && pfnUniform1i != nullptr &&
           pfnUniform1f != nullptr && pfnUniform2f != nullptr &&
           pfnUniform4f != nullptr && pfnActiveTexture != nullptr &&
           pfnGenFramebuffers != nullptr && pfnBindFramebuffer != nullptr &&
           pfnFramebufferTexture2D != nullptr &&
           pfnCheckFramebufferStatus != nullptr &&
           pfnDeleteFramebuffers != nullptr;
}

bool LoadWglContextExtensions() {
    pfnCreateContextAttribsARB =
        LoadEntry<PfnCreateContextAttribsARB>("wglCreateContextAttribsARB");
    return pfnCreateContextAttribsARB != nullptr;
}

}  // namespace gl
}  // namespace web
