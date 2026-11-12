#pragma once

#include <memory>

#include "gfx/renderer.h"

namespace gfx {
namespace detail {

std::unique_ptr<Renderer> CreateDirect2DRenderer();

}  // namespace detail
}  // namespace gfx
