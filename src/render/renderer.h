#pragma once

#include "../scene/scene.h"
#include "./settings.h"

namespace snr
{
// One-shot rendering uses the same settings as a reusable RenderSession.
struct RenderOptions
{
    ImageSize             image;
    IntegratorSettings    integrator;
    DisplaySettings       display;
    OutputSettings        output;
    uint32_t              spp = 256, batchSize = 16;
    bool                  validation = false;
    std::filesystem::path shaderDir;
};

CameraSettings cameraFromScene(const Scene& scene);
void           render(const Scene& scene, const RenderOptions& options);
} // namespace snr
