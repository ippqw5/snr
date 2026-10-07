#pragma once

#include "./common.h"

#include <array>

namespace snr
{

struct TextureCoordinates
{
    int       set = -1;
    glm::mat3 transform = glm::mat3(1);

    vec2 apply(vec2 uv) const
    {
        return vec2(transform * vec3(uv, 1));
    }
};

// Base-color and metallic/roughness textures each select their own UV set and transform.
using MaterialBindings = std::array<TextureCoordinates, 2>;

struct ImportedMaterials
{
    std::vector<MaterialBindings> bindings;
    uint32_t                      defaultMaterial;
};

ImportedMaterials importMaterials(const tinygltf::Model& model, Scene& scene);
} // namespace snr