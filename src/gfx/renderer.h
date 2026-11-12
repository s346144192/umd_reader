#pragma once

#include <windows.h>

#include <memory>

#include "gfx/canvas.h"
#include "gfx/text_engine.h"
#include "gfx/types.h"

namespace gfx {

class D2DInterop;

// 渲染后端入口：持有绘制画布与排版引擎，并负责设备资源的创建、重建与释放。
class Renderer {
public:
    virtual ~Renderer() = default;

    virtual bool Initialize(HWND hwnd) = 0;
    virtual void Shutdown() = 0;
    // 返回 true 表示渲染目标被重建，调用方需要清空依赖设备的资源。
    virtual bool Resize(unsigned widthPixels, unsigned heightPixels, unsigned dpi) = 0;
    virtual float DpiScale() const = 0;
    virtual unsigned Dpi() const = 0;

    virtual Canvas& Graphics() = 0;
    virtual TextEngine& Text() = 0;
    virtual const RendererCapabilities& Capabilities() const = 0;
    // 仅 Direct2D 后端提供，其他后端返回 nullptr。
    virtual D2DInterop* Direct2DInterop() { return nullptr; }
};

}  // namespace gfx
