#pragma once

#include "./common.h"

namespace snr
{
std::vector<float>    readAttribute(const tinygltf::Model& model, int index, int type);
std::vector<uint32_t> readIndices(const tinygltf::Model& model, int accessor, size_t vertexCount);
} // namespace snr
