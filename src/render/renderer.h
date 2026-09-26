#pragma once

#include "../scene/scene.h"
#include "./settings.h"

struct RenderOptions
{
    uint32_t              width = 800, height = 800, spp = 256, maxBounces = 12;
    uint32_t              seed = 1, batchSize = 16;
    float                 exposure = 0;
    bool                  nee = true, validation = false, writePfm = false;
    std::filesystem::path output;
    std::filesystem::path shaderDir;
};

CameraSettings cameraFromScene(const Scene& scene);
void           render(const Scene& scene, const RenderOptions& options);