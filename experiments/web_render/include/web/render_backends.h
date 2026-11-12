#pragma once

#include <memory>

#include "web/render.h"

namespace web {

// 后端工厂实现，由具体后端源文件提供；失败返回 nullptr。
std::unique_ptr<RenderDevice> CreateDirect3D11RenderDevice();
std::unique_ptr<RenderDevice> CreateOpenGL33RenderDevice();

}  // namespace web
