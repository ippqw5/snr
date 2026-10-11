#pragma once

#include "shaders/scene_io.h.slang"

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>

namespace snr
{

struct ImageSize
{
    uint32_t width = 800, height = 800;
    bool     operator==(const ImageSize& other) const = default;
};

struct CameraSettings
{
    glm::vec3 eye = {0, 1, 3.6f}, target = {0, 1, 0}, up = {0, 1, 0};
    float     fov = 40;
    bool      operator==(const CameraSettings& other) const = default;
};

struct IntegratorSettings
{
    uint32_t maxBounces = 12, seed = 1;
    bool     nee = true;
    bool     operator==(const IntegratorSettings& other) const = default;
};

struct RenderSettings
{
    ImageSize          imageSize;
    CameraSettings     camera;
    IntegratorSettings integrator;
    // Path tracer accumulation: the target samples per pixel and the batch a dispatch adds. These are
    // not per-draw constants, so they stay on the host side of the session. The raster preview draws
    // one frame and ignores them.
    uint32_t samplesPerPixel = 256;
    uint32_t batchSize = 16;
    // Raster preview only: which channel its fragment shader writes to the colour target instead of
    // the shaded image. One of the rasterDebug* constants; the path tracer ignores it.
    uint32_t debugView = rasterDebugShaded;
};

struct DisplaySettings
{
    float exposure = 0;
};

// Which renderer draws the frame. The two are peers over the same scene snapshot, film and image
// output, so selecting one is a runtime choice rather than a build-time one.
enum class RenderingMode
{
    PathTracing,
    Raster,
};

struct OutputSettings
{
    std::filesystem::path path;
};

} // namespace snr
