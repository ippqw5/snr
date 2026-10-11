#include "scene/scene_json.h"

#include "scene/scene.h"
#include "util/json.h"

#include <string>

namespace snr
{
static std::string jsonTexture(int texture)
{
    return texture < 0 ? "null" : std::to_string(texture);
}

static std::string jsonWrap(TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return "\"clampToEdge\"";
    case TextureWrap::MirrorRepeat:
        return "\"mirrorRepeat\"";
    default:
        return "\"repeat\"";
    }
}

static std::string jsonAlphaMode(uint mode)
{
    switch (mode)
    {
    case alphaModeMask:
        return "\"MASK\"";
    case alphaModeBlend:
        return "\"BLEND\"";
    default:
        return "\"OPAQUE\"";
    }
}

std::string sceneJson(const Scene& scene)
{
    std::string json = "{\n";

    json += "  \"camera\": {\"eye\": " + jsonVector(scene.eye) + ", \"target\": " + jsonVector(scene.target)
          + ", \"up\": " + jsonVector(scene.up) + ", \"fov\": " + jsonNumber(scene.fov) + "},\n";
    json += "  \"environment\": " + jsonVector(scene.environment) + ",\n";
    json += "  \"bounds\": {\"min\": " + jsonVector(scene.lower) + ", \"max\": " + jsonVector(scene.upper) + "},\n";
    json += "  \"counts\": {\"triangles\": " + std::to_string(scene.indices.size())
          + ", \"vertices\": " + std::to_string(scene.positions.size())
          + ", \"primitives\": " + std::to_string(scene.primitives.size())
          + ", \"instances\": " + std::to_string(scene.instances.size())
          + ", \"materials\": " + std::to_string(scene.materials.size())
          + ", \"lights\": " + std::to_string(scene.lights.size())
          + ", \"textures\": " + std::to_string(scene.textures.size())
          + ", \"samplers\": " + std::to_string(scene.samplers.size()) + "},\n";

    // The bindless heaps. Entry i is exactly slot i of the shader's images[] and samplers[] arrays,
    // because the host fills those arrays in registration order, so the material indices below can
    // be read directly against this layout.
    json += "  \"bindless\": {\"maxTextures\": " + std::to_string(SNR_MAX_TEXTURES)
          + ", \"maxSamplers\": " + std::to_string(SNR_MAX_SAMPLERS) + "},\n";

    json += "  \"textures\": [";
    for (size_t index = 0; index < scene.textures.size(); ++index)
    {
        const TextureData& texture = scene.textures[index];
        json += (index == 0 ? "\n" : ",\n");
        json += "    {\"width\": " + std::to_string(texture.width) + ", \"height\": " + std::to_string(texture.height)
              + ", \"srgb\": " + (texture.srgb ? "true" : "false")
              + ", \"levels\": " + std::to_string(texture.levels.size()) + "}";
    }
    json += scene.textures.empty() ? "],\n" : "\n  ],\n";

    json += "  \"samplers\": [";
    for (size_t index = 0; index < scene.samplers.size(); ++index)
    {
        const TextureSampler& sampler = scene.samplers[index];
        json += (index == 0 ? "\n" : ",\n");
        json += "    {\"wrapU\": " + jsonWrap(sampler.wrapU) + ", \"wrapV\": " + jsonWrap(sampler.wrapV)
              + ", \"nearest\": " + (sampler.nearest ? "true" : "false") + "}";
    }
    json += scene.samplers.empty() ? "],\n" : "\n  ],\n";

    json += "  \"materials\": [";
    for (size_t index = 0; index < scene.materials.size(); ++index)
    {
        const GltfMaterial& material = scene.materials[index];
        json += (index == 0 ? "\n" : ",\n");
        json += "    {\"baseColor\": " + jsonVector(material.pbrBaseColorFactor)
              + ", \"emissive\": " + jsonVector(vec3(material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z))
              + ", \"metallic\": " + jsonNumber(material.pbrMetallicRoughnessFactor.x)
              + ", \"roughness\": " + jsonNumber(material.pbrMetallicRoughnessFactor.y)
              + ", \"doubleSided\": " + (material.flags.x ? "true" : "false")
              + ", \"alphaMode\": " + jsonAlphaMode(material.flags.z)
              + ", \"alphaCutoff\": " + jsonNumber(material.pbrMetallicRoughnessFactor.z)
              + ", \"baseColorTexture\": " + jsonTexture(material.pbrBaseColorTexture)
              + ", \"baseColorSampler\": " + jsonTexture(material.pbrBaseColorSampler)
              + ", \"metallicRoughnessTexture\": " + jsonTexture(material.pbrMetallicRoughnessTexture)
              + ", \"metallicRoughnessSampler\": " + jsonTexture(material.pbrMetallicRoughnessSampler)
              + ", \"normalTexture\": " + jsonTexture(material.normalTexture)
              + ", \"normalSampler\": " + jsonTexture(material.normalSampler)
              + ", \"normalScale\": " + jsonNumber(material.normalScale)
              + ", \"occlusionTexture\": " + jsonTexture(material.occlusionTexture)
              + ", \"occlusionSampler\": " + jsonTexture(material.occlusionSampler)
              + ", \"occlusionStrength\": " + jsonNumber(material.occlusionStrength)
              + ", \"emissiveTexture\": " + jsonTexture(material.emissiveTexture)
              + ", \"emissiveSampler\": " + jsonTexture(material.emissiveSampler)
              + "}";
    }
    json += scene.materials.empty() ? "],\n" : "\n  ],\n";

    json += "  \"primitives\": [";
    for (size_t index = 0; index < scene.primitives.size(); ++index)
    {
        const Primitive& primitive = scene.primitives[index];
        json += (index == 0 ? "\n" : ",\n");
        json += "    {\"material\": " + std::to_string(primitive.material)
              + ", \"firstTriangle\": " + std::to_string(primitive.firstTriangle)
              + ", \"triangleCount\": " + std::to_string(primitive.triangleCount) + "}";
    }
    json += scene.primitives.empty() ? "],\n" : "\n  ],\n";

    json += "  \"instances\": [";
    for (size_t index = 0; index < scene.instances.size(); ++index)
    {
        const Instance& instance = scene.instances[index];
        json += (index == 0 ? "\n" : ",\n");
        json += "    {\"primitive\": " + std::to_string(instance.primitive)
              + ", \"material\": " + std::to_string(instance.material)
              + ", \"lightBase\": " + (instance.lightBase == noLight ? std::string("null") : std::to_string(instance.lightBase))
              + ", \"transform\": [" + jsonVector(instance.transformRows[0]) + ", " + jsonVector(instance.transformRows[1])
              + ", " + jsonVector(instance.transformRows[2]) + "]}";
    }
    json += scene.instances.empty() ? "],\n" : "\n  ],\n";

    json += "  \"warnings\": [" + jsonStrings(scene.warnings) + "],\n";
    json += "  \"errors\": [" + jsonStrings(scene.errors) + "]\n";
    json += "}\n";
    return json;
}
} // namespace snr
