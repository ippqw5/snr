#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>

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
};

struct DisplaySettings
{
    float exposure = 0;
};

struct OutputSettings
{
    std::filesystem::path path;
    bool                  writePfm = false;
};

// void validateImageSize(const ImageSize& image);
// void validateCamera(const CameraSettings& camera);
// void validateIntegrator(const IntegratorSettings& integrator);
// void validateIntegrator(glm::vec3 environment);
// void validateSampling(uint32_t targetSamples, uint32_t batchSize);
// void validateDisplay(const DisplaySettings& display);