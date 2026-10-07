#include "./materials.h"
#include "io/gltf/common.h"

namespace snr
{

static float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

static glm::vec2 vector2(const tinygltf::Value& value, const char* description)
{
    require(value.IsArray() && value.ArrayLen() == 2, std::string("Invalid ") + description);
    return {number(value.Get(size_t(0)), description), number(value.Get(size_t(1)), description)};
}

static TextureCoordinates TextureCoordinates(const tinygltf::TextureInfo& info)
{
    struct TextureCoordinates result{};
    result.set = info.texCoord;
    glm::vec2 offset(0), scale(1);
    float     rotaion = 0;

    if (const auto* transform = extension(info.extensions, "KHR_texture_transform"))
    {
        require(transform->IsObject(), "Invalid KHR_texture_transform");
    }
    return result;
}

ImportedMaterials importMaterials(const tinygltf::Model& model, Scene& scene)
{
    struct ImportedMaterials result;
    return result;
}

} // namespace snr