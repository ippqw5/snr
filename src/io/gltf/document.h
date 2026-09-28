#pragma once

#include "./common.h"

#include <filesystem>

namespace snr::gltf
{
tinygltf::Model readDocument(const std::filesystem::path& path, std::vector<std::string>& warnings);
}