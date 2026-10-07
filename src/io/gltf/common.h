#pragma once

#include "../../scene/scene.h"

#include <cmath>
#include <stdexcept>
#include <tiny_gltf.h>

namespace snr
{

constexpr size_t maxTexturePixel = 16 * 1024 * 1024;

inline void require(bool condition, const std::string& msg)
{
    if (!condition)
        throw std::runtime_error(msg);
}

template <typename T>
const T& at(const std::vector<T>& values, int index, const char* description)
{
    require(index >= 0 && size_t(index) < values.size(), std::string("Invalid glTF ") + description + " index: " + std::to_string(index));
    return values[size_t(index)];
}

inline const tinygltf::Value* extension(const tinygltf::ExtensionMap& extensions, const char* name)
{
    auto found = extensions.find(name);
    return found == extensions.end() ? nullptr : &found->second;
}

inline float number(const tinygltf::Value& value, const char* description)
{
    require(value.IsNumber(), std::string("Expected a numeric ") + description);
    float result = float(value.GetNumberAsDouble());
    require(std::isfinite(result), std::string("Non-finite ") + description);
    return result;
}

} // namespace snr