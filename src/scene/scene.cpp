#include "./scene.h"

#define STB_IMAGE_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace tg = tinygltf;

static void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

uint32_t Scene::addMaterial(glm::vec3 color, glm::vec3 emission)
{
    Material material;
    material.baseColor = glm::vec4(color, 1);
    material.emission = glm::vec4(emission, 0);
    materials.push_back(material);
    return uint32_t(materials.size() - 1);
}

void Scene::addTriangle(glm::vec3 a, glm::vec3 b, glm::vec3 c, uint32_t material)
{
    glm::vec3 normal = glm::cross(b - a, c - a);
    require(glm::length(normal) > 1e-12f, "Degenerate triangle");
    normal = glm::normalize(normal);
    triangles.push_back({
        glm::vec4(a, 0),
        glm::vec4(b, 0),
        glm::vec4(c, 0),
        glm::vec4(normal, 0),
        glm::vec4(normal, 0),
        glm::vec4(normal, 0),
        glm::vec4(0),
        glm::vec4(0),
        glm::uvec4(material, UINT32_MAX, 0, 0),
    });
}

void Scene::addQuad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material)
{
    addTriangle(a, b, c, material);
    addTriangle(a, c, d, material);
}

void Scene::finalize()
{
    require(!triangles.empty(), "Scene has no non-degenerate triangles");
    require(triangles.size() < UINT32_MAX / 3,
            "Scene exceeds 32-bit geometry limits");
    lower = glm::vec3(std::numeric_limits<float>::max());
    upper = -lower;
    lights.clear();
    for (size_t i = 0; i < triangles.size(); ++i)
    {
        auto& t = triangles[i];
        for (glm::vec3 p : {glm::vec3(t.p0), glm::vec3(t.p1), glm::vec3(t.p2)})
        {
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                    "Non-finite vertex position");
            lower = glm::min(lower, p);
            upper = glm::max(upper, p);
        }
        t.info.y = UINT32_MAX;
        const auto emission = glm::vec3(materials.at(t.info.x).emission);
        if (glm::dot(emission, emission) > 0)
        {
            t.info.y = uint32_t(lights.size());
            lights.push_back(uint32_t(i));
        }
    }
}

static void addBox(Scene& scene, glm::vec3(center), glm::vec3 size, float angle, uint32_t material)
{
    glm::vec3 p[8];
    float     c = std::cos(angle), s = std::sin(angle);
    for (int i = 0; i < 8; i++)
    {
        glm::vec3 q = (glm::vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1) - 0.5f) * size;
        p[i] = center + glm::vec3(c * q.x + s * q.z, q.y, -s * q.x + c * q.z);
    }
    scene.addQuad(p[0], p[4], p[6], p[2], material);
    scene.addQuad(p[1], p[3], p[7], p[5], material);
    scene.addQuad(p[0], p[1], p[5], p[4], material);
    scene.addQuad(p[2], p[6], p[7], p[3], material);
    scene.addQuad(p[0], p[2], p[3], p[1], material);
    scene.addQuad(p[4], p[5], p[7], p[6], material);
}

Scene makeCornellBox()
{
    Scene s;
    auto  white = s.addMaterial(glm::vec3(0.73f));
    auto  red = s.addMaterial(glm::vec3(0.63f, 0.065f, 0.05f));
    auto  green = s.addMaterial(glm::vec3(0.14f, 0.45f, 0.091f));
    auto  light = s.addMaterial(glm::vec3(0), glm::vec3(14.0f, 12.8f, 10.5f));
    s.addQuad({-1, 0, 1}, {1, 0, 1}, {1, 0, -1}, {-1, 0, -1}, white);
    s.addQuad({-1, 2, -1}, {1, 2, -1}, {1, 2, 1}, {-1, 2, 1}, white);
    s.addQuad({-1, 0, -1}, {1, 0, -1}, {1, 2, -1}, {-1, 2, -1}, white);
    s.addQuad({-1, 0, 1}, {-1, 0, -1}, {-1, 2, -1}, {-1, 2, 1}, red);

    s.addQuad({1, 0, -1}, {1, 0, 1}, {1, 2, 1}, {1, 2, -1}, green);
    s.addQuad({-0.33f, 1.985f, -0.3f}, {0.33f, 1.985f, -0.3f},
              {0.33f, 1.985f, 0.25f}, {-0.33f, 1.985f, 0.25f}, light);

    addBox(s, {-0.4f, 0.32f, 0.35f}, {0.62f, 0.64f, 0.62f}, -0.28f, white);
    addBox(s, {0.36f, 0.63f, -0.3f}, {0.62f, 1.26f, 0.62f}, 0.3f, white);
    s.finalize();
    return s;
}

Scene makeFurnaceScene()
{
    Scene s;
    auto  matte = s.addMaterial(glm::vec3(0.6f, 0.4f, 0.2f));
    s.addQuad({-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {-100, 100, 0},
              matte);
    s.eye = {0, 0, 2};
    s.target = {0, 0, 0};
    s.environment = glm::vec3(1);
    s.finalize();
    return s;
}

static fs::path localUri(const fs::path& parent, const char* uri)
{
    require(uri && !std::strchr(uri, ':') && !std::strchr(uri, '\\'),
            "Only relative local assets URIs are supported");

    std::string decoded;
    decoded.reserve(std::strlen(uri));
    for (size_t i = 0; uri[i] != '\0'; ++i)
    {
        if (uri[i] == '%' && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2])))
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return 10 + c - 'a';
                if (c >= 'A' && c <= 'F')
                    return 10 + c - 'A';
                return 0;
            };
            decoded.push_back(char((hex(uri[i + 1]) << 4) | hex(uri[i + 2])));
            i += 2;
        }
        else
        {
            decoded.push_back(uri[i]);
        }
    }

    fs::path relative(decoded);
    require(!relative.is_absolute(), "Absolute asset URIs are not supported");

    for (const auto& part : relative)
    {
        require(part != "..", "Asset URIs must stay inside the model directory");
    }

    const auto root = fs::weakly_canonical(parent);
    const auto result = fs::weakly_canonical(parent / relative);

    auto mismatch =
        std::mismatch(root.begin(), root.end(), result.begin(), result.end());
    require(mismatch.first == root.end(),
            "Asset symlink escapes the model directory");
    return result;
}

static float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

static std::string decodeUri(const std::string& uri)
{
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2])))
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return 10 + c - 'a';
                if (c >= 'A' && c <= 'F')
                    return 10 + c - 'A';
                return 0;
            };
            decoded.push_back(char((hex(uri[i + 1]) << 4) | hex(uri[i + 2])));
            i += 2;
        }
        else
        {
            decoded.push_back(uri[i]);
        }
    }
    return decoded;
}

static const tg::Node* findNodeByPointer(const tg::Model& model, const tg::Node* node)
{
    if (!node)
        return nullptr;

    for (const auto& candidate : model.nodes)
    {
        if (&candidate == node)
            return &candidate;
    }
    return nullptr;
}

static glm::ivec4 loadTexture(Scene& scene, const tg::Model& model, const tg::Texture& texture, const fs::path& parent)
{
    require(texture.source >= 0 && texture.source < int(model.images.size()),
            "Missing texture image (compressed textures unsupported)");

    const auto& image = model.images[texture.source];
    int         width = 0, height = 0, channels = 0;
    stbi_uc*    pixels = nullptr;
    if (image.bufferView >= 0)
    {
        require(image.bufferView < int(model.bufferViews.size()), "Texture buffer view out of range");
        const auto& view = model.bufferViews[image.bufferView];
        require(view.byteOffset + view.byteLength <= model.buffers.at(view.buffer).data.size(),
                "Texture exceeds decoder limits");
        const auto& buffer = model.buffers.at(view.buffer);
        const auto* bytes = buffer.data.data() + view.byteOffset;
        pixels = stbi_load_from_memory(bytes, int(view.byteLength), &width, &height,
                                       &channels, 4);
    }
    else
    {
        require(!image.uri.empty(), "Missing texture URI");
        auto path = localUri(parent, decodeUri(image.uri).c_str());
        pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    }

    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> owner(pixels,
                                                               stbi_image_free);
    require(pixels != nullptr, "Could not decode PNG/JPEG texture");
    require(width > 0 && height > 0 && size_t(width) * size_t(height) <= 16777216,
            "Texture exceeds 16 million pixel limits");
    require(scene.texels.size() + size_t(width) * height <= INT32_MAX,
            "Texture atlas too large");

    int offset = int(scene.texels.size());
    for (size_t i = 0; i < size_t(width) * height; ++i)
    {
        scene.texels.emplace_back(srgbToLinear(pixels[i * 4] / 255.f),
                                  srgbToLinear(pixels[i * 4 + 1] / 255.f),
                                  srgbToLinear(pixels[i * 4 + 2] / 255.f),
                                  pixels[i * 4 + 3] / 255.f);
    }

    bool nearest = false;
    if (texture.sampler >= 0 && texture.sampler < int(model.samplers.size()))
    {
        nearest = model.samplers[texture.sampler].magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST;
    }
    return {offset, width, height, nearest ? 1 : 0};
}

static std::vector<float> unpack(const tg::Model& model, const tg::Accessor& accessor)
{
    require(accessor.bufferView >= 0 && accessor.bufferView < int(model.bufferViews.size()),
            "Missing or incorrectly typed vertex attribute");
    require(accessor.count < UINT32_MAX / 4, "Vertex attribute is too large");

    const auto& bufferView = model.bufferViews[accessor.bufferView];
    require(bufferView.buffer >= 0 && bufferView.buffer < int(model.buffers.size()),
            "Vertex attribute buffer is out of range");
    const auto&          buffer = model.buffers[bufferView.buffer];
    const unsigned char* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
    size_t               stride = accessor.ByteStride(bufferView);
    size_t               components = size_t(tg::GetNumComponentsInType(accessor.type));
    size_t               elementSize = size_t(tg::GetComponentSizeInBytes(accessor.componentType));
    size_t               packedStride = stride ? stride : (components * elementSize);
    std::vector<float>   values(accessor.count * components);

    for (size_t i = 0; i < accessor.count; ++i)
    {
        const unsigned char* element = data + i * packedStride;
        for (size_t c = 0; c < components; ++c)
        {
            float value = 0.0f;
            switch (accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                value = reinterpret_cast<const float*>(element)[c];
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                value = reinterpret_cast<const unsigned char*>(element)[c] / 255.0f;
                break;
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                value = std::max(-1.0f, reinterpret_cast<const signed char*>(element)[c] / 127.0f);
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                value = reinterpret_cast<const unsigned short*>(element)[c] / 65535.0f;
                break;
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                value = std::max(-1.0f, reinterpret_cast<const short*>(element)[c] / 32767.0f);
                break;
            default:
                throw std::runtime_error("Unsupported accessor component type");
            }

            if (accessor.normalized && accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                values[i * components + c] = value;
            }
            else
            {
                values[i * components + c] = value;
            }
        }
    }
    return values;
}

Scene loadGltf(const fs::path& input)
{
    fs::path     path = fs::absolute(input);
    tg::TinyGLTF loader;
    tg::Model    model;
    std::string  err;
    std::string  warn;
    bool         ok = path.extension() == ".glb"
                          ? loader.LoadBinaryFromFile(&model, &err, &warn, path.string())
                          : loader.LoadASCIIFromFile(&model, &err, &warn, path.string());
    require(ok, "Could not parse glTF: " + path.string() + (err.empty() ? "" : "\n" + err));
    require(model.asset.version == "2.0", "glTF 2.0 required");
    for (const auto& name : model.extensionsRequired)
    {
        require(name == "KHR_texture_transform" ||
                    name == "KHR_materials_emissive_strength",
                "Unsupported required glTF extension: " + name);
    }

    Scene scene;
    scene.warnings.push_back("glTf materials use diffuse base color; "
                             "metallic/roughness are not yet shaded");
    if (!model.animations.empty())
    {
        scene.warnings.push_back(
            "Animations are not evaluated; rendering the static node transform"
        );
    }
    std::unordered_map<int, glm::ivec4> textures;
    for (size_t i = 0; i < model.materials.size(); ++i)
    {
        const auto& m = model.materials[i];
        require(m.alphaMode.empty() || m.alphaMode == "OPAQUE",
                "Alpha MASK/BLEND materials are not supported yet");
        require(m.extensions.find("KHR_materials_transmission") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_volume") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_pbrSpecularGlossiness") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_clearcoat") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_sheen") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_iridescence") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_anisotropy") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_specular") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_diffuse_transmission") == m.extensions.end() &&
                    m.extensions.find("KHR_materials_unlit") == m.extensions.end(),
                "Unsupported glTF material extension");
        require(m.emissiveTexture.index < 0 && m.normalTexture.index < 0 &&
                    m.occlusionTexture.index < 0,
                "Emissive, normal, and occlusion textures are not supported yet");

        Material material;
        material.baseColor = glm::vec4(
            float(m.pbrMetallicRoughness.baseColorFactor[0]),
            float(m.pbrMetallicRoughness.baseColorFactor[1]),
            float(m.pbrMetallicRoughness.baseColorFactor[2]),
            float(m.pbrMetallicRoughness.baseColorFactor[3])
        );
        float emissiveStrength = 1.0f;
        auto  emissiveStrengthIt = m.extensions.find("KHR_materials_emissive_strength");
        if (emissiveStrengthIt != m.extensions.end() && emissiveStrengthIt->second.IsObject())
        {
            const auto& ext = emissiveStrengthIt->second.Get<tg::Value::Object>();
            auto        found = ext.find("emissiveStrength");
            if (found != ext.end())
            {
                emissiveStrength = float(found->second.GetNumberAsDouble());
            }
        }
        material.emission = glm::vec4(
            float(m.emissiveFactor[0]) * emissiveStrength,
            float(m.emissiveFactor[1]) * emissiveStrength,
            float(m.emissiveFactor[2]) * emissiveStrength,
            0
        );

        material.settings.z = m.doubleSided ? 1 : 0;
        const auto& view = m.pbrMetallicRoughness.baseColorTexture;

        if (view.index >= 0)
        {
            auto found = textures.find(view.index);
            if (found == textures.end())
            {
                found = textures.emplace(view.index, loadTexture(scene, model, model.textures.at(view.index), path.parent_path())).first;
            }
            material.texture = found->second;
            const auto& tex = model.textures.at(view.index);
            if (tex.sampler >= 0)
            {
                const auto& sampler = model.samplers.at(tex.sampler);
                material.settings.x = sampler.wrapS;
                material.settings.y = sampler.wrapT;
            }
        }
        scene.materials.push_back(material);
    }

    uint32_t defaultMaterial = scene.addMaterial(glm::vec3(1));
    scene.materials.back().settings.z = 0;
    const tg::Scene* selected = nullptr;
    if (model.defaultScene >= 0 && model.defaultScene < int(model.scenes.size()))
    {
        selected = &model.scenes[model.defaultScene];
    }
    else if (!model.scenes.empty())
    {
        selected = &model.scenes[0];
    }
    require(selected, "glTF has no scene");

    std::vector<const tg::Node*> nodes;
    for (int nodeIndex : selected->nodes)
    {
        require(nodeIndex >= 0 && nodeIndex < int(model.nodes.size()), "Scene node out of range");
        nodes.push_back(&model.nodes[nodeIndex]);
    }
    std::vector<const tg::Node*> visited;
    for (size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const auto* node = nodes[ni];
        require(std::find(visited.begin(), visited.end(), node) == visited.end(),
                "Repeated or cyclic glTF node");
        visited.push_back(node);
        for (int childIndex : node->children)
        {
            require(childIndex >= 0 && childIndex < int(model.nodes.size()), "Child node out of range");
            nodes.push_back(&model.nodes[childIndex]);
        }

        if (node->mesh < 0)
            continue;

        glm::mat4 transform;
        if (node->matrix.size() == 16)
        {
            transform = glm::make_mat4(node->matrix.data());
        }
        else
        {
            transform = glm::mat4(1.0f);
            if (node->translation.size() == 3)
            {
                transform = glm::translate(transform, glm::vec3(node->translation[0], node->translation[1], node->translation[2]));
            }
            if (node->rotation.size() == 4)
            {
                transform *= glm::mat4_cast(glm::quat(float(node->rotation[3]), float(node->rotation[0]), float(node->rotation[1]), float(node->rotation[2])));
            }
            if (node->scale.size() == 3)
            {
                transform = glm::scale(transform, glm::vec3(node->scale[0], node->scale[1], node->scale[2]));
            }
        }
        float determinant = glm::determinant(glm::mat3(transform));
        require(std::isfinite(determinant) && std::abs(determinant) > 1e-12f,
                "Singular node transform");

        glm::mat3 normalTransform =
            glm::transpose(glm::inverse(glm::mat3(transform)));
        const auto& mesh = model.meshes.at(node->mesh);
        for (const auto& p : mesh.primitives)
        {
            require(p.mode == TINYGLTF_MODE_TRIANGLES || p.mode == -1,
                    "Only TRIANGLES glTF primitives are supported");
            require(p.targets.empty(),
                    "Draco and morph targets are not supported");
            uint32_t material =
                p.material >= 0 ? uint32_t(p.material) : defaultMaterial;
            tinygltf::TextureInfo view = {};
            int                   uvIndex = view.texCoord;
            glm::vec2             textureOffset(0.0f, 0.0f);
            float                 textureScale = 1.0f;
            float                 textureRotation = 0.0f;
            if (p.material >= 0)
            {
                view = model.materials.at(p.material).pbrMetallicRoughness.baseColorTexture;
                auto transformIt = view.extensions.find("KHR_texture_transform");
                if (transformIt != view.extensions.end() && transformIt->second.IsObject())
                {
                    const auto& transform = transformIt->second.Get<tg::Value::Object>();
                    auto        offsetIt = transform.find("offset");
                    if (offsetIt != transform.end() && offsetIt->second.IsArray() && offsetIt->second.ArrayLen() >= 2)
                    {
                        textureOffset = glm::vec2(
                            float(offsetIt->second.Get(0).GetNumberAsDouble()),
                            float(offsetIt->second.Get(1).GetNumberAsDouble())
                        );
                    }
                    auto scaleIt = transform.find("scale");
                    if (scaleIt != transform.end() && scaleIt->second.IsArray() && scaleIt->second.ArrayLen() >= 2)
                    {
                        textureScale = float(scaleIt->second.Get(0).GetNumberAsDouble());
                    }
                    auto rotationIt = transform.find("rotation");
                    if (rotationIt != transform.end())
                    {
                        textureRotation = float(rotationIt->second.GetNumberAsDouble());
                    }
                    auto texCoordIt = transform.find("texCoord");
                    if (texCoordIt != transform.end())
                    {
                        uvIndex = texCoordIt->second.GetNumberAsInt();
                    }
                }
            }
            const tg::Accessor* positions = nullptr;
            const tg::Accessor* normals = nullptr;
            const tg::Accessor* uvs = nullptr;

            for (const auto& attribute : p.attributes)
            {
                require(attribute.first != "COLOR_0",
                        "Vertex colors are not supported yet");
                if (attribute.first == "POSITION")
                    positions = &model.accessors.at(attribute.second);
                if (attribute.first == "NORMAL")
                    normals = &model.accessors.at(attribute.second);
                if (attribute.first == "TEXCOORD_0" && uvIndex == 0)
                    uvs = &model.accessors.at(attribute.second);
                if (attribute.first == "TEXCOORD_1" && uvIndex == 1)
                    uvs = &model.accessors.at(attribute.second);
            }

            require(positions != nullptr, "Missing POSITION attribute");
            auto ps = unpack(model, *positions);
            auto ns =
                normals ? unpack(model, *normals) : std::vector<float>();
            auto ts = uvs ? unpack(model, *uvs) : std::vector<float>();

            const tg::Accessor* indexAccessor = nullptr;
            if (p.indices >= 0)
            {
                indexAccessor = &model.accessors.at(p.indices);
            }
            size_t count = indexAccessor ? indexAccessor->count : positions->count;
            for (size_t k = 0; k < count; k += 3)
            {
                glm::vec3 vertices[3], shadingNormals[3];
                glm::vec2 uv[3] = {};
                for (int j = 0; j < 3; ++j)
                {
                    int    corner = determinant < 0 && j != 0 ? 3 - j : j;
                    size_t index = k + corner;
                    if (indexAccessor)
                    {
                        const auto&          bv = model.bufferViews.at(indexAccessor->bufferView);
                        const auto&          buf = model.buffers.at(bv.buffer);
                        const unsigned char* base = buf.data.data() + bv.byteOffset + indexAccessor->byteOffset;
                        size_t               stride = indexAccessor->ByteStride(bv);
                        if (stride == 0)
                        {
                            stride = tg::GetComponentSizeInBytes(indexAccessor->componentType);
                        }
                        base += (k + corner) * stride;
                        switch (indexAccessor->componentType)
                        {
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            index = *reinterpret_cast<const unsigned char*>(base);
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            index = *reinterpret_cast<const unsigned short*>(base);
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                            index = *reinterpret_cast<const unsigned int*>(base);
                            break;
                        default:
                            throw std::runtime_error("Unsupported index component type");
                        }
                    }

                    vertices[j] = glm::vec3(
                        transform * glm::vec4(glm::make_vec3(ps.data() + index * 3), 1)
                    );
                    if (normals)
                    {
                        glm::vec3 n =
                            normalTransform * glm::make_vec3(ns.data() + index * 3);
                        shadingNormals[j] = glm::normalize(n);
                    }
                    if (uvs)
                    {
                        uv[j] = glm::make_vec2(ts.data() + index * 2);
                    }
                    if (p.material >= 0)
                    {
                        glm::vec2 q = uv[j] * textureScale;
                        uv[j] = textureOffset + glm::vec2(
                                                    std::cos(textureRotation) * q.x - std::sin(textureRotation) * q.y,
                                                    std::sin(textureRotation) * q.x + std::cos(textureRotation) * q.y
                                                );
                    }
                }

                if (glm::length(glm::cross(vertices[1] - vertices[0],
                                           vertices[2] - vertices[0])) <= 1e-12f)
                {
                    continue;
                }

                scene.addTriangle(vertices[0], vertices[1], vertices[2], material);
                auto& triangle = scene.triangles.back();
                if (normals)
                {
                    triangle.n0 = glm::vec4(shadingNormals[0], 0);
                    triangle.n1 = glm::vec4(shadingNormals[1], 0);
                    triangle.n2 = glm::vec4(shadingNormals[2], 0);
                }
                triangle.uv01 = glm::vec4(uv[0], uv[1]);
                triangle.uv2 = glm::vec4(uv[2], 0, 0);
            }
        }
    }

    scene.finalize();
    scene.target = (scene.lower + scene.upper) * 0.5f;
    float radius = glm::length(scene.upper - scene.lower) * 0.5f;
    scene.eye = scene.target +
                glm::normalize(glm::vec3(1.3f, 0.8f, 1.8f) * radius * 3.6f);
    scene.environment = glm::vec3(0.35f);
    return scene;
}

void addStudio(Scene& scene)
{
    glm::vec3 center = (scene.lower + scene.upper) * 0.5f;

    float r = glm::length(scene.upper - scene.lower) * 0.5f;
    float y = scene.lower.y - r * 0.01f;

    auto ground = scene.addMaterial(glm::vec3(0.55f));
    scene.addQuad(center + glm::vec3(-4 * r, y - center.y, 4 * r),
                  center + glm::vec3(4 * r, y - center.y, 4 * r),
                  center + glm::vec3(4 * r, y - center.y, -4 * r),
                  center + glm::vec3(-4 * r, y - center.y, -4 * r), ground);

    auto      light = scene.addMaterial(glm::vec3(0), glm::vec3(9));
    glm::vec3 p = center + glm::vec3(-r, 3 * r, r);
    scene.addQuad(p + glm::vec3(-r, 0, -r), p + glm::vec3(r, 0, -r),
                  p + glm::vec3(r, 0, r), p + glm::vec3(-r, 0, r), light);

    scene.environment = glm::vec3(0.06f);
    scene.finalize();
}