#pragma once

#include "./materials.h"

namespace snr::gltf
{
void importGeomtry(const tinygltf::Model& mdel, const ImportedMaterials& materials, Scene& scene);
}