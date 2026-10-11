#pragma once

#include "../scene/scene.h"
#include "./settings.h"

namespace snr
{
// One-shot rendering uses the same settings as a reusable session.
struct RenderOptions
{
    ImageSize             image;
    IntegratorSettings    integrator;
    DisplaySettings       display;
    OutputSettings        output;
    RenderingMode         mode = RenderingMode::PathTracing;
    // Raster preview only: draw a debug view of one channel instead of the shaded image. A view other
    // than rasterDebugShaded implies Raster, since the path tracer has none of these channels.
    uint32_t              debugView = rasterDebugShaded;
    // Opt into the interactive swapchain window instead of a headless one-shot render.
    bool                  windowed = false;
    uint32_t              spp = 256, batchSize = 16;
    bool                  validation = false;
    std::filesystem::path shaderDir;
};

CameraSettings cameraFromScene(const Scene& scene);
void           render(const Scene& scene, const RenderOptions& options);
} // namespace snr
