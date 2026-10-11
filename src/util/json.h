#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace snr
{
// Minimal JSON writers. They return fragments so callers can assemble a document without a
// dependency on a JSON library.
std::string jsonNumber(float value);
std::string jsonString(const std::string& value);
std::string jsonStrings(const std::vector<std::string>& values);
std::string jsonVector(glm::vec3 value);
std::string jsonVector(glm::vec4 value);
} // namespace snr
