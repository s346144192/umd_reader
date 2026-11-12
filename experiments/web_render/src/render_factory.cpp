#include "web/render.h"

#include "web/render_backends.h"

namespace web {

std::unique_ptr<RenderDevice> CreateRenderDevice(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::Direct3D11:
        return CreateDirect3D11RenderDevice();
    case RenderBackend::OpenGL33:
        return CreateOpenGL33RenderDevice();
    }
    return nullptr;
}

}  // namespace web
