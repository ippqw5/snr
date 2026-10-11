#pragma once

#include <string>

namespace snr
{
struct Scene;

// Serializes the derived scene for --inspect. Geometry is summarized as counts, so this stays
// cheap on large models and never needs a GPU device.
std::string sceneJson(const Scene& scene);
} // namespace snr
