#pragma once

#include "./settings.h"

#include <vector>

namespace snr
{
// CPU-owned snaphost; independent of the device and any subsequent rendering.
struct RenderResult
{
    ImageSize              image;
    uint32_t               samples = 0;
    std::vector<glm::vec4> sums;
    std::vector<glm::vec4> display;
};
} // namespace snr
