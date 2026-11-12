// OpenGL 3.3 离屏渲染后端实现。
// 该后端使用隐藏窗口创建 WGL 上下文，只渲染到离屏帧缓冲，不创建交换链；
// 像素通过 glReadPixels 回读到 CPU。除 OpenGL 1.1 基础函数外，所有现代入口
// 都由 gl_loader 通过 wglGetProcAddress 获取，不依赖 GLEW 或 GLAD。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl_loader.h"
#include "web/render.h"
#include "web/render_backends.h"

namespace web {
namespace {

// -------------------------------------------------------------------------------
// OpenGL 3.3 常量。Windows 的 GL/gl.h 只声明到 1.1，以下名称在头文件中不存在，
// 在这里以 kGl 前缀集中定义，避免与将来引入的 glext.h 宏冲突。
// -------------------------------------------------------------------------------

constexpr GLenum kGlArrayBuffer = 0x8892;
constexpr GLenum kGlStreamDraw = 0x88E0;

constexpr GLenum kGlVertexShader = 0x8B31;
constexpr GLenum kGlFragmentShader = 0x8B30;
constexpr GLenum kGlCompileStatus = 0x8B81;
constexpr GLenum kGlLinkStatus = 0x8B82;

constexpr GLenum kGlTexture0 = 0x84C0;
constexpr GLenum kGlR8 = 0x8229;
constexpr GLenum kGlRgba8 = 0x8058;
constexpr GLenum kGlClampToEdge = 0x812F;

constexpr GLenum kGlFramebuffer = 0x8D40;
constexpr GLenum kGlColorAttachment0 = 0x8CE0;
constexpr GLenum kGlFramebufferComplete = 0x8CD5;

// WGL_ARB_create_context 的属性与取值。
constexpr int kWglContextMajorVersionArb = 0x2091;
constexpr int kWglContextMinorVersionArb = 0x2092;
constexpr int kWglContextProfileMaskArb = 0x9126;
constexpr int kWglContextCoreProfileBitArb = 0x00000001;

// -------------------------------------------------------------------------------
// 内嵌 GLSL 330 源码。语义与 Direct3D 11 后端保持一致。
// -------------------------------------------------------------------------------

// 实心图元：顶点着色器以 gl_VertexID 生成覆盖矩形的两个三角形，
// 片元着色器使用圆角矩形有向距离场计算覆盖率。
const char kSolidVertexShaderSource[] = R"GLSL(
#version 330 core

uniform vec2 uViewportSize;   // 目标尺寸（像素）
uniform vec4 uRect;           // 左上角坐标与宽高（像素）
uniform float uRadius;        // 圆角半径（像素）
uniform float uBorderWidth;   // 内描边线宽；0 表示填充
uniform vec4 uColor;          // 非预乘 RGBA

out vec2 vLocalPosition;      // 相对矩形中心的像素坐标
out vec2 vHalfExtent;         // 半宽与半高
out vec2 vShape;              // x 为半径，y 为内描边线宽
out vec4 vColor;

const vec2 kCorners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    vec2 corner = kCorners[gl_VertexID];
    vec2 position = uRect.xy + corner * uRect.zw;

    gl_Position = vec4(position.x / uViewportSize.x * 2.0 - 1.0,
                       1.0 - position.y / uViewportSize.y * 2.0,
                       0.0, 1.0);

    vLocalPosition = corner * uRect.zw - uRect.zw * 0.5;
    vHalfExtent = uRect.zw * 0.5;
    vShape = vec2(uRadius, uBorderWidth);
    vColor = uColor;
}
)GLSL";

const char kSolidFragmentShaderSource[] = R"GLSL(
#version 330 core

in vec2 vLocalPosition;
in vec2 vHalfExtent;
in vec2 vShape;
in vec4 vColor;

out vec4 oColor;

void main() {
    float radius = vShape.x;
    float borderWidth = vShape.y;

    // 圆角矩形有向距离场：内部为负，外部为正。
    vec2 innerHalfExtent = max(vHalfExtent - vec2(radius), vec2(0.0));
    vec2 q = abs(vLocalPosition) - innerHalfExtent;
    float signedDistance = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;

    // 以 1 像素宽度做覆盖率抗锯齿。
    float coverage = clamp(0.5 - signedDistance, 0.0, 1.0);
    if (borderWidth > 0.0) {
        // 描边沿矩形内侧绘制：保留外沿覆盖率，扣除线宽以内区域。
        float innerCoverage = clamp(0.5 - signedDistance - borderWidth, 0.0, 1.0);
        coverage = clamp(coverage - innerCoverage, 0.0, 1.0);
    }

    oColor = vec4(vColor.rgb, vColor.a * coverage);
}
)GLSL";

// 纹理管线顶点着色器：位置为左上原点的像素坐标，UV 为归一化坐标，
// 颜色用于文字着色或图片不透明度调制。
const char kTextureVertexShaderSource[] = R"GLSL(
#version 330 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aShape;   // 中心 x、中心 y、半宽、半高
layout(location = 4) in float aRadius; // 图片圆角半径，字形为 0

uniform vec2 uViewportSize;

out vec2 vTexCoord;
out vec4 vColor;
out vec2 vLocalPosition;
out vec2 vHalfExtent;
out float vRadius;

void main() {
    gl_Position = vec4(aPosition.x / uViewportSize.x * 2.0 - 1.0,
                       1.0 - aPosition.y / uViewportSize.y * 2.0,
                       0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
    vLocalPosition = aPosition - aShape.xy;
    vHalfExtent = aShape.zw;
    vRadius = aRadius;
}
)GLSL";

// 字形：图集为 R8 单通道纹理，仅取红色通道作为覆盖率。
const char kGlyphFragmentShaderSource[] = R"GLSL(
#version 330 core

uniform sampler2D uTexture;

in vec2 vTexCoord;
in vec4 vColor;

out vec4 oColor;

void main() {
    float coverage = texture(uTexture, vTexCoord).r;
    oColor = vec4(vColor.rgb, vColor.a * coverage);
}
)GLSL";

// 图片：RGBA 纹理；圆角半径大于 0 时按距离场裁剪，边缘以 1 像素宽度抗锯齿。
const char kImageFragmentShaderSource[] = R"GLSL(
#version 330 core

uniform sampler2D uTexture;

in vec2 vTexCoord;
in vec4 vColor;
in vec2 vLocalPosition;
in vec2 vHalfExtent;
in float vRadius;

out vec4 oColor;

void main() {
    vec4 texel = texture(uTexture, vTexCoord);
    float coverage = 1.0;
    if (vRadius > 0.0) {
        vec2 innerHalfExtent = max(vHalfExtent - vec2(vRadius), vec2(0.0));
        vec2 q = abs(vLocalPosition) - innerHalfExtent;
        float signedDistance = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - vRadius;
        coverage = clamp(0.5 - signedDistance, 0.0, 1.0);
    }
    oColor = vec4(texel.rgb, texel.a * vColor.a * coverage);
}
)GLSL";

// -------------------------------------------------------------------------------
// CPU 侧数据结构。
// -------------------------------------------------------------------------------

// 纹理管线顶点；步长 64 字节，与 VAO 属性偏移对应。
struct TextureVertex {
    float position[2];   // 偏移 0：目标像素坐标，左上原点
    float texcoord[2];   // 偏移 8：归一化纹理坐标
    float color[4];      // 偏移 16：调制颜色
    float shape[4];      // 偏移 32：中心 x、中心 y、半宽、半高
    float radius;        // 偏移 48：图片圆角半径
    float padding[3];    // 偏移 52：凑齐 64 字节
};

static_assert(sizeof(TextureVertex) == 16 * sizeof(float),
              "纹理顶点布局必须保持 64 字节步长");

constexpr std::size_t kPositionOffset = 0;
constexpr std::size_t kTexCoordOffset = 2 * sizeof(float);
constexpr std::size_t kColorOffset = 4 * sizeof(float);
constexpr std::size_t kShapeOffset = 8 * sizeof(float);
constexpr std::size_t kRadiusOffset = 12 * sizeof(float);

struct SolidUniforms {
    GLint viewport_size = -1;
    GLint rect = -1;
    GLint radius = -1;
    GLint border_width = -1;
    GLint color = -1;
};

struct TextureUniforms {
    GLint viewport_size = -1;
    GLint texture = -1;
};

// -------------------------------------------------------------------------------
// 着色器辅助。
// -------------------------------------------------------------------------------

GLuint CompileShader(GLenum shader_type, const char* source) {
    if (web::gl::pfnCreateShader == nullptr || source == nullptr) {
        return 0;
    }
    const GLuint shader = web::gl::pfnCreateShader(shader_type);
    if (shader == 0) {
        return 0;
    }
    const char* source_text = source;
    web::gl::pfnShaderSource(shader, 1, &source_text, nullptr);
    web::gl::pfnCompileShader(shader);

    GLint status = GL_FALSE;
    web::gl::pfnGetShaderiv(shader, kGlCompileStatus, &status);
    if (status == GL_FALSE) {
        web::gl::pfnDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkShaderProgram(const char* vertex_source, const char* fragment_source) {
    const GLuint vertex_shader = CompileShader(kGlVertexShader, vertex_source);
    if (vertex_shader == 0) {
        return 0;
    }
    const GLuint fragment_shader = CompileShader(kGlFragmentShader, fragment_source);
    if (fragment_shader == 0) {
        web::gl::pfnDeleteShader(vertex_shader);
        return 0;
    }

    const GLuint program = web::gl::pfnCreateProgram();
    if (program == 0) {
        web::gl::pfnDeleteShader(vertex_shader);
        web::gl::pfnDeleteShader(fragment_shader);
        return 0;
    }

    web::gl::pfnAttachShader(program, vertex_shader);
    web::gl::pfnAttachShader(program, fragment_shader);
    web::gl::pfnLinkProgram(program);

    GLint status = GL_FALSE;
    web::gl::pfnGetProgramiv(program, kGlLinkStatus, &status);

    // 链接完成后着色器对象即可删除，已附加的对象会保留到程序销毁。
    web::gl::pfnDeleteShader(vertex_shader);
    web::gl::pfnDeleteShader(fragment_shader);

    if (status == GL_FALSE) {
        web::gl::pfnDeleteProgram(program);
        return 0;
    }
    return program;
}

// 线性过滤、边缘夹取、无 mipmap 的二维纹理参数。
void SetLinearClampParameters() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
}

void AppendQuadVertices(std::vector<TextureVertex>* vertices, float x0, float y0, float x1,
                        float y1, float u0, float v0, float u1, float v1,
                        const Color& color, float corner_radius) {
    if (vertices == nullptr || x1 <= x0 || y1 <= y0) {
        return;
    }

    // 两个三角形覆盖同一矩形，顶点顺序与 GL_TRIANGLES 拓扑一致。
    const float positions[6][2] = {
        {x0, y0}, {x1, y0}, {x0, y1},
        {x1, y0}, {x1, y1}, {x0, y1},
    };
    const float texture_coords[6][2] = {
        {u0, v0}, {u1, v0}, {u0, v1},
        {u1, v0}, {u1, v1}, {u0, v1},
    };

    const float half_width = (x1 - x0) * 0.5f;
    const float half_height = (y1 - y0) * 0.5f;
    const float center_x = x0 + half_width;
    const float center_y = y0 + half_height;
    const float clamped_radius =
        std::max(0.0f, std::min(corner_radius, std::min(half_width, half_height)));

    for (std::size_t i = 0; i < 6; ++i) {
        TextureVertex vertex = {};
        vertex.position[0] = positions[i][0];
        vertex.position[1] = positions[i][1];
        vertex.texcoord[0] = texture_coords[i][0];
        vertex.texcoord[1] = texture_coords[i][1];
        vertex.color[0] = color.r;
        vertex.color[1] = color.g;
        vertex.color[2] = color.b;
        vertex.color[3] = color.a;
        vertex.shape[0] = center_x;
        vertex.shape[1] = center_y;
        vertex.shape[2] = half_width;
        vertex.shape[3] = half_height;
        vertex.radius = clamped_radius;
        vertices->push_back(vertex);
    }
}

std::atomic<std::uint32_t> g_window_class_serial{0};

// -------------------------------------------------------------------------------
// OpenGL 3.3 离屏渲染设备。
// -------------------------------------------------------------------------------

// 图片纹理记录：更新内容时可在同尺寸下使用子图像上传。
struct GlImageRecord {
    GLuint texture = 0;
    int width = 0;
    int height = 0;
};

class Gl33RenderDevice final : public RenderDevice {
public:
    Gl33RenderDevice() = default;
    ~Gl33RenderDevice() override { Shutdown(); }

    // 创建隐藏窗口、WGL 上下文与全部着色器资源；供工厂在返回对象前调用。
    bool InitializeDevice();

    bool Initialize(int width, int height) override;
    void Shutdown() override;
    bool Resize(int width, int height) override;

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    RenderBackend Kind() const override { return RenderBackend::OpenGL33; }

    void BeginFrame(const Color& clear_color) override;
    bool EndFrame() override;

    void FillRect(const RectF& rect, const Color& color) override;
    void FillRoundedRect(const RectF& rect, float radius, const Color& color) override;
    void StrokeRect(const RectF& rect, float width, const Color& color) override;
    void StrokeRoundedRect(const RectF& rect, float radius, float width,
                           const Color& color) override;

    void UpdateFontAtlas(const uint8_t* coverage, int width, int height) override;
    void DrawGlyphs(const GlyphQuad* quads, size_t count) override;

    ImageHandle CreateImage(const ImageData& image) override;
    void DrawImage(ImageHandle image, const RectF& rect, float opacity,
                   float corner_radius) override;
    void DrawImageSubRect(ImageHandle image, const RectF& rect, const RectF& uv, float opacity,
                          float corner_radius) override;
    void UpdateImage(ImageHandle image, const ImageData& image_data) override;

    void PushClip(const RectF& rect) override;
    void PopClip() override;

    bool ReadPixels(std::vector<uint8_t>* rgba_pixels) override;

private:
    bool CreateContext();
    void DestroyContext();

    bool CreateTargets();
    void ReleaseTarget();

    bool CreatePrograms();
    bool CreateGeometry();
    void ReleaseGlObjects();

    void DrawSolid(const RectF& rect, float radius, float border_width, const Color& color);
    void DrawTexturedVertices(GLuint program, const TextureUniforms& uniforms, GLuint texture);

    void ApplyScissor();
    RectF FullTargetRect() const;
    RectF CurrentClip() const;

    HWND window_ = nullptr;
    HDC device_context_ = nullptr;
    HGLRC render_context_ = nullptr;
    HINSTANCE instance_ = nullptr;
    std::wstring window_class_name_;
    bool window_class_registered_ = false;
    bool device_ready_ = false;

    int width_ = 0;
    int height_ = 0;

    GLuint framebuffer_ = 0;
    GLuint color_texture_ = 0;

    GLuint solid_program_ = 0;
    GLuint glyph_program_ = 0;
    GLuint image_program_ = 0;
    SolidUniforms solid_uniforms_;
    TextureUniforms glyph_uniforms_;
    TextureUniforms image_uniforms_;

    GLuint solid_vertex_array_ = 0;
    GLuint texture_vertex_array_ = 0;
    GLuint vertex_buffer_ = 0;

    GLuint atlas_texture_ = 0;
    int atlas_width_ = 0;
    int atlas_height_ = 0;

    std::unordered_map<ImageHandle, GlImageRecord> images_;
    ImageHandle next_image_handle_ = 1;

    std::vector<TextureVertex> scratch_vertices_;
    std::vector<RectF> clip_stack_;
};

bool Gl33RenderDevice::CreateContext() {
    if (render_context_ != nullptr) {
        return true;
    }

    instance_ = GetModuleHandleW(nullptr);
    const std::uint32_t serial =
        g_window_class_serial.fetch_add(1, std::memory_order_relaxed) + 1u;
    window_class_name_ = L"WebRenderGl33Offscreen_" + std::to_wstring(serial);

    // 隐藏的 1x1 弹出窗口，仅用于获得设备上下文与像素格式。
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance_;
    window_class.lpszClassName = window_class_name_.c_str();
    if (RegisterClassExW(&window_class) == 0) {
        return false;
    }
    window_class_registered_ = true;

    window_ = CreateWindowExW(0, window_class_name_.c_str(), L"", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, instance_, nullptr);
    if (window_ == nullptr) {
        return false;
    }

    device_context_ = GetDC(window_);
    if (device_context_ == nullptr) {
        return false;
    }

    PIXELFORMATDESCRIPTOR pixel_format = {};
    pixel_format.nSize = sizeof(pixel_format);
    pixel_format.nVersion = 1;
    pixel_format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pixel_format.iPixelType = PFD_TYPE_RGBA;
    pixel_format.cColorBits = 32;
    pixel_format.cAlphaBits = 8;
    pixel_format.cDepthBits = 0;
    pixel_format.cStencilBits = 0;
    pixel_format.iLayerType = PFD_MAIN_PLANE;

    const int chosen_format = ChoosePixelFormat(device_context_, &pixel_format);
    if (chosen_format == 0) {
        return false;
    }
    if (SetPixelFormat(device_context_, chosen_format, &pixel_format) == FALSE) {
        return false;
    }

    // 先创建兼容上下文，wglCreateContextAttribsARB 只能在此之后才能获取。
    const HGLRC legacy_context = wglCreateContext(device_context_);
    if (legacy_context == nullptr) {
        return false;
    }
    if (wglMakeCurrent(device_context_, legacy_context) == FALSE) {
        wglDeleteContext(legacy_context);
        return false;
    }

    HGLRC active_context = legacy_context;
    const bool has_context_attribs = web::gl::LoadWglContextExtensions();
    if (has_context_attribs) {
        const int attributes[] = {
            kWglContextMajorVersionArb, 3,
            kWglContextMinorVersionArb, 3,
            kWglContextProfileMaskArb, kWglContextCoreProfileBitArb,
            0,
        };
        const HGLRC core_context = web::gl::pfnCreateContextAttribsARB(
            device_context_, nullptr, attributes);
        if (core_context != nullptr) {
            if (wglMakeCurrent(device_context_, core_context) != FALSE) {
                wglDeleteContext(legacy_context);
                active_context = core_context;
            } else {
                wglDeleteContext(core_context);
            }
        }
    }

    // 兼容路径下也要求驱动实际提供 3.3 及以上版本，否则后续 330 着色器无法工作。
    render_context_ = active_context;
    const GLubyte* version_string = glGetString(GL_VERSION);
    if (version_string == nullptr) {
        return false;
    }
    int major_version = 0;
    int minor_version = 0;
    const char* version_text = reinterpret_cast<const char*>(version_string);
    char* cursor = nullptr;
    const long major_value = std::strtol(version_text, &cursor, 10);
    if (cursor == version_text || *cursor != '.') {
        return false;
    }
    const char* minor_text = cursor + 1;
    const long minor_value = std::strtol(minor_text, &cursor, 10);
    if (cursor == minor_text) {
        return false;
    }
    major_version = static_cast<int>(major_value);
    minor_version = static_cast<int>(minor_value);
    if (!(major_version >= 4 || (major_version == 3 && minor_version >= 3))) {
        return false;
    }
    return true;
}

void Gl33RenderDevice::DestroyContext() {
    if (render_context_ != nullptr) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(render_context_);
        render_context_ = nullptr;
    }
    if (device_context_ != nullptr) {
        ReleaseDC(window_, device_context_);
        device_context_ = nullptr;
    }
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (window_class_registered_) {
        UnregisterClassW(window_class_name_.c_str(), instance_);
        window_class_registered_ = false;
    }
    window_class_name_.clear();
    instance_ = nullptr;
    device_ready_ = false;
}

bool Gl33RenderDevice::CreatePrograms() {
    solid_program_ = LinkShaderProgram(kSolidVertexShaderSource, kSolidFragmentShaderSource);
    if (solid_program_ == 0) {
        return false;
    }
    glyph_program_ = LinkShaderProgram(kTextureVertexShaderSource, kGlyphFragmentShaderSource);
    if (glyph_program_ == 0) {
        return false;
    }
    image_program_ = LinkShaderProgram(kTextureVertexShaderSource, kImageFragmentShaderSource);
    if (image_program_ == 0) {
        return false;
    }

    solid_uniforms_.viewport_size =
        web::gl::pfnGetUniformLocation(solid_program_, "uViewportSize");
    solid_uniforms_.rect = web::gl::pfnGetUniformLocation(solid_program_, "uRect");
    solid_uniforms_.radius = web::gl::pfnGetUniformLocation(solid_program_, "uRadius");
    solid_uniforms_.border_width =
        web::gl::pfnGetUniformLocation(solid_program_, "uBorderWidth");
    solid_uniforms_.color = web::gl::pfnGetUniformLocation(solid_program_, "uColor");

    glyph_uniforms_.viewport_size =
        web::gl::pfnGetUniformLocation(glyph_program_, "uViewportSize");
    glyph_uniforms_.texture = web::gl::pfnGetUniformLocation(glyph_program_, "uTexture");

    image_uniforms_.viewport_size =
        web::gl::pfnGetUniformLocation(image_program_, "uViewportSize");
    image_uniforms_.texture = web::gl::pfnGetUniformLocation(image_program_, "uTexture");

    // 两个纹理程序都使用纹理单元 0。
    web::gl::pfnUseProgram(glyph_program_);
    web::gl::pfnUniform1i(glyph_uniforms_.texture, 0);
    web::gl::pfnUseProgram(image_program_);
    web::gl::pfnUniform1i(image_uniforms_.texture, 0);
    web::gl::pfnUseProgram(0);
    return true;
}

bool Gl33RenderDevice::CreateGeometry() {
    // 实心图元由 gl_VertexID 生成几何，仍需要一个顶点数组对象供核心模式绑定。
    web::gl::pfnGenVertexArrays(1, &solid_vertex_array_);
    if (solid_vertex_array_ == 0) {
        return false;
    }

    web::gl::pfnGenVertexArrays(1, &texture_vertex_array_);
    web::gl::pfnGenBuffers(1, &vertex_buffer_);
    if (texture_vertex_array_ == 0 || vertex_buffer_ == 0) {
        return false;
    }

    web::gl::pfnBindVertexArray(texture_vertex_array_);
    web::gl::pfnBindBuffer(kGlArrayBuffer, vertex_buffer_);

    web::gl::pfnEnableVertexAttribArray(0);
    web::gl::pfnVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                    static_cast<GLsizei>(sizeof(TextureVertex)),
                                    reinterpret_cast<const void*>(kPositionOffset));

    web::gl::pfnEnableVertexAttribArray(1);
    web::gl::pfnVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                    static_cast<GLsizei>(sizeof(TextureVertex)),
                                    reinterpret_cast<const void*>(kTexCoordOffset));

    web::gl::pfnEnableVertexAttribArray(2);
    web::gl::pfnVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE,
                                    static_cast<GLsizei>(sizeof(TextureVertex)),
                                    reinterpret_cast<const void*>(kColorOffset));

    web::gl::pfnEnableVertexAttribArray(3);
    web::gl::pfnVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE,
                                    static_cast<GLsizei>(sizeof(TextureVertex)),
                                    reinterpret_cast<const void*>(kShapeOffset));

    web::gl::pfnEnableVertexAttribArray(4);
    web::gl::pfnVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE,
                                    static_cast<GLsizei>(sizeof(TextureVertex)),
                                    reinterpret_cast<const void*>(kRadiusOffset));

    web::gl::pfnBindVertexArray(0);
    web::gl::pfnBindBuffer(kGlArrayBuffer, 0);
    return true;
}

bool Gl33RenderDevice::InitializeDevice() {
    if (device_ready_) {
        return true;
    }
    if (!CreateContext()) {
        DestroyContext();
        return false;
    }
    if (!web::gl::LoadOpenGL33()) {
        DestroyContext();
        return false;
    }
    if (!CreatePrograms()) {
        ReleaseGlObjects();
        DestroyContext();
        return false;
    }
    if (!CreateGeometry()) {
        ReleaseGlObjects();
        DestroyContext();
        return false;
    }
    device_ready_ = true;
    return true;
}

bool Gl33RenderDevice::Initialize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (!InitializeDevice()) {
        return false;
    }
    width_ = width;
    height_ = height;
    clip_stack_.clear();
    clip_stack_.push_back(FullTargetRect());
    return CreateTargets();
}

void Gl33RenderDevice::Shutdown() {
    ReleaseGlObjects();
    DestroyContext();

    width_ = 0;
    height_ = 0;
    next_image_handle_ = 1;
    clip_stack_.clear();
    scratch_vertices_.clear();
}

bool Gl33RenderDevice::Resize(int width, int height) {
    if (width <= 0 || height <= 0 || render_context_ == nullptr) {
        return false;
    }
    if (framebuffer_ != 0 && width_ == width && height_ == height) {
        return true;
    }

    width_ = width;
    height_ = height;
    clip_stack_.clear();
    clip_stack_.push_back(FullTargetRect());
    // 尺寸变化后此前的绘制坐标已失效，直接重建目标。
    return CreateTargets();
}

bool Gl33RenderDevice::CreateTargets() {
    ReleaseTarget();
    if (render_context_ == nullptr || width_ <= 0 || height_ <= 0) {
        return false;
    }
    if (web::gl::pfnGenFramebuffers == nullptr ||
        web::gl::pfnCheckFramebufferStatus == nullptr) {
        return false;
    }

    glGetError();  // 清除此前遗留的错误状态。

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenTextures(1, &color_texture_);
    if (color_texture_ == 0) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, color_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, kGlRgba8, static_cast<GLsizei>(width_),
                 static_cast<GLsizei>(height_), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        ReleaseTarget();
        return false;
    }
    SetLinearClampParameters();
    glBindTexture(GL_TEXTURE_2D, 0);

    web::gl::pfnGenFramebuffers(1, &framebuffer_);
    if (framebuffer_ == 0) {
        ReleaseTarget();
        return false;
    }
    web::gl::pfnBindFramebuffer(kGlFramebuffer, framebuffer_);
    web::gl::pfnFramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                                     color_texture_, 0);
    const GLenum status = web::gl::pfnCheckFramebufferStatus(kGlFramebuffer);
    web::gl::pfnBindFramebuffer(kGlFramebuffer, 0);
    if (status != kGlFramebufferComplete) {
        ReleaseTarget();
        return false;
    }
    return true;
}

void Gl33RenderDevice::ReleaseTarget() {
    if (render_context_ != nullptr) {
        if (framebuffer_ != 0 && web::gl::pfnDeleteFramebuffers != nullptr) {
            web::gl::pfnDeleteFramebuffers(1, &framebuffer_);
        }
        if (color_texture_ != 0) {
            glDeleteTextures(1, &color_texture_);
        }
    }
    framebuffer_ = 0;
    color_texture_ = 0;
}

void Gl33RenderDevice::ReleaseGlObjects() {
    const bool has_context = render_context_ != nullptr;

    ReleaseTarget();

    if (has_context) {
        if (atlas_texture_ != 0) {
            glDeleteTextures(1, &atlas_texture_);
        }
        if (!images_.empty()) {
            std::vector<GLuint> textures;
            textures.reserve(images_.size());
            for (const auto& entry : images_) {
                if (entry.second.texture != 0) {
                    textures.push_back(entry.second.texture);
                }
            }
            if (!textures.empty()) {
                glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
            }
        }
        if (vertex_buffer_ != 0 && web::gl::pfnDeleteBuffers != nullptr) {
            web::gl::pfnDeleteBuffers(1, &vertex_buffer_);
        }
        if (solid_vertex_array_ != 0 && web::gl::pfnDeleteVertexArrays != nullptr) {
            web::gl::pfnDeleteVertexArrays(1, &solid_vertex_array_);
        }
        if (texture_vertex_array_ != 0 && web::gl::pfnDeleteVertexArrays != nullptr) {
            web::gl::pfnDeleteVertexArrays(1, &texture_vertex_array_);
        }
        if (solid_program_ != 0 && web::gl::pfnDeleteProgram != nullptr) {
            web::gl::pfnDeleteProgram(solid_program_);
        }
        if (glyph_program_ != 0 && web::gl::pfnDeleteProgram != nullptr) {
            web::gl::pfnDeleteProgram(glyph_program_);
        }
        if (image_program_ != 0 && web::gl::pfnDeleteProgram != nullptr) {
            web::gl::pfnDeleteProgram(image_program_);
        }
    }

    atlas_texture_ = 0;
    atlas_width_ = 0;
    atlas_height_ = 0;
    images_.clear();
    vertex_buffer_ = 0;
    solid_vertex_array_ = 0;
    texture_vertex_array_ = 0;
    solid_program_ = 0;
    glyph_program_ = 0;
    image_program_ = 0;
}

RectF Gl33RenderDevice::FullTargetRect() const {
    return RectF::FromXYWH(0.0f, 0.0f, static_cast<float>(width_),
                           static_cast<float>(height_));
}

RectF Gl33RenderDevice::CurrentClip() const {
    if (!clip_stack_.empty()) {
        return clip_stack_.back();
    }
    return FullTargetRect();
}

void Gl33RenderDevice::ApplyScissor() {
    const RectF clip = CurrentClip();

    // 与 Direct3D 后端同样按像素取整：左、上向外取整，右、下向内取整。
    const int left = static_cast<int>(std::floor(clip.left));
    const int top = static_cast<int>(std::floor(clip.top));
    const int right = static_cast<int>(std::ceil(clip.right));
    const int bottom = static_cast<int>(std::ceil(clip.bottom));

    const int clamped_left = std::max(0, std::min(left, width_));
    const int clamped_right = std::max(clamped_left, std::min(right, width_));
    const int clamped_top = std::max(0, std::min(top, height_));
    const int clamped_bottom = std::max(clamped_top, std::min(bottom, height_));

    // 裁剪矩形以左上角为原点，OpenGL 的裁剪原点在左下角，需要翻转 y。
    const int scissor_x = clamped_left;
    const int scissor_y = height_ - clamped_bottom;
    const int scissor_width = clamped_right - clamped_left;
    const int scissor_height = clamped_bottom - clamped_top;
    glScissor(static_cast<GLint>(scissor_x), static_cast<GLint>(scissor_y),
              static_cast<GLsizei>(scissor_width), static_cast<GLsizei>(scissor_height));
}

void Gl33RenderDevice::BeginFrame(const Color& clear_color) {
    clip_stack_.clear();
    clip_stack_.push_back(FullTargetRect());

    if (render_context_ == nullptr || framebuffer_ == 0) {
        return;
    }

    web::gl::pfnBindFramebuffer(kGlFramebuffer, framebuffer_);
    glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));

    // 重置本后端依赖的状态，避免上一帧或外部调用残留。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    web::gl::pfnUseProgram(0);
    web::gl::pfnBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    ApplyScissor();

    // 目标纹理始终按不透明画布处理，因此清屏颜色的 alpha 固定为 1。
    glClearColor(clear_color.r, clear_color.g, clear_color.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

bool Gl33RenderDevice::EndFrame() {
    if (render_context_ == nullptr || framebuffer_ == 0) {
        return false;
    }
    glFlush();
    return true;
}

void Gl33RenderDevice::DrawSolid(const RectF& rect, float radius, float border_width,
                                 const Color& color) {
    if (solid_program_ == 0 || render_context_ == nullptr) {
        return;
    }
    if (rect.IsEmpty() || !color.Visible()) {
        return;
    }

    const float rect_width = rect.Width();
    const float rect_height = rect.Height();
    const float half_min_extent = std::min(rect_width, rect_height) * 0.5f;
    const float clamped_radius = std::min(std::max(radius, 0.0f), half_min_extent);
    float clamped_border = 0.0f;
    if (border_width > 0.0f) {
        clamped_border = std::min(border_width, half_min_extent);
    }

    web::gl::pfnUseProgram(solid_program_);
    web::gl::pfnUniform2f(solid_uniforms_.viewport_size, static_cast<GLfloat>(width_),
                          static_cast<GLfloat>(height_));
    web::gl::pfnUniform4f(solid_uniforms_.rect, rect.left, rect.top, rect_width, rect_height);
    web::gl::pfnUniform1f(solid_uniforms_.radius, clamped_radius);
    web::gl::pfnUniform1f(solid_uniforms_.border_width, clamped_border);
    web::gl::pfnUniform4f(solid_uniforms_.color, color.r, color.g, color.b, color.a);

    web::gl::pfnBindVertexArray(solid_vertex_array_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    web::gl::pfnBindVertexArray(0);
}

void Gl33RenderDevice::DrawTexturedVertices(GLuint program, const TextureUniforms& uniforms,
                                            GLuint texture) {
    if (render_context_ == nullptr || program == 0 || texture == 0) {
        return;
    }
    if (scratch_vertices_.empty()) {
        return;
    }

    web::gl::pfnUseProgram(program);
    web::gl::pfnUniform2f(uniforms.viewport_size, static_cast<GLfloat>(width_),
                          static_cast<GLfloat>(height_));
    web::gl::pfnActiveTexture(kGlTexture0);
    glBindTexture(GL_TEXTURE_2D, texture);
    web::gl::pfnUniform1i(uniforms.texture, 0);

    web::gl::pfnBindVertexArray(texture_vertex_array_);
    web::gl::pfnBindBuffer(kGlArrayBuffer, vertex_buffer_);
    web::gl::pfnBufferData(kGlArrayBuffer,
                           static_cast<web::gl::GLsizeiptr>(scratch_vertices_.size() *
                                                            sizeof(TextureVertex)),
                           scratch_vertices_.data(), kGlStreamDraw);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(scratch_vertices_.size()));

    web::gl::pfnBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Gl33RenderDevice::FillRect(const RectF& rect, const Color& color) {
    DrawSolid(rect, 0.0f, 0.0f, color);
}

void Gl33RenderDevice::FillRoundedRect(const RectF& rect, float radius, const Color& color) {
    DrawSolid(rect, radius, 0.0f, color);
}

void Gl33RenderDevice::StrokeRect(const RectF& rect, float width, const Color& color) {
    DrawSolid(rect, 0.0f, width, color);
}

void Gl33RenderDevice::StrokeRoundedRect(const RectF& rect, float radius, float width,
                                         const Color& color) {
    DrawSolid(rect, radius, width, color);
}

void Gl33RenderDevice::UpdateFontAtlas(const uint8_t* coverage, int width, int height) {
    if (render_context_ == nullptr) {
        return;
    }

    // 传入空数据或无效尺寸时释放图集，后续文字绘制会被跳过。
    if (coverage == nullptr || width <= 0 || height <= 0) {
        if (atlas_texture_ != 0) {
            glDeleteTextures(1, &atlas_texture_);
        }
        atlas_texture_ = 0;
        atlas_width_ = 0;
        atlas_height_ = 0;
        return;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGetError();  // 清除此前遗留的错误状态，避免误判本次上传失败。

    if (atlas_texture_ != 0 && atlas_width_ == width && atlas_height_ == height) {
        glBindTexture(GL_TEXTURE_2D, atlas_texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width),
                        static_cast<GLsizei>(height), GL_RED, GL_UNSIGNED_BYTE, coverage);
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return;
    }

    // 尺寸变化时重建纹理与采样参数。
    if (atlas_texture_ != 0) {
        glDeleteTextures(1, &atlas_texture_);
        atlas_texture_ = 0;
        atlas_width_ = 0;
        atlas_height_ = 0;
    }

    glGenTextures(1, &atlas_texture_);
    if (atlas_texture_ == 0) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return;
    }

    glBindTexture(GL_TEXTURE_2D, atlas_texture_);
    SetLinearClampParameters();
    glTexImage2D(GL_TEXTURE_2D, 0, kGlR8, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RED, GL_UNSIGNED_BYTE, coverage);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (glGetError() != GL_NO_ERROR) {
        if (atlas_texture_ != 0) {
            glDeleteTextures(1, &atlas_texture_);
        }
        atlas_texture_ = 0;
        atlas_width_ = 0;
        atlas_height_ = 0;
        return;
    }
    atlas_width_ = width;
    atlas_height_ = height;
}

void Gl33RenderDevice::DrawGlyphs(const GlyphQuad* quads, size_t count) {
    if (quads == nullptr || count == 0 || atlas_texture_ == 0 || glyph_program_ == 0) {
        return;
    }

    scratch_vertices_.clear();
    scratch_vertices_.reserve(count * 6);
    for (size_t i = 0; i < count; ++i) {
        const GlyphQuad& glyph = quads[i];
        if (glyph.x1 <= glyph.x0 || glyph.y1 <= glyph.y0 || !glyph.color.Visible()) {
            continue;
        }
        AppendQuadVertices(&scratch_vertices_, glyph.x0, glyph.y0, glyph.x1, glyph.y1,
                           glyph.u0, glyph.v0, glyph.u1, glyph.v1, glyph.color, 0.0f);
    }

    if (scratch_vertices_.empty()) {
        return;
    }
    DrawTexturedVertices(glyph_program_, glyph_uniforms_, atlas_texture_);
}

ImageHandle Gl33RenderDevice::CreateImage(const ImageData& image) {
    if (render_context_ == nullptr || !image.Valid()) {
        return 0;
    }

    GLuint texture = 0;
    glGetError();  // 清除此前遗留的错误状态，避免误判本次上传失败。
    glGenTextures(1, &texture);
    if (texture == 0) {
        return 0;
    }

    // 顶行优先的 RGBA8 数据直接上传：第一行对应 v=0，与纹理坐标约定一致。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, texture);
    SetLinearClampParameters();
    glTexImage2D(GL_TEXTURE_2D, 0, kGlRgba8, static_cast<GLsizei>(image.width),
                 static_cast<GLsizei>(image.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 image.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }

    const ImageHandle handle = next_image_handle_;
    ++next_image_handle_;
    GlImageRecord record;
    record.texture = texture;
    record.width = image.width;
    record.height = image.height;
    images_.emplace(handle, record);
    return handle;
}

void Gl33RenderDevice::UpdateImage(ImageHandle image, const ImageData& image_data) {
    if (render_context_ == nullptr || image == 0 || !image_data.Valid()) {
        return;
    }
    const auto found = images_.find(image);
    if (found == images_.end()) {
        return;
    }

    if (found->second.width == image_data.width && found->second.height == image_data.height) {
        // 同尺寸时只更新纹理内容，避免重建纹理对象。
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, found->second.texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(image_data.width),
                        static_cast<GLsizei>(image_data.height), GL_RGBA, GL_UNSIGNED_BYTE,
                        image_data.pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // 尺寸变化时重建纹理，句柄保持不变。
    GLuint texture = 0;
    glGetError();
    glGenTextures(1, &texture);
    if (texture == 0) {
        return;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, texture);
    SetLinearClampParameters();
    glTexImage2D(GL_TEXTURE_2D, 0, kGlRgba8, static_cast<GLsizei>(image_data.width),
                 static_cast<GLsizei>(image_data.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 image_data.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return;
    }
    if (found->second.texture != 0) {
        glDeleteTextures(1, &found->second.texture);
    }
    found->second.texture = texture;
    found->second.width = image_data.width;
    found->second.height = image_data.height;
}

void Gl33RenderDevice::DrawImage(ImageHandle image, const RectF& rect, float opacity,
                                 float corner_radius) {
    DrawImageSubRect(image, rect, RectF::FromLTRB(0.0f, 0.0f, 1.0f, 1.0f), opacity,
                     corner_radius);
}

void Gl33RenderDevice::DrawImageSubRect(ImageHandle image, const RectF& rect, const RectF& uv,
                                        float opacity, float corner_radius) {
    if (image == 0 || !(opacity > 0.0f)) {
        return;
    }
    const auto found = images_.find(image);
    if (found == images_.end()) {
        return;
    }

    const float clamped_opacity = std::min(opacity, 1.0f);
    scratch_vertices_.clear();
    scratch_vertices_.reserve(6);
    AppendQuadVertices(&scratch_vertices_, rect.left, rect.top, rect.right, rect.bottom,
                       uv.left, uv.top, uv.right, uv.bottom,
                       Rgba(1.0f, 1.0f, 1.0f, clamped_opacity), corner_radius);
    if (scratch_vertices_.empty()) {
        return;
    }
    DrawTexturedVertices(image_program_, image_uniforms_, found->second.texture);
}

void Gl33RenderDevice::PushClip(const RectF& rect) {
    if (clip_stack_.empty()) {
        clip_stack_.push_back(FullTargetRect());
    }
    clip_stack_.push_back(clip_stack_.back().Intersect(rect));
    if (render_context_ != nullptr) {
        ApplyScissor();
    }
}

void Gl33RenderDevice::PopClip() {
    if (clip_stack_.size() > 1) {
        clip_stack_.pop_back();
    }
    if (render_context_ != nullptr) {
        ApplyScissor();
    }
}

bool Gl33RenderDevice::ReadPixels(std::vector<uint8_t>* rgba_pixels) {
    if (rgba_pixels == nullptr) {
        return false;
    }
    if (render_context_ == nullptr || framebuffer_ == 0 || width_ <= 0 || height_ <= 0) {
        return false;
    }

    web::gl::pfnBindFramebuffer(kGlFramebuffer, framebuffer_);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(kGlColorAttachment0);

    const std::size_t row_bytes = static_cast<std::size_t>(width_) * 4u;
    std::vector<uint8_t> bottom_up(row_bytes * static_cast<std::size_t>(height_));
    glReadPixels(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_),
                 GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());

    // glReadPixels 返回底行优先，这里翻转为顶行优先输出。
    rgba_pixels->resize(row_bytes * static_cast<std::size_t>(height_));
    for (int y = 0; y < height_; ++y) {
        const std::size_t source_row =
            static_cast<std::size_t>(height_ - 1 - y) * row_bytes;
        const std::size_t destination_row = static_cast<std::size_t>(y) * row_bytes;
        std::memcpy(rgba_pixels->data() + destination_row, bottom_up.data() + source_row,
                    row_bytes);
        // 目标纹理按不透明画布处理，回读结果 alpha 固定为 255（非预乘）。
        for (int x = 0; x < width_; ++x) {
            (*rgba_pixels)[destination_row + static_cast<std::size_t>(x) * 4u + 3u] = 255;
        }
    }
    return true;
}

}  // namespace

std::unique_ptr<RenderDevice> CreateOpenGL33RenderDevice() {
    std::unique_ptr<Gl33RenderDevice> device = std::make_unique<Gl33RenderDevice>();
    if (!device->InitializeDevice()) {
        return nullptr;
    }
    return device;
}

}  // namespace web
