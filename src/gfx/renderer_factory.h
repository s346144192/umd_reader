#pragma once

#include <memory>
#include <string>

#include "gfx/renderer.h"
#include "gfx/types.h"

namespace gfx {

// 按后端类型创建渲染器；不支持的类型返回空指针。
std::unique_ptr<Renderer> CreateRenderer(BackendKind kind);

// 读取环境变量 UMD_READER_RENDERER 选择后端；未设置、auto 或无法识别时回退 Direct2D。
std::unique_ptr<Renderer> CreateRendererFromEnvironment();

// 后端显示名，用于启动日志与状态提示。
std::wstring BackendDisplayName(BackendKind kind);

}  // namespace gfx
