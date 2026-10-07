#pragma once

#include "./materials.h"

namespace snr
{
void importGeomtry(const tinygltf::Model& model, const ImportedMaterials& materials, Scene& scene);
}