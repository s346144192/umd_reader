#include "gfx/renderer_factory.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "gfx/d2d_renderer.h"

namespace gfx {
namespace {

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return std::wstring();
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written >= length) {
        return std::wstring();
    }
    value.resize(written);
    return value;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    return value;
}

// 解析后端名称；无法识别时返回 false，由调用方回退默认后端。
bool ParseBackendName(const std::wstring& name, BackendKind& kind) {
    const std::wstring normalized = ToLower(name);
    if (normalized.empty() || normalized == L"auto" || normalized == L"default" ||
        normalized == L"d2d" || normalized == L"direct2d" || normalized == L"direct-2d") {
        kind = BackendKind::Direct2D;
        return true;
    }
    return false;
}

}  // namespace

std::unique_ptr<Renderer> CreateRenderer(BackendKind kind) {
    switch (kind) {
    case BackendKind::Direct2D:
        return detail::CreateDirect2DRenderer();
    }
    return nullptr;
}

std::unique_ptr<Renderer> CreateRendererFromEnvironment() {
    const std::wstring requested = EnvironmentValue(L"UMD_READER_RENDERER");
    BackendKind kind = BackendKind::Direct2D;
    if (ParseBackendName(requested, kind)) {
        std::unique_ptr<Renderer> renderer = CreateRenderer(kind);
        if (renderer) {
            return renderer;
        }
    }
    // 未设置、名称无法识别或创建失败时统一回退到 Direct2D，保证阅读器可用。
    return CreateRenderer(BackendKind::Direct2D);
}

std::wstring BackendDisplayName(BackendKind kind) {
    switch (kind) {
    case BackendKind::Direct2D:
        return L"Direct2D";
    }
    return L"未知";
}

}  // namespace gfx
