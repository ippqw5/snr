#pragma once

#include "./settings.h"

#include <vector>

struct RenderResult
{
    ImageSize              imageSize;
    uint32_t               samples = 0;
    std::vector<glm::vec4> sums;
    std::vector<glm::vec4> display;
};