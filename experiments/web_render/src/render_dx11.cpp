// Direct3D 11 离屏渲染后端实现。
// 该后端只渲染到离屏纹理，不创建窗口，也不使用交换链；像素通过 ReadPixels 回读到 CPU。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "web/render.h"
#include "web/render_backends.h"

namespace web {
namespace {

using Microsoft::WRL::ComPtr;

// -------------------------------------------------------------------------------
// 内嵌 HLSL 源码：运行时由 D3DCompile 编译，不依赖外部着色器文件。
// -------------------------------------------------------------------------------

// 实心图元：几何通过实例数据提供，像素着色器以有向距离场计算圆角矩形覆盖率。
const char kSolidShaderSource[] = R"(
cbuffer FrameConstants : register(b0) {
    float2 g_inverse_size;
    float2 g_padding;
};

struct InstanceInput {
    float4 rect : RECT;              // 左上角坐标与宽高（像素）
    float radius : RADIUS;           // 圆角半径（像素）
    float border_width : BORDER;     // 内描边线宽；0 表示填充
    float4 color : COLOR0;           // 非预乘 RGBA
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float2 local_position : TEXCOORD0;  // 相对矩形中心的像素坐标
    float2 half_extent : TEXCOORD1;     // 半宽与半高
    float2 shape : TEXCOORD2;           // x 为半径，y 为内描边线宽
    float4 color : COLOR0;
};

VertexOutput VSMain(uint vertex_id : SV_VertexID, InstanceInput instance) {
    // 单位四边形四个角：0 到 3 依次为 (0,0)、(1,0)、(0,1)、(1,1)，与三角带拓扑一致。
    float2 corner = float2(float(vertex_id & 1), float(vertex_id >> 1));
    float2 position = instance.rect.xy + corner * instance.rect.zw;

    VertexOutput output;
    output.position = float4(position.x * g_inverse_size.x * 2.0 - 1.0,
                             1.0 - position.y * g_inverse_size.y * 2.0,
                             0.0, 1.0);
    output.local_position = corner * instance.rect.zw - instance.rect.zw * 0.5;
    output.half_extent = instance.rect.zw * 0.5;
    output.shape = float2(instance.radius, instance.border_width);
    output.color = instance.color;
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
    float radius = input.shape.x;
    float border_width = input.shape.y;

    // 圆角矩形有向距离场：内部为负，外部为正。
    float2 inner_half_extent = max(input.half_extent - radius, float2(0.0, 0.0));
    float2 q = abs(input.local_position) - inner_half_extent;
    float signed_distance = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - radius;

    // 以 1 像素宽度做覆盖率抗锯齿。
    float coverage = saturate(0.5 - signed_distance);
    if (border_width > 0.0) {
        // 描边沿矩形内侧绘制：保留外沿覆盖率，扣除线宽以内区域。
        float inner_coverage = saturate(0.5 - signed_distance - border_width);
        coverage = saturate(coverage - inner_coverage);
    }

    float alpha = input.color.a * coverage;
    // 输出预乘 alpha，与预乘混合状态匹配。
    return float4(input.color.rgb * alpha, alpha);
}
)";

// 纹理管线：同一顶点着色器与两个像素着色器入口，分别用于 A8 字形图集与 RGBA 图片。
const char kTextureShaderSource[] = R"(
cbuffer FrameConstants : register(b0) {
    float2 g_inverse_size;
    float2 g_padding;
};

struct VertexInput {
    float2 position : POSITION;   // 目标纹理像素坐标，左上原点
    float2 uv : TEXCOORD0;        // 归一化纹理坐标
    float4 color : COLOR0;        // 调制颜色：文字用颜色，图片用 opacity
    float4 shape : TEXCOORD1;     // 中心 x、中心 y、半宽、半高
    float radius : TEXCOORD2;     // 图片圆角半径，字形为 0
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float2 local_position : TEXCOORD1;
    float2 half_extent : TEXCOORD2;
    float radius : TEXCOORD3;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position.x * g_inverse_size.x * 2.0 - 1.0,
                             1.0 - input.position.y * g_inverse_size.y * 2.0,
                             0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    output.local_position = input.position - input.shape.xy;
    output.half_extent = input.shape.zw;
    output.radius = input.radius;
    return output;
}

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

// 字形图集为 R8_UNORM，仅取红色通道作为覆盖率。
float4 PSGlyph(VertexOutput input) : SV_TARGET {
    float coverage = g_texture.Sample(g_sampler, input.uv).r;
    float alpha = coverage * input.color.a;
    return float4(input.color.rgb * alpha, alpha);
}

// RGBA 图片：圆角半径大于 0 时按距离场裁剪，边缘以 1 像素宽度抗锯齿。
float4 PSImage(VertexOutput input) : SV_TARGET {
    float4 texel = g_texture.Sample(g_sampler, input.uv);
    float coverage = 1.0;
    if (input.radius > 0.0) {
        float2 inner_half_extent = max(input.half_extent - input.radius, float2(0.0, 0.0));
        float2 q = abs(input.local_position) - inner_half_extent;
        float signed_distance = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) -
                                input.radius;
        coverage = saturate(0.5 - signed_distance);
    }
    float alpha = texel.a * input.color.a * coverage;
    return float4(texel.rgb * alpha, alpha);
}
)";

// -------------------------------------------------------------------------------
// CPU 侧数据结构，需与 HLSL 输入签名逐元素对应。
// -------------------------------------------------------------------------------

// 帧常量：把像素坐标转换为裁剪空间坐标。
struct FrameConstants {
    float inverse_width;
    float inverse_height;
    float padding[2];
};

// 实心图元实例；步长 48 字节，各字段偏移与输入布局保持一致。
struct SolidInstance {
    float rect[4];        // 偏移 0：left、top、width、height
    float radius;         // 偏移 16：圆角半径
    float border_width;   // 偏移 20：内描边线宽
    float padding[2];     // 偏移 24：保留，凑齐 16 字节对齐
    float color[4];       // 偏移 32：非预乘 RGBA
};

// 纹理管线顶点；步长 64 字节，与输入布局逐元素对应。
struct TextureVertex {
    float position[2];  // 偏移 0
    float uv[2];        // 偏移 8
    float color[4];     // 偏移 16
    float shape[4];     // 偏移 32：中心 x、中心 y、半宽、半高
    float radius;       // 偏移 48：图片圆角半径
    float padding[3];   // 偏移 52：凑齐 64 字节
};

static_assert(sizeof(TextureVertex) == 16 * sizeof(float),
              "纹理顶点布局必须保持 64 字节步长");

enum class DrawKind {
    Solid,
    Glyph,
    Image,
};

// 单条绘制命令；裁剪矩形在调用时确定，保证 PushClip/PopClip 之间的绘制互不影响。
struct DrawCommand {
    DrawKind kind = DrawKind::Solid;
    size_t first = 0;               // 实例索引或顶点索引
    size_t count = 0;               // 实例数量或顶点数量
    ImageHandle image = 0;          // 图片命令使用的句柄
    LONG scissor_left = 0;
    LONG scissor_top = 0;
    LONG scissor_right = 0;
    LONG scissor_bottom = 0;
};

// -------------------------------------------------------------------------------
// 着色器编译辅助。
// -------------------------------------------------------------------------------

bool CompileShaderBlob(const char* source, const char* entry_point, const char* target,
                       ComPtr<ID3DBlob>* blob) {
    if (source == nullptr || entry_point == nullptr || target == nullptr || blob == nullptr) {
        return false;
    }
    ComPtr<ID3DBlob> compiled;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "web_render_dx11", nullptr, nullptr,
                                  entry_point, target,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  compiled.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || compiled == nullptr) {
        return false;
    }
    *blob = std::move(compiled);
    return true;
}

// -------------------------------------------------------------------------------
// 离屏渲染设备。
// -------------------------------------------------------------------------------

// 图片纹理记录：更新内容时可在同尺寸下复用纹理对象。
struct TextureRecord {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
};

class Dx11RenderDevice final : public RenderDevice {
public:
    // 创建 D3D11 设备、着色器与状态对象；供工厂在返回对象前调用。
    bool InitializeDevice();

    bool Initialize(int width, int height) override;
    void Shutdown() override;
    bool Resize(int width, int height) override;

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    RenderBackend Kind() const override { return RenderBackend::Direct3D11; }

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
    bool CreateTargets();
    void ReleaseTarget();
    bool CreateStates();
    bool CreateFrameConstants();

    void ResetFrameData();
    void UpdateFrameConstants();
    RectF FullTargetRect() const;
    RectF CurrentClip() const;
    DrawCommand CurrentScissorCommand(DrawKind kind) const;

    void AppendSolid(const RectF& rect, float radius, float border_width, const Color& color);
    void AppendQuadVertices(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                            float v1, const Color& color, float corner_radius);
    void AppendTextureQuad(const RectF& rect, float u0, float v0, float u1, float v1,
                           const Color& color, DrawKind kind, ImageHandle image,
                           float corner_radius);

    bool EnsureVertexBuffer(ComPtr<ID3D11Buffer>& buffer, size_t& capacity, size_t required,
                            size_t stride);
    bool UploadGeometry();
    void BindSolidPipeline();
    void BindTexturePipeline(ID3D11ShaderResourceView* view, bool glyph);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_10_0;

    int width_ = 0;
    int height_ = 0;

    ComPtr<ID3D11Texture2D> target_texture_;
    ComPtr<ID3D11RenderTargetView> target_view_;
    ComPtr<ID3D11Texture2D> staging_texture_;

    ComPtr<ID3D11VertexShader> solid_vertex_shader_;
    ComPtr<ID3D11VertexShader> texture_vertex_shader_;
    ComPtr<ID3D11PixelShader> solid_pixel_shader_;
    ComPtr<ID3D11PixelShader> glyph_pixel_shader_;
    ComPtr<ID3D11PixelShader> image_pixel_shader_;
    ComPtr<ID3D11InputLayout> solid_input_layout_;
    ComPtr<ID3D11InputLayout> texture_input_layout_;

    ComPtr<ID3D11Buffer> frame_constants_;
    ComPtr<ID3D11BlendState> blend_state_;
    ComPtr<ID3D11RasterizerState> rasterizer_state_;
    ComPtr<ID3D11SamplerState> sampler_state_;

    ComPtr<ID3D11Buffer> instance_buffer_;
    size_t instance_capacity_ = 0;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    size_t vertex_capacity_ = 0;

    ComPtr<ID3D11Texture2D> atlas_texture_;
    ComPtr<ID3D11ShaderResourceView> atlas_view_;
    int atlas_width_ = 0;
    int atlas_height_ = 0;

    std::unordered_map<ImageHandle, TextureRecord> images_;
    ImageHandle next_image_handle_ = 1;

    std::vector<SolidInstance> instances_;
    std::vector<TextureVertex> vertices_;
    std::vector<DrawCommand> commands_;
    std::vector<RectF> clip_stack_;
};

bool Dx11RenderDevice::InitializeDevice() {
    if (device_ != nullptr && context_ != nullptr) {
        return true;
    }

    const D3D11_CREATE_DEVICE_FLAG create_flags = static_cast<D3D11_CREATE_DEVICE_FLAG>(
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED);

    // 先申请 11_0，失败后回退 10_0。
    const D3D_FEATURE_LEVEL requested_levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> created_device;
    ComPtr<ID3D11DeviceContext> created_context;
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_10_0;
    bool created = false;
    for (const D3D_FEATURE_LEVEL level : requested_levels) {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             create_flags, &level, 1, D3D11_SDK_VERSION,
                                             created_device.GetAddressOf(), &created_level,
                                             created_context.GetAddressOf());
        if (SUCCEEDED(hr) && created_device != nullptr && created_context != nullptr) {
            created = true;
            break;
        }
        created_device.Reset();
        created_context.Reset();
    }
    if (!created) {
        return false;
    }

    device_ = std::move(created_device);
    context_ = std::move(created_context);
    feature_level_ = created_level;

    const bool support_shader_model_5 = feature_level_ >= D3D_FEATURE_LEVEL_11_0;
    const char* vertex_vs_target = support_shader_model_5 ? "vs_5_0" : "vs_4_0";
    const char* pixel_vs_target = support_shader_model_5 ? "ps_5_0" : "ps_4_0";

    ComPtr<ID3DBlob> solid_vs_blob;
    ComPtr<ID3DBlob> solid_ps_blob;
    ComPtr<ID3DBlob> texture_vs_blob;
    ComPtr<ID3DBlob> glyph_ps_blob;
    ComPtr<ID3DBlob> image_ps_blob;
    if (!CompileShaderBlob(kSolidShaderSource, "VSMain", vertex_vs_target, &solid_vs_blob)) {
        return false;
    }
    if (!CompileShaderBlob(kSolidShaderSource, "PSMain", pixel_vs_target, &solid_ps_blob)) {
        return false;
    }
    if (!CompileShaderBlob(kTextureShaderSource, "VSMain", vertex_vs_target, &texture_vs_blob)) {
        return false;
    }
    if (!CompileShaderBlob(kTextureShaderSource, "PSGlyph", pixel_vs_target, &glyph_ps_blob)) {
        return false;
    }
    if (!CompileShaderBlob(kTextureShaderSource, "PSImage", pixel_vs_target, &image_ps_blob)) {
        return false;
    }

    if (FAILED(device_->CreateVertexShader(solid_vs_blob->GetBufferPointer(),
                                           solid_vs_blob->GetBufferSize(), nullptr,
                                           solid_vertex_shader_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(solid_ps_blob->GetBufferPointer(),
                                          solid_ps_blob->GetBufferSize(), nullptr,
                                          solid_pixel_shader_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(device_->CreateVertexShader(texture_vs_blob->GetBufferPointer(),
                                           texture_vs_blob->GetBufferSize(), nullptr,
                                           texture_vertex_shader_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(glyph_ps_blob->GetBufferPointer(),
                                          glyph_ps_blob->GetBufferSize(), nullptr,
                                          glyph_pixel_shader_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(image_ps_blob->GetBufferPointer(),
                                          image_ps_blob->GetBufferSize(), nullptr,
                                          image_pixel_shader_.GetAddressOf()))) {
        return false;
    }

    // 实例数据的顶点缓冲位于槽位 1，几何由 SV_VertexID 生成，不需要槽位 0 的顶点缓冲。
    const D3D11_INPUT_ELEMENT_DESC solid_elements[] = {
        {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"RADIUS", 0, DXGI_FORMAT_R32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BORDER", 0, DXGI_FORMAT_R32_FLOAT, 1, 20, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    if (FAILED(device_->CreateInputLayout(solid_elements, 4, solid_vs_blob->GetBufferPointer(),
                                          solid_vs_blob->GetBufferSize(),
                                          solid_input_layout_.GetAddressOf()))) {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC texture_elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device_->CreateInputLayout(texture_elements, 5,
                                          texture_vs_blob->GetBufferPointer(),
                                          texture_vs_blob->GetBufferSize(),
                                          texture_input_layout_.GetAddressOf()))) {
        return false;
    }

    return CreateStates() && CreateFrameConstants();
}

bool Dx11RenderDevice::CreateStates() {
    if (device_ == nullptr) {
        return false;
    }

    // 预乘 alpha 混合：源颜色已乘过 alpha，目标保留剩余权重。
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.AlphaToCoverageEnable = FALSE;
    blend_desc.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& target_blend = blend_desc.RenderTarget[0];
    target_blend.BlendEnable = TRUE;
    target_blend.SrcBlend = D3D11_BLEND_ONE;
    target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target_blend.BlendOp = D3D11_BLEND_OP_ADD;
    target_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
    target_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target_blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blend_desc, blend_state_.GetAddressOf()))) {
        return false;
    }

    // 关闭背面剔除，启用 scissor 裁剪；裁剪矩形以左上原点定义。
    D3D11_RASTERIZER_DESC rasterizer_desc = {};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.FrontCounterClockwise = FALSE;
    rasterizer_desc.DepthBias = 0;
    rasterizer_desc.DepthBiasClamp = 0.0f;
    rasterizer_desc.SlopeScaledDepthBias = 0.0f;
    rasterizer_desc.DepthClipEnable = TRUE;
    rasterizer_desc.ScissorEnable = TRUE;
    rasterizer_desc.MultisampleEnable = FALSE;
    rasterizer_desc.AntialiasedLineEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rasterizer_desc,
                                              rasterizer_state_.GetAddressOf()))) {
        return false;
    }

    // 图集与图片都使用线性过滤与边缘夹取，避免缩放时出现接缝。
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MipLODBias = 0.0f;
    sampler_desc.MaxAnisotropy = 1;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.BorderColor[0] = 0.0f;
    sampler_desc.BorderColor[1] = 0.0f;
    sampler_desc.BorderColor[2] = 0.0f;
    sampler_desc.BorderColor[3] = 0.0f;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, sampler_state_.GetAddressOf()))) {
        return false;
    }

    return true;
}

bool Dx11RenderDevice::CreateFrameConstants() {
    if (device_ == nullptr) {
        return false;
    }
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(sizeof(FrameConstants));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    return SUCCEEDED(device_->CreateBuffer(&desc, nullptr, frame_constants_.GetAddressOf()));
}

bool Dx11RenderDevice::CreateTargets() {
    ReleaseTarget();
    if (device_ == nullptr || width_ <= 0 || height_ <= 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC target_desc = {};
    target_desc.Width = static_cast<UINT>(width_);
    target_desc.Height = static_cast<UINT>(height_);
    target_desc.MipLevels = 1;
    target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.SampleDesc.Quality = 0;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    target_desc.CPUAccessFlags = 0;
    target_desc.MiscFlags = 0;

    if (FAILED(device_->CreateTexture2D(&target_desc, nullptr,
                                        target_texture_.GetAddressOf()))) {
        ReleaseTarget();
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(target_texture_.Get(), nullptr,
                                               target_view_.GetAddressOf()))) {
        ReleaseTarget();
        return false;
    }

    // 回读用的暂存纹理，CPU 只读访问。
    D3D11_TEXTURE2D_DESC staging_desc = target_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(&staging_desc, nullptr,
                                        staging_texture_.GetAddressOf()))) {
        ReleaseTarget();
        return false;
    }
    return true;
}

void Dx11RenderDevice::ReleaseTarget() {
    target_view_.Reset();
    target_texture_.Reset();
    staging_texture_.Reset();
}

bool Dx11RenderDevice::Initialize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (device_ == nullptr && !InitializeDevice()) {
        return false;
    }
    width_ = width;
    height_ = height;
    return CreateTargets();
}

void Dx11RenderDevice::Shutdown() {
    if (context_ != nullptr) {
        context_->ClearState();
        context_->Flush();
    }

    images_.clear();
    next_image_handle_ = 1;

    atlas_view_.Reset();
    atlas_texture_.Reset();
    atlas_width_ = 0;
    atlas_height_ = 0;

    ReleaseTarget();

    instance_buffer_.Reset();
    instance_capacity_ = 0;
    vertex_buffer_.Reset();
    vertex_capacity_ = 0;

    frame_constants_.Reset();
    blend_state_.Reset();
    rasterizer_state_.Reset();
    sampler_state_.Reset();

    solid_input_layout_.Reset();
    texture_input_layout_.Reset();
    solid_vertex_shader_.Reset();
    texture_vertex_shader_.Reset();
    solid_pixel_shader_.Reset();
    glyph_pixel_shader_.Reset();
    image_pixel_shader_.Reset();

    context_.Reset();
    device_.Reset();

    instances_.clear();
    vertices_.clear();
    commands_.clear();
    clip_stack_.clear();

    width_ = 0;
    height_ = 0;
}

bool Dx11RenderDevice::Resize(int width, int height) {
    if (width <= 0 || height <= 0 || context_ == nullptr) {
        return false;
    }
    if (target_view_ != nullptr && width_ == width && height_ == height) {
        return true;
    }
    width_ = width;
    height_ = height;
    // 尺寸变化后此前的绘制命令坐标已失效，直接丢弃。
    ResetFrameData();
    return CreateTargets();
}

void Dx11RenderDevice::ResetFrameData() {
    instances_.clear();
    vertices_.clear();
    commands_.clear();
    clip_stack_.clear();
    clip_stack_.push_back(FullTargetRect());
}

void Dx11RenderDevice::BeginFrame(const Color& clear_color) {
    ResetFrameData();
    if (context_ == nullptr || target_view_ == nullptr) {
        return;
    }

    ID3D11RenderTargetView* views[1] = {target_view_.Get()};
    context_->OMSetRenderTargets(1, views, nullptr);
    context_->OMSetDepthStencilState(nullptr, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_state_.Get());
    context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);

    const D3D11_RECT full_rect = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    context_->RSSetScissorRects(1, &full_rect);

    // 目标纹理始终按不透明画布处理，因此清屏颜色的 alpha 固定为 1。
    const float clear_rgba[4] = {clear_color.r, clear_color.g, clear_color.b, 1.0f};
    context_->ClearRenderTargetView(target_view_.Get(), clear_rgba);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetConstantBuffers(0, 1, frame_constants_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_state_.GetAddressOf());

    UpdateFrameConstants();
}

void Dx11RenderDevice::UpdateFrameConstants() {
    if (context_ == nullptr || frame_constants_ == nullptr) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(frame_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    FrameConstants* constants = static_cast<FrameConstants*>(mapped.pData);
    constants->inverse_width = width_ > 0 ? 1.0f / static_cast<float>(width_) : 0.0f;
    constants->inverse_height = height_ > 0 ? 1.0f / static_cast<float>(height_) : 0.0f;
    constants->padding[0] = 0.0f;
    constants->padding[1] = 0.0f;
    context_->Unmap(frame_constants_.Get(), 0);
}

RectF Dx11RenderDevice::FullTargetRect() const {
    return RectF::FromXYWH(0.0f, 0.0f, static_cast<float>(width_),
                           static_cast<float>(height_));
}

RectF Dx11RenderDevice::CurrentClip() const {
    if (!clip_stack_.empty()) {
        return clip_stack_.back();
    }
    return FullTargetRect();
}

DrawCommand Dx11RenderDevice::CurrentScissorCommand(DrawKind kind) const {
    const RectF clip = CurrentClip();
    LONG left = static_cast<LONG>(std::floor(clip.left));
    LONG top = static_cast<LONG>(std::floor(clip.top));
    LONG right = static_cast<LONG>(std::ceil(clip.right));
    LONG bottom = static_cast<LONG>(std::ceil(clip.bottom));

    const LONG target_width = static_cast<LONG>(width_);
    const LONG target_height = static_cast<LONG>(height_);
    left = std::max<LONG>(0, std::min<LONG>(left, target_width));
    top = std::max<LONG>(0, std::min<LONG>(top, target_height));
    right = std::max<LONG>(left, std::min<LONG>(right, target_width));
    bottom = std::max<LONG>(top, std::min<LONG>(bottom, target_height));

    DrawCommand command;
    command.kind = kind;
    command.scissor_left = left;
    command.scissor_top = top;
    command.scissor_right = right;
    command.scissor_bottom = bottom;
    return command;
}

void Dx11RenderDevice::FillRect(const RectF& rect, const Color& color) {
    AppendSolid(rect, 0.0f, 0.0f, color);
}

void Dx11RenderDevice::FillRoundedRect(const RectF& rect, float radius, const Color& color) {
    AppendSolid(rect, radius, 0.0f, color);
}

void Dx11RenderDevice::StrokeRect(const RectF& rect, float width, const Color& color) {
    AppendSolid(rect, 0.0f, width, color);
}

void Dx11RenderDevice::StrokeRoundedRect(const RectF& rect, float radius, float width,
                                         const Color& color) {
    AppendSolid(rect, radius, width, color);
}

void Dx11RenderDevice::AppendSolid(const RectF& rect, float radius, float border_width,
                                   const Color& color) {
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

    SolidInstance instance = {};
    instance.rect[0] = rect.left;
    instance.rect[1] = rect.top;
    instance.rect[2] = rect_width;
    instance.rect[3] = rect_height;
    instance.radius = clamped_radius;
    instance.border_width = clamped_border;
    instance.padding[0] = 0.0f;
    instance.padding[1] = 0.0f;
    instance.color[0] = color.r;
    instance.color[1] = color.g;
    instance.color[2] = color.b;
    instance.color[3] = color.a;
    instances_.push_back(instance);

    DrawCommand command = CurrentScissorCommand(DrawKind::Solid);
    command.first = instances_.size() - 1;
    command.count = 1;
    commands_.push_back(command);
}

void Dx11RenderDevice::AppendQuadVertices(float x0, float y0, float x1, float y1, float u0,
                                          float v0, float u1, float v1,
                                          const Color& color, float corner_radius) {
    // 两个三角形覆盖同一矩形，顶点顺序与三角形列表拓扑一致。
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

    for (size_t i = 0; i < 6; ++i) {
        TextureVertex vertex = {};
        vertex.position[0] = positions[i][0];
        vertex.position[1] = positions[i][1];
        vertex.uv[0] = texture_coords[i][0];
        vertex.uv[1] = texture_coords[i][1];
        vertex.color[0] = color.r;
        vertex.color[1] = color.g;
        vertex.color[2] = color.b;
        vertex.color[3] = color.a;
        vertex.shape[0] = center_x;
        vertex.shape[1] = center_y;
        vertex.shape[2] = half_width;
        vertex.shape[3] = half_height;
        vertex.radius = clamped_radius;
        vertices_.push_back(vertex);
    }
}

void Dx11RenderDevice::AppendTextureQuad(const RectF& rect, float u0, float v0, float u1, float v1,
                                         const Color& color, DrawKind kind, ImageHandle image,
                                         float corner_radius) {
    if (rect.IsEmpty() || !color.Visible()) {
        return;
    }
    const size_t first_vertex = vertices_.size();
    AppendQuadVertices(rect.left, rect.top, rect.right, rect.bottom, u0, v0, u1, v1, color,
                       corner_radius);

    DrawCommand command = CurrentScissorCommand(kind);
    command.first = first_vertex;
    command.count = vertices_.size() - first_vertex;
    command.image = image;
    commands_.push_back(command);
}

void Dx11RenderDevice::UpdateFontAtlas(const uint8_t* coverage, int width, int height) {
    if (device_ == nullptr) {
        return;
    }

    // 传入空数据或无效尺寸时释放图集，后续文字绘制会被跳过。
    if (coverage == nullptr || width <= 0 || height <= 0) {
        if (context_ != nullptr) {
            ID3D11ShaderResourceView* null_views[1] = {nullptr};
            context_->PSSetShaderResources(0, 1, null_views);
        }
        atlas_view_.Reset();
        atlas_texture_.Reset();
        atlas_width_ = 0;
        atlas_height_ = 0;
        return;
    }

    // 尺寸未变化时直接更新纹理内容，避免重建 SRV。
    if (atlas_texture_ != nullptr && atlas_width_ == width && atlas_height_ == height) {
        if (context_ != nullptr) {
            ID3D11ShaderResourceView* null_views[1] = {nullptr};
            context_->PSSetShaderResources(0, 1, null_views);
            context_->UpdateSubresource(atlas_texture_.Get(), 0, nullptr, coverage,
                                        static_cast<UINT>(width), 0);
        }
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = coverage;
    initial.SysMemPitch = static_cast<UINT>(width);
    initial.SysMemSlicePitch = 0;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, &initial, texture.GetAddressOf()))) {
        return;
    }
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr,
                                                 view.GetAddressOf()))) {
        return;
    }

    if (context_ != nullptr) {
        ID3D11ShaderResourceView* null_views[1] = {nullptr};
        context_->PSSetShaderResources(0, 1, null_views);
    }
    atlas_texture_ = std::move(texture);
    atlas_view_ = std::move(view);
    atlas_width_ = width;
    atlas_height_ = height;
}

void Dx11RenderDevice::DrawGlyphs(const GlyphQuad* quads, size_t count) {
    if (quads == nullptr || count == 0 || atlas_view_ == nullptr) {
        return;
    }

    const size_t first_vertex = vertices_.size();
    vertices_.reserve(vertices_.size() + count * 6);
    for (size_t i = 0; i < count; ++i) {
        const GlyphQuad& glyph = quads[i];
        if (glyph.x1 <= glyph.x0 || glyph.y1 <= glyph.y0 || !glyph.color.Visible()) {
            continue;
        }
        AppendQuadVertices(glyph.x0, glyph.y0, glyph.x1, glyph.y1, glyph.u0, glyph.v0,
                           glyph.u1, glyph.v1, glyph.color, 0.0f);
    }

    const size_t appended = vertices_.size() - first_vertex;
    if (appended == 0) {
        return;
    }

    DrawCommand command = CurrentScissorCommand(DrawKind::Glyph);
    command.first = first_vertex;
    command.count = appended;
    commands_.push_back(command);
}

ImageHandle Dx11RenderDevice::CreateImage(const ImageData& image) {
    if (device_ == nullptr || !image.Valid()) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(image.width);
    desc.Height = static_cast<UINT>(image.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = image.pixels.data();
    initial.SysMemPitch = static_cast<UINT>(image.width) * 4u;
    initial.SysMemSlicePitch = 0;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, &initial, texture.GetAddressOf()))) {
        return 0;
    }
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr,
                                                 view.GetAddressOf()))) {
        return 0;
    }

    const ImageHandle handle = next_image_handle_;
    ++next_image_handle_;
    TextureRecord record;
    record.texture = std::move(texture);
    record.view = std::move(view);
    images_.emplace(handle, std::move(record));
    return handle;
}

void Dx11RenderDevice::UpdateImage(ImageHandle image, const ImageData& image_data) {
    if (device_ == nullptr || image == 0 || !image_data.Valid()) {
        return;
    }
    const auto found = images_.find(image);
    if (found == images_.end()) {
        return;
    }

    const UINT width = static_cast<UINT>(image_data.width);
    const UINT height = static_cast<UINT>(image_data.height);
    D3D11_TEXTURE2D_DESC existing = {};
    found->second.texture->GetDesc(&existing);
    if (existing.Width != width || existing.Height != height) {
        // 尺寸变化时重建纹理与视图，句柄保持不变。
        D3D11_TEXTURE2D_DESC desc = existing;
        desc.Width = width;
        desc.Height = height;

        D3D11_SUBRESOURCE_DATA initial = {};
        initial.pSysMem = image_data.pixels.data();
        initial.SysMemPitch = width * 4u;
        initial.SysMemSlicePitch = 0;

        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device_->CreateTexture2D(&desc, &initial, texture.GetAddressOf()))) {
            return;
        }
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr,
                                                     view.GetAddressOf()))) {
            return;
        }
        found->second.texture = std::move(texture);
        found->second.view = std::move(view);
        return;
    }

    // 同尺寸时直接覆盖纹理内容，避免重建资源。
    context_->UpdateSubresource(found->second.texture.Get(), 0, nullptr,
                                image_data.pixels.data(), width * 4u, 0);
}

void Dx11RenderDevice::DrawImage(ImageHandle image, const RectF& rect, float opacity,
                                 float corner_radius) {
    DrawImageSubRect(image, rect, RectF::FromLTRB(0.0f, 0.0f, 1.0f, 1.0f), opacity,
                     corner_radius);
}

void Dx11RenderDevice::DrawImageSubRect(ImageHandle image, const RectF& rect, const RectF& uv,
                                        float opacity, float corner_radius) {
    if (image == 0 || !(opacity > 0.0f)) {
        return;
    }
    if (images_.find(image) == images_.end()) {
        return;
    }
    const float clamped_opacity = std::min(opacity, 1.0f);
    const Color modulation = Rgba(1.0f, 1.0f, 1.0f, clamped_opacity);
    AppendTextureQuad(rect, uv.left, uv.top, uv.right, uv.bottom, modulation, DrawKind::Image,
                      image, corner_radius);
}

void Dx11RenderDevice::PushClip(const RectF& rect) {
    if (clip_stack_.empty()) {
        clip_stack_.push_back(FullTargetRect());
    }
    clip_stack_.push_back(clip_stack_.back().Intersect(rect));
}

void Dx11RenderDevice::PopClip() {
    if (clip_stack_.size() > 1) {
        clip_stack_.pop_back();
    }
}

bool Dx11RenderDevice::EnsureVertexBuffer(ComPtr<ID3D11Buffer>& buffer, size_t& capacity,
                                          size_t required, size_t stride) {
    if (buffer != nullptr && capacity >= required) {
        return true;
    }
    if (device_ == nullptr || stride == 0) {
        return false;
    }

    size_t next_capacity = required;
    if (capacity > 0) {
        next_capacity = std::max(next_capacity, capacity * 2);
    }
    next_capacity = std::max<size_t>(next_capacity, 256);

    // 单帧缓冲上限 64 MB，避免超出 D3D11 单资源尺寸限制。
    const size_t max_bytes = 64u * 1024u * 1024u;
    size_t byte_width = next_capacity * stride;
    if (byte_width > max_bytes) {
        next_capacity = max_bytes / stride;
        byte_width = next_capacity * stride;
    }
    if (next_capacity < required) {
        return false;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(byte_width);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    ComPtr<ID3D11Buffer> created;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, created.GetAddressOf()))) {
        return false;
    }
    buffer = std::move(created);
    capacity = next_capacity;
    return true;
}

bool Dx11RenderDevice::UploadGeometry() {
    if (context_ == nullptr) {
        return false;
    }

    if (!instances_.empty()) {
        if (!EnsureVertexBuffer(instance_buffer_, instance_capacity_, instances_.size(),
                                sizeof(SolidInstance))) {
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(instance_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, instances_.data(),
                    instances_.size() * sizeof(SolidInstance));
        context_->Unmap(instance_buffer_.Get(), 0);
    }

    if (!vertices_.empty()) {
        if (!EnsureVertexBuffer(vertex_buffer_, vertex_capacity_, vertices_.size(),
                                sizeof(TextureVertex))) {
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, vertices_.data(), vertices_.size() * sizeof(TextureVertex));
        context_->Unmap(vertex_buffer_.Get(), 0);
    }
    return true;
}

void Dx11RenderDevice::BindSolidPipeline() {
    if (context_ == nullptr) {
        return;
    }
    UINT stride = static_cast<UINT>(sizeof(SolidInstance));
    UINT offset = 0;
    ID3D11Buffer* buffer = instance_buffer_.Get();
    context_->IASetVertexBuffers(1, 1, &buffer, &stride, &offset);
    context_->IASetInputLayout(solid_input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(solid_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(solid_pixel_shader_.Get(), nullptr, 0);
}

void Dx11RenderDevice::BindTexturePipeline(ID3D11ShaderResourceView* view, bool glyph) {
    if (context_ == nullptr) {
        return;
    }
    UINT stride = static_cast<UINT>(sizeof(TextureVertex));
    UINT offset = 0;
    ID3D11Buffer* buffer = vertex_buffer_.Get();
    context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    context_->IASetInputLayout(texture_input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(texture_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(glyph ? glyph_pixel_shader_.Get() : image_pixel_shader_.Get(), nullptr,
                          0);
    context_->PSSetShaderResources(0, 1, &view);
}

bool Dx11RenderDevice::EndFrame() {
    if (context_ == nullptr || target_view_ == nullptr) {
        return false;
    }
    if (commands_.empty()) {
        return true;
    }
    if (!UploadGeometry()) {
        return false;
    }

    bool has_scissor = false;
    LONG scissor_left = 0;
    LONG scissor_top = 0;
    LONG scissor_right = 0;
    LONG scissor_bottom = 0;

    for (const DrawCommand& command : commands_) {
        // 仅在裁剪矩形变化时更新光栅化状态。
        if (!has_scissor || command.scissor_left != scissor_left ||
            command.scissor_top != scissor_top || command.scissor_right != scissor_right ||
            command.scissor_bottom != scissor_bottom) {
            const D3D11_RECT scissor = {command.scissor_left, command.scissor_top,
                                        command.scissor_right, command.scissor_bottom};
            context_->RSSetScissorRects(1, &scissor);
            scissor_left = command.scissor_left;
            scissor_top = command.scissor_top;
            scissor_right = command.scissor_right;
            scissor_bottom = command.scissor_bottom;
            has_scissor = true;
        }

        if (command.count == 0) {
            continue;
        }
        if (command.kind == DrawKind::Solid) {
            BindSolidPipeline();
            context_->DrawInstanced(4, static_cast<UINT>(command.count), 0,
                                    static_cast<UINT>(command.first));
            continue;
        }

        ID3D11ShaderResourceView* view = nullptr;
        if (command.kind == DrawKind::Glyph) {
            view = atlas_view_.Get();
        } else {
            const auto found = images_.find(command.image);
            if (found != images_.end()) {
                view = found->second.view.Get();
            }
        }
        if (view == nullptr) {
            continue;
        }
        BindTexturePipeline(view, command.kind == DrawKind::Glyph);
        context_->Draw(static_cast<UINT>(command.count), static_cast<UINT>(command.first));
    }

    // 解绑纹理，避免下一次图集更新或资源释放时与管线读取产生冲突。
    ID3D11ShaderResourceView* null_views[1] = {nullptr};
    context_->PSSetShaderResources(0, 1, null_views);

    instances_.clear();
    vertices_.clear();
    commands_.clear();
    return true;
}

bool Dx11RenderDevice::ReadPixels(std::vector<uint8_t>* rgba_pixels) {
    if (rgba_pixels == nullptr) {
        return false;
    }
    if (context_ == nullptr || target_texture_ == nullptr || staging_texture_ == nullptr ||
        width_ <= 0 || height_ <= 0) {
        return false;
    }

    // 先解绑渲染目标视图，避免同一资源同时处于渲染与复制两种用途。
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->CopyResource(staging_texture_.Get(), target_texture_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(width_) * 4u;
    rgba_pixels->resize(row_bytes * static_cast<size_t>(height_));
    const uint8_t* source = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < height_; ++y) {
        uint8_t* destination = rgba_pixels->data() + static_cast<size_t>(y) * row_bytes;
        std::memcpy(destination, source + static_cast<size_t>(y) * mapped.RowPitch, row_bytes);
        // 目标纹理按不透明画布处理，回读结果 alpha 固定为 255（非预乘）。
        for (int x = 0; x < width_; ++x) {
            destination[static_cast<size_t>(x) * 4u + 3u] = 255;
        }
    }

    context_->Unmap(staging_texture_.Get(), 0);
    return true;
}

}  // namespace

std::unique_ptr<RenderDevice> CreateDirect3D11RenderDevice() {
    std::unique_ptr<Dx11RenderDevice> device = std::make_unique<Dx11RenderDevice>();
    if (!device->InitializeDevice()) {
        return nullptr;
    }
    return device;
}

}  // namespace web
