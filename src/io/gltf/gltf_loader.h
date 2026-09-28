#pragma once

#include "../../scene/scene.h"

#include <filesystem>

namespace snr
{
// Load the supported static glTF subset into renderer-owned, world-space data
Scene loadGltf(const std::filesystem::path& path);
} // namespace snr