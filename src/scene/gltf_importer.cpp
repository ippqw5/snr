#include "./gltf_importer.h"
#include "./scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace snr
{
namespace
{
namespace fs = std::filesystem;

static_assert(std::endian::native == std::endian::little);

// A texture may hold at most 4096 x 4096 pixels.
constexpr size_t maxTexturePixel = 16 * 1024 * 1024;

// Sentinels for "already reported as unusable".
constexpr size_t   badTexel = size_t(-1);
constexpr uint32_t badPrimitive = UINT32_MAX;

// Guards against a cyclic children list in a malformed file.
constexpr int maxNodeDepth = 256;

// glTF sampler enumerators, forwarded to the shader unchanged.
constexpr int repeat = 10497;
constexpr int clampToEdge = 33071;
constexpr int mirroredRepeat = 33648;
constexpr int nearest = 9728;

// Extensions the importer understands; any other *required* extension is rejected.
constexpr const char* supportedExtensions[] = {
    "KHR_texture_transform",
    "KHR_materials_emissive_strength",
};

bool validWrap(int mode)
{
    return mode == repeat || mode == clampToEdge || mode == mirroredRepeat;
}

std::string join(const std::vector<std::string>& values)
{
    std::string result;
    for (const auto& value : values)
    {
        if (!result.empty())
            result += ", ";
        result += value;
    }
    return result;
}

float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

const tinygltf::Value* extension(const tinygltf::ExtensionMap& extensions, const char* name)
{
    auto found = extensions.find(name);
    return found == extensions.end() ? nullptr : &found->second;
}

// Extension values are untyped, so every read checks the shape and keeps the default otherwise.
void getNumber(const tinygltf::Value& object, const char* name, int& result)
{
    if (!object.Has(name))
        return;
    const auto& value = object.Get(name);
    if (value.IsNumber())
        result = int(value.GetNumberAsDouble());
}

void getNumber(const tinygltf::Value& object, const char* name, float& result)
{
    if (!object.Has(name))
        return;
    const auto& value = object.Get(name);
    if (value.IsNumber())
        result = float(value.GetNumberAsDouble());
}

void getNumbers(const tinygltf::Value& object, const char* name, float* result, size_t count)
{
    if (!object.Has(name))
        return;
    const auto& value = object.Get(name);
    if (!value.IsArray() || value.ArrayLen() != count)
        return;
    for (size_t i = 0; i < count; ++i)
    {
        if (value.Get(i).IsNumber())
            result[i] = float(value.Get(i).GetNumberAsDouble());
    }
}

vec3 factor3(const std::vector<double>& values, vec3 fallback)
{
    if (values.size() != 3)
        return fallback;
    return vec3(float(values[0]), float(values[1]), float(values[2]));
}

vec4 factor4(const std::vector<double>& values, vec4 fallback)
{
    if (values.size() != 4)
        return fallback;
    return vec4(float(values[0]), float(values[1]), float(values[2]), float(values[3]));
}

template <typename T>
T component(const unsigned char* bytes)
{
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint32_t unsignedComponent(const unsigned char* bytes, int type)
{
    switch (type)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return *bytes;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return component<uint16_t>(bytes);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return component<uint32_t>(bytes);
    default:
        return 0;
    }
}

float floatComponent(const unsigned char* bytes, int type, bool normalized)
{
    switch (type)
    {
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return component<float>(bytes);
    case TINYGLTF_COMPONENT_TYPE_BYTE:
        return normalized ? std::max(-1.0f, component<int8_t>(bytes) / 127.0f) : float(component<int8_t>(bytes));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return normalized ? *bytes / 255.0f : float(*bytes);
    case TINYGLTF_COMPONENT_TYPE_SHORT:
        return normalized ? std::max(-1.0f, component<int16_t>(bytes) / 32767.0f) : float(component<int16_t>(bytes));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return normalized ? component<uint16_t>(bytes) / 65535.0f : float(component<uint16_t>(bytes));
    default:
        return std::numeric_limits<float>::quiet_NaN();
    }
}

// Confines every asset read to the model directory. Reporting failure by returning false makes
// tinygltf surface an error, so the sandbox holds without exceptions or assertions -- an assertion
// would silently disappear in a release build.
bool confinedPath(const std::string& filename, const fs::path& root, fs::path& result)
{
    if (filename.find('\0') != std::string::npos)
        return false;

    std::error_code code;
    const fs::path  path = fs::weakly_canonical(filename, code);
    if (code)
        return false;

    const auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    if (mismatch.first != root.end())
        return false;

    result = path;
    return true;
}

bool decodeUri(const std::string& uri, std::string* decoded, void* userData)
{
    auto       warnings = static_cast<std::vector<std::string>*>(userData);
    const auto note = [warnings](const char* message) {
        if (warnings)
            warnings->push_back(message);
    };

    if (!tinygltf::URIDecode(uri, decoded, nullptr))
        return false;

    if (decoded->find_first_of(":\\") != std::string::npos || decoded->find('\0') != std::string::npos)
    {
        note("Only relative local asset URIs are supported");
        return false;
    }

    const fs::path path(*decoded);
    if (path.is_absolute())
    {
        note("Absolute asset URIs are not supported");
        return false;
    }
    for (const auto& part : path)
    {
        if (part == "..")
        {
            note("Asset URIs must stay inside the model directory");
            return false;
        }
    }
    return true;
}

bool decodeImage(
    tinygltf::Image*     image,
    int                  index,
    std::string*         error,
    std::string*         warning,
    int                  requestedWidth,
    int                  requestedHeight,
    const unsigned char* bytes,
    int                  size,
    void*
)
{
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes, size, &width, &height, &channels) || width <= 0 || height <= 0 ||
        size_t(width) * size_t(height) > maxTexturePixel)
    {
        *error += "Invalid texture or texture exceeds the 4096 x 4096 pixel limit";
        return false;
    }

    return tinygltf::LoadImageData(
        image,
        index,
        error,
        warning,
        requestedWidth,
        requestedHeight,
        bytes,
        size,
        nullptr
    );
}

glm::mat4 nodeMatrix(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
        return glm::make_mat4(node.matrix.data());

    vec3 translation(0.0f);
    vec3 scale(1.0f);
    // glTF stores the rotation as (x, y, z, w).
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);

    if (node.translation.size() == 3)
        translation = glm::make_vec3(node.translation.data());
    if (node.scale.size() == 3)
        scale = glm::make_vec3(node.scale.data());
    if (node.rotation.size() == 4)
        rotation = glm::quat(
            float(node.rotation[3]),
            float(node.rotation[0]),
            float(node.rotation[1]),
            float(node.rotation[2])
        );

    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1.0f), scale);
}
} // namespace

//----------------------------------------------------------------------------
// Diagnostics
//----------------------------------------------------------------------------

void GltfImporter::fail(std::string message)
{
    m_scene->errors.push_back(std::move(message));
}

void GltfImporter::warn(std::string message)
{
    if (std::find(m_scene->warnings.begin(), m_scene->warnings.end(), message) == m_scene->warnings.end())
        m_scene->warnings.push_back(std::move(message));
}

void GltfImporter::ignore(std::string feature)
{
    if (std::find(m_ignored.begin(), m_ignored.end(), feature) == m_ignored.end())
        m_ignored.push_back(std::move(feature));
}

template <typename T>
const T* GltfImporter::check(const std::vector<T>& values, int index, const char* description)
{
    if (index < 0 || size_t(index) >= values.size())
    {
        fail(std::string("Invalid glTF ") + description + " index: " + std::to_string(index));
        return nullptr;
    }
    return &values[size_t(index)];
}

//----------------------------------------------------------------------------
// File loading
//----------------------------------------------------------------------------

bool GltfImporter::loadFile(const std::filesystem::path& path)
{
    std::error_code code;
    const fs::path  canonical = fs::canonical(path, code);
    if (code)
    {
        fail("Cannot open glTF file " + path.string() + ": " + code.message());
        return false;
    }

    const fs::path root = canonical.parent_path();
    const fs::path extensionName = canonical.extension();
    if (extensionName != ".gltf" && extensionName != ".glb")
    {
        fail("Expected a .gltf or .glb file");
        return false;
    }

    tinygltf::TinyGLTF loader;
    std::string        error, warning;

    tinygltf::FsCallbacks files;
    files.FileExists = [root](const std::string& name, void*) {
        fs::path resolved;
        return confinedPath(name, root, resolved) && fs::is_regular_file(resolved);
    };
    files.ExpandFilePath = [](const std::string& name, void*) { return name; };
    files.ReadWholeFile =
        [root](std::vector<unsigned char>* bytes, std::string* error, const std::string& name, void*) {
            fs::path resolved;
            if (!confinedPath(name, root, resolved))
                return false;
            return tinygltf::ReadWholeFile(bytes, error, resolved.string(), nullptr);
        };
    files.GetFileSizeInBytes = [root](size_t* size, std::string* error, const std::string& name, void*) {
        fs::path resolved;
        if (!confinedPath(name, root, resolved))
            return false;
        return tinygltf::GetFileSizeInBytes(size, error, resolved.string(), nullptr);
    };
    files.WriteWholeFile = [](std::string* error, const std::string&, const std::vector<unsigned char>&, void*) {
        *error = "The glTF importer is read-only";
        return false;
    };
    files.user_data = nullptr;

    if (!loader.SetFsCallbacks(std::move(files), &error))
    {
        fail("glTF filesystem callbacks rejected: " + error);
        return false;
    }
    if (!loader.SetURICallbacks({nullptr, decodeUri, &m_scene->warnings}, &error))
    {
        fail("glTF URI callbacks rejected: " + error);
        return false;
    }
    loader.SetImageLoader(decodeImage, nullptr);

    const bool binary = extensionName == ".glb";
    const bool loaded = binary ? loader.LoadBinaryFromFile(m_model.get(), &error, &warning, canonical.string())
                               : loader.LoadASCIIFromFile(m_model.get(), &error, &warning, canonical.string());
    if (!loaded)
    {
        fail("Could not load glTF " + canonical.string() + (error.empty() ? "" : ": " + error));
        return false;
    }
    if (m_model->asset.version != "2.0")
    {
        fail("glTF 2.0 required, found version \"" + m_model->asset.version + "\"");
        return false;
    }

    if (!warning.empty())
        warn(warning);
    if (!m_model->animations.empty())
        warn("Animations are not evaluated; rendering the static node transform");

    return validateExtensions();
}

bool GltfImporter::validateExtensions()
{
    for (const auto& name : m_model->extensionsRequired)
    {
        const auto* found = std::find(std::begin(supportedExtensions), std::end(supportedExtensions), name);
        if (found == std::end(supportedExtensions))
        {
            fail("Unsupported required glTF extension: " + name);
            return false;
        }
    }
    return true;
}

bool GltfImporter::selectScene()
{
    if (m_model->nodes.empty())
    {
        fail("glTF has no nodes");
        return false;
    }
    if (m_model->scenes.empty())
    {
        fail("glTF has no scenes");
        return false;
    }
    if (m_scene->currentScene < 0 || size_t(m_scene->currentScene) >= m_model->scenes.size())
    {
        fail("Invalid glTF scene index: " + std::to_string(m_scene->currentScene));
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------
// Derived data
//----------------------------------------------------------------------------

// The scene is prepared by the caller; a fresh Importer is used per call, so its caches start empty.
bool GltfImporter::import(int sceneIndex)
{
    // One importer outlives one derivation, so every cache that refers to the scene built last time
    // has to be dropped: resetDerived has just emptied the materials, texels and primitives they
    // point into.
    m_bindings.clear();
    m_materialIds.clear();
    m_texelBlocks.clear();
    m_primitiveCache.clear();
    m_ignored.clear();
    m_defaultBindings = MaterialBindings{};
    m_defaultMaterial = 0;
    m_degenerateTriangles = 0;

    m_scene->currentScene = sceneIndex;

    for (const auto& name : m_model->extensionsUsed)
    {
        const auto* found = std::find(std::begin(supportedExtensions), std::end(supportedExtensions), name);
        if (found == std::end(supportedExtensions))
            ignore(name);
    }

    if (!selectScene())
        return false;

    importMaterials();
    importGeometry();

    if (!m_ignored.empty())
        warn("Ignoring unsupported glTF features: " + join(m_ignored));
    if (m_degenerateTriangles > 0)
        warn("Skipped " + std::to_string(m_degenerateTriangles) + " degenerate glTF triangles");

    if (m_scene->triangles.empty())
    {
        fail("glTF scene has no triangles to render");
        return false;
    }

    m_scene->finalize();
    if (!frameCamera())
        return false;

    return true;
}

void GltfImporter::importGeometry()
{
    const tinygltf::Scene& scene = m_model->scenes[size_t(m_scene->currentScene)];
    for (int nodeIndex : scene.nodes)
        visitNode(nodeIndex, glm::mat4(1.0f), 0);
}

void GltfImporter::visitNode(int nodeIndex, const glm::mat4& parent, int depth)
{
    if (depth >= maxNodeDepth)
    {
        fail("glTF node hierarchy is too deep (possible cycle)");
        return;
    }

    const tinygltf::Node* node = check(m_model->nodes, nodeIndex, "node");
    if (!node)
        return;

    const glm::mat4 world = parent * nodeMatrix(*node);

    if (node->mesh >= 0)
    {
        if (const tinygltf::Mesh* mesh = check(m_model->meshes, node->mesh, "mesh"))
            emitMesh(*mesh, node->mesh, world);
    }

    for (int child : node->children)
        visitNode(child, world, depth + 1);
}

void GltfImporter::emitMesh(const tinygltf::Mesh& mesh, int meshIndex, const glm::mat4& world)
{
    if (!mesh.weights.empty())
        ignore("morph targets");

    for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
    {
        const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
        if (!primitive.targets.empty())
            ignore("morph targets");

        // Geometry is authored once per (mesh, primitive), in object space; every node that
        // references it only contributes an instance.
        const uint32_t shared = primitiveFor(meshIndex, int(primitiveIndex), primitive);
        if (shared == badPrimitive)
            continue;

        m_scene->addInstance(world, shared);
    }
}

uint32_t GltfImporter::primitiveFor(int meshIndex, int primitiveIndex, const tinygltf::Primitive& primitive)
{
    const uint64_t key = (uint64_t(uint32_t(meshIndex)) << 32) | uint32_t(primitiveIndex);
    if (const auto found = m_primitiveCache.find(key); found != m_primitiveCache.end())
        return found->second;

    const uint32_t index = buildPrimitive(primitive);
    m_primitiveCache.emplace(key, index);
    return index;
}

uint32_t GltfImporter::buildPrimitive(const tinygltf::Primitive& primitive)
{
    const auto positionAttribute = primitive.attributes.find("POSITION");
    if (positionAttribute == primitive.attributes.end())
    {
        fail("glTF primitive has no POSITION attribute");
        return badPrimitive;
    }

    std::vector<float> positions;
    if (!readAttribute(positionAttribute->second, TINYGLTF_TYPE_VEC3, positions))
        return badPrimitive;
    if (positions.empty() || positions.size() % 3 != 0)
    {
        fail("Malformed POSITION attribute");
        return badPrimitive;
    }
    const size_t vertexCount = positions.size() / 3;

    std::vector<float> normals;
    if (const auto found = primitive.attributes.find("NORMAL"); found != primitive.attributes.end())
    {
        if (!readAttribute(found->second, TINYGLTF_TYPE_VEC3, normals))
            return badPrimitive;
        if (normals.size() != positions.size())
        {
            fail("Mismatched NORMAL attribute count");
            return badPrimitive;
        }
    }

    const MaterialBindings* binding = &m_defaultBindings;
    uint32_t                material = m_defaultMaterial;
    if (primitive.material >= 0)
    {
        if (!check(m_model->materials, primitive.material, "material"))
            return badPrimitive;
        binding = &m_bindings[size_t(primitive.material)];
        material = m_materialIds[size_t(primitive.material)];
    }

    std::vector<glm::vec2> baseUv, roughnessUv;
    if (!readUv(primitive, binding->baseColor, vertexCount, baseUv))
        return badPrimitive;
    if (!readUv(primitive, binding->metallicRoughness, vertexCount, roughnessUv))
        return badPrimitive;

    std::vector<uint32_t> indices;
    if (!readIndices(primitive.indices, vertexCount, indices))
        return badPrimitive;

    const std::vector<uint32_t> corners = trianglesOf(primitive, indices);
    if (corners.empty())
        return badPrimitive;

    const auto uvAt = [](const std::vector<glm::vec2>& uv, uint32_t index) {
        return uv.empty() ? glm::vec2(0.0f) : uv[index];
    };

    const uint32_t first = uint32_t(m_scene->triangles.size());
    for (size_t i = 0; i + 2 < corners.size(); i += 3)
    {
        const uint32_t corner[3] = {corners[i], corners[i + 1], corners[i + 2]};
        vec3           points[3];
        for (size_t v = 0; v < 3; ++v)
        {
            const size_t offset = size_t(corner[v]) * 3;
            points[v] = vec3(positions[offset], positions[offset + 1], positions[offset + 2]);
        }

        const vec3 crossProduct = glm::cross(points[1] - points[0], points[2] - points[0]);
        // A zero cross product has no direction and would normalize to NaN.
        if (glm::dot(crossProduct, crossProduct) <= 0.0f)
        {
            ++m_degenerateTriangles;
            continue;
        }
        const vec3 geometric = glm::normalize(crossProduct);

        vec3 shading[3];
        for (size_t v = 0; v < 3; ++v)
        {
            vec3 normal = geometric;
            if (!normals.empty())
            {
                const size_t offset = size_t(corner[v]) * 3;
                const vec3   attributed(normals[offset], normals[offset + 1], normals[offset + 2]);
                if (glm::dot(attributed, attributed) > 0.0f)
                    normal = glm::normalize(attributed);
            }
            shading[v] = normal;
        }

        Triangle triangle = {};
        triangle.p0 = vec4(points[0], 0.0f);
        triangle.p1 = vec4(points[1], 0.0f);
        triangle.p2 = vec4(points[2], 0.0f);
        triangle.n0 = vec4(shading[0], 0.0f);
        triangle.n1 = vec4(shading[1], 0.0f);
        triangle.n2 = vec4(shading[2], 0.0f);
        triangle.baseColorUv01 = vec4(uvAt(baseUv, corner[0]), uvAt(baseUv, corner[1]));
        triangle.baseColorUv2 = vec4(uvAt(baseUv, corner[2]), 0.0f, 0.0f);
        triangle.metallicRoughnessUv01 =
            vec4(uvAt(roughnessUv, corner[0]), uvAt(roughnessUv, corner[1]));
        triangle.metallicRoughnessUv2 = vec4(uvAt(roughnessUv, corner[2]), 0.0f, 0.0f);
        m_scene->triangles.push_back(triangle);
    }

    if (m_scene->triangles.size() == first)
        return badPrimitive;

    return m_scene->addPrimitive(first, uint32_t(m_scene->triangles.size()) - first, material);
}

bool GltfImporter::readUv(
    const tinygltf::Primitive& primitive,
    const TextureCoordinates&  coordinates,
    size_t                     vertexCount,
    std::vector<glm::vec2>&    values
)
{
    if (coordinates.set < 0)
        return true;

    const std::string name = "TEXCOORD_" + std::to_string(coordinates.set);
    const auto        found = primitive.attributes.find(name);
    if (found == primitive.attributes.end())
    {
        warn("glTF primitive is missing the " + name + " attribute its material requires");
        return true;
    }

    std::vector<float> attribute;
    if (!readAttribute(found->second, TINYGLTF_TYPE_VEC2, attribute))
        return false;
    if (attribute.size() != vertexCount * 2)
    {
        fail("Mismatched " + name + " attribute count");
        return false;
    }

    values.resize(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i)
        values[i] = coordinates.apply(glm::vec2(attribute[i * 2], attribute[i * 2 + 1]));
    return true;
}

std::vector<uint32_t> GltfImporter::trianglesOf(
    const tinygltf::Primitive&   primitive,
    const std::vector<uint32_t>& indices
)
{
    std::vector<uint32_t> corners;
    switch (primitive.mode)
    {
    case -1:
    case TINYGLTF_MODE_TRIANGLES:
        if (indices.size() % 3 != 0)
        {
            fail("TRIANGLES primitive has an incomplete triangle");
            return {};
        }
        corners = indices;
        break;
    case TINYGLTF_MODE_TRIANGLE_STRIP:
        for (size_t i = 2; i < indices.size(); ++i)
        {
            const bool alternate = ((i - 2) % 2) != 0;
            corners.push_back(alternate ? indices[i - 1] : indices[i - 2]);
            corners.push_back(alternate ? indices[i - 2] : indices[i - 1]);
            corners.push_back(indices[i]);
        }
        break;
    case TINYGLTF_MODE_TRIANGLE_FAN:
        for (size_t i = 2; i < indices.size(); ++i)
        {
            corners.push_back(indices[0]);
            corners.push_back(indices[i - 1]);
            corners.push_back(indices[i]);
        }
        break;
    default:
        ignore("primitive modes other than TRIANGLES, TRIANGLE_STRIP and TRIANGLE_FAN");
        break;
    }
    return corners;
}

bool GltfImporter::frameCamera()
{
    m_scene->target = (m_scene->lower + m_scene->upper) * 0.5f;
    const float radius = glm::length(m_scene->upper - m_scene->lower) * 0.5f;
    if (!(radius > 0.0f))
    {
        fail("glTF scene has no extent");
        return false;
    }

    m_scene->eye = m_scene->target + glm::normalize(vec3(1.3f, 0.8f, 1.8f)) * radius * 3.6f;
    m_scene->environment = vec3(0.35f);
    return true;
}

//----------------------------------------------------------------------------
// Materials and textures
//----------------------------------------------------------------------------

glm::vec2 GltfImporter::TextureCoordinates::apply(glm::vec2 uv) const
{
    return glm::vec2(transform * glm::vec3(uv, 1.0f));
}

GltfImporter::TextureCoordinates GltfImporter::textureCoordinates(const tinygltf::TextureInfo& info)
{
    TextureCoordinates result;
    result.set = info.texCoord;

    glm::vec2 offset(0.0f);
    glm::vec2 scale(1.0f);
    float     rotation = 0.0f;

    if (const tinygltf::Value* transform = extension(info.extensions, "KHR_texture_transform"))
    {
        if (!transform->IsObject())
        {
            warn("Invalid KHR_texture_transform; ignoring it");
        }
        else
        {
            getNumbers(*transform, "offset", &offset.x, 2);
            getNumbers(*transform, "scale", &scale.x, 2);
            getNumber(*transform, "rotation", rotation);

            int texCoord = result.set;
            getNumber(*transform, "texCoord", texCoord);
            if (texCoord < 0)
                warn("Invalid KHR_texture_transform texCoord; using the material's");
            else
                result.set = texCoord;
        }
    }

    // UV' = translation * rotation * scale, per the extension specification.
    const float cosine = std::cos(rotation);
    const float sine = std::sin(rotation);
    result.transform = glm::mat3(
        cosine * scale.x,
        sine * scale.x,
        0.0f,
        -sine * scale.y,
        cosine * scale.y,
        0.0f,
        offset.x,
        offset.y,
        1.0f
    );
    return result;
}

int GltfImporter::imageOf(const tinygltf::Texture& texture)
{
    if (extension(texture.extensions, "KHR_texture_basisu"))
    {
        ignore("KHR_texture_basisu textures");
        return -1;
    }
    if (texture.source < 0)
    {
        fail("glTF texture has no image source");
        return -1;
    }
    return texture.source;
}

bool GltfImporter::texelsFor(int imageIndex, bool srgb, TexelBlock& block)
{
    const uint64_t key = (uint64_t(uint32_t(imageIndex)) << 1) | uint64_t(srgb ? 1 : 0);
    if (const auto found = m_texelBlocks.find(key); found != m_texelBlocks.end())
    {
        block = found->second;
        return block.offset != badTexel;
    }

    const auto remember = [this, key](TexelBlock value) {
        m_texelBlocks.emplace(key, value);
        return value;
    };
    const auto reject = [&remember](const char* message) {
        remember(TexelBlock{badTexel, 0, 0});
        return message;
    };

    const tinygltf::Image* image = check(m_model->images, imageIndex, "image");
    if (!image)
        return remember(TexelBlock{badTexel, 0, 0}), false;
    if (image->width <= 0 || image->height <= 0)
    {
        fail(reject("Invalid glTF image size"));
        return false;
    }
    if (image->component != 4)
    {
        fail(reject("Expected a decoded RGBA glTF image"));
        return false;
    }
    if (image->bits != 8 && image->bits != 16)
    {
        fail(reject("Unsupported glTF image bit depth"));
        return false;
    }

    const size_t bytesPerComponent = size_t(image->bits) / 8;
    const size_t pixelCount = size_t(image->width) * size_t(image->height);
    if (image->image.size() < pixelCount * 4 * bytesPerComponent)
    {
        fail(reject("Truncated glTF image data"));
        return false;
    }

    TexelBlock result;
    result.offset = m_scene->texels.size();
    result.width = image->width;
    result.height = image->height;
    if (result.offset + pixelCount > size_t(INT32_MAX))
    {
        fail(reject("Texture data exceeds the addressable texel range"));
        return false;
    }

    m_scene->texels.resize(result.offset + pixelCount);
    const unsigned char* data = image->image.data();
    for (size_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const size_t base = pixel * 4 * bytesPerComponent;
        float        channels[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (size_t c = 0; c < 4; ++c)
        {
            if (bytesPerComponent == 2)
            {
                const size_t offset = base + c * 2;
                channels[c] = float(uint16_t(data[offset]) | (uint16_t(data[offset + 1]) << 8)) / 65535.0f;
            }
            else
            {
                channels[c] = float(data[base + c]) / 255.0f;
            }
        }

        vec4 texel(channels[0], channels[1], channels[2], channels[3]);
        if (srgb)
            texel = vec4(srgbToLinear(texel.r), srgbToLinear(texel.g), srgbToLinear(texel.b), texel.a);
        m_scene->texels[result.offset + pixel] = texel;
    }

    block = remember(result);
    return true;
}

void GltfImporter::bindTexture(
    const tinygltf::TextureInfo& info,
    bool                         srgb,
    TextureCoordinates&          coordinates,
    ivec4&                       texture,
    uvec4&                       sampler
)
{
    // An absent texture leaves the slot empty. The shader treats a negative offset as "no texture"
    // and samples white, so the material factor is then used on its own.
    texture = ivec4(-1, 0, 0, 0);
    sampler = uvec4(repeat, repeat, 0, 0);
    if (info.index < 0)
        return;

    coordinates = textureCoordinates(info);

    const tinygltf::Texture* source = check(m_model->textures, info.index, "texture");
    if (!source)
        return;

    int wrapS = repeat, wrapT = repeat, filter = 0;
    if (source->sampler >= 0)
    {
        if (const tinygltf::Sampler* gltfSampler = check(m_model->samplers, source->sampler, "sampler"))
        {
            wrapS = gltfSampler->wrapS;
            wrapT = gltfSampler->wrapT;
            filter = gltfSampler->magFilter == nearest ? 1 : 0;
        }
    }
    if (!validWrap(wrapS) || !validWrap(wrapT))
    {
        warn("Unsupported glTF sampler wrap mode; using repeat");
        wrapS = repeat;
        wrapT = repeat;
    }

    const int imageIndex = imageOf(*source);
    if (imageIndex < 0)
        return;

    TexelBlock block;
    if (!texelsFor(imageIndex, srgb, block))
        return;

    texture = ivec4(int(block.offset), block.width, block.height, 0);
    sampler = uvec4(uint32_t(wrapS), uint32_t(wrapT), uint32_t(filter), 0);
}

void GltfImporter::reportUnsupportedMaterial(const tinygltf::Material& material)
{
    if (material.emissiveTexture.index >= 0)
        ignore("emissive textures");
    if (material.normalTexture.index >= 0)
        ignore("normal maps");
    if (material.occlusionTexture.index >= 0)
        ignore("occlusion maps");
    if (material.alphaMode != "OPAQUE")
        ignore("alpha modes other than OPAQUE (" + material.alphaMode + ")");
}

void GltfImporter::importMaterials()
{
    m_bindings.reserve(m_model->materials.size());
    m_materialIds.reserve(m_model->materials.size());

    for (const tinygltf::Material& source : m_model->materials)
    {
        const tinygltf::PbrMetallicRoughness& pbr = source.pbrMetallicRoughness;

        GltfMaterial material;
        material.pbrBaseColorFactor = factor4(pbr.baseColorFactor, vec4(1.0f));
        material.pbrMetallicRoughnessFactor.x = float(pbr.metallicFactor);
        material.pbrMetallicRoughnessFactor.y = float(pbr.roughnessFactor);
        material.flags.x = source.doubleSided ? 1u : 0u;
        material.flags.y = 1u; // Enable the metallic-roughness response.

        float emissiveStrength = 1.0f;
        if (const tinygltf::Value* ext = extension(source.extensions, "KHR_materials_emissive_strength"))
        {
            if (ext->IsObject())
                getNumber(*ext, "emissiveStrength", emissiveStrength);
            else
                warn("Invalid KHR_materials_emissive_strength; using the default");
        }
        if (emissiveStrength < 0.0f)
        {
            warn("Negative KHR_materials_emissive_strength; clamping to zero");
            emissiveStrength = 0.0f;
        }

        // emissiveFactor only scales the emissive texture, so without texture support keeping it
        // would make every surface of the material emit white.
        const bool textured = source.emissiveTexture.index >= 0;
        const vec3 emitted = textured ? vec3(0.0f) : factor3(source.emissiveFactor, vec3(0.0f));
        material.emissiveFactor = vec4(emitted * emissiveStrength, 0.0f);

        MaterialBindings binding;
        bindTexture(
            pbr.baseColorTexture,
            true,
            binding.baseColor,
            material.pbrBaseColorTexture,
            material.pbrBaseColorSampler
        );
        bindTexture(
            pbr.metallicRoughnessTexture,
            false,
            binding.metallicRoughness,
            material.pbrMetallicRoughnessTexture,
            material.pbrMetallicRoughnessSampler
        );

        reportUnsupportedMaterial(source);

        m_materialIds.push_back(uint32_t(m_scene->materials.size()));
        m_bindings.push_back(binding);
        m_scene->materials.push_back(material);
    }

    // The glTF default material: white, fully metallic, fully rough, single sided.
    GltfMaterial fallback;
    fallback.pbrBaseColorFactor = vec4(1.0f);
    fallback.pbrMetallicRoughnessFactor = vec4(1.0f, 1.0f, 0.0f, 0.0f);
    fallback.flags = uvec4(0, 1, 0, 0);
    m_defaultMaterial = uint32_t(m_scene->materials.size());
    m_scene->materials.push_back(fallback);
}

//----------------------------------------------------------------------------
// Accessors
//----------------------------------------------------------------------------

bool GltfImporter::viewOf(int bufferViewIndex, size_t offset, size_t elementSize, size_t count, AccessorView& view)
{
    const tinygltf::BufferView* bufferView = check(m_model->bufferViews, bufferViewIndex, "buffer view");
    if (!bufferView)
        return false;

    const tinygltf::Buffer* buffer = check(m_model->buffers, bufferView->buffer, "buffer");
    if (!buffer)
        return false;

    const size_t viewStart = size_t(bufferView->byteOffset);
    const size_t viewLength = size_t(bufferView->byteLength);
    if (viewStart > buffer->data.size() || viewLength > buffer->data.size() - viewStart)
    {
        fail("glTF buffer view extends past its buffer");
        return false;
    }
    if (offset > viewLength)
    {
        fail("glTF accessor offset is outside its buffer view");
        return false;
    }

    const size_t stride = bufferView->byteStride > 0 ? size_t(bufferView->byteStride) : elementSize;
    if (stride < elementSize)
    {
        fail("glTF buffer view stride is smaller than its elements");
        return false;
    }

    if (count > 0)
    {
        const size_t last = count - 1;
        if (last > (std::numeric_limits<size_t>::max() - elementSize) / stride)
        {
            fail("glTF accessor size overflows");
            return false;
        }
        if (last * stride + elementSize > viewLength - offset)
        {
            fail("glTF accessor reads past its buffer view");
            return false;
        }
    }

    view.bytes = std::span<const unsigned char>(buffer->data).subspan(viewStart + offset, viewLength - offset);
    view.stride = stride;
    return true;
}

bool GltfImporter::readAttribute(int accessorIndex, int type, std::vector<float>& values)
{
    const tinygltf::Accessor* accessor = check(m_model->accessors, accessorIndex, "accessor");
    if (!accessor)
        return false;
    if (accessor->type != type)
    {
        fail("Incorrectly typed vertex attribute");
        return false;
    }

    const size_t components = size_t(tinygltf::GetNumComponentsInType(uint32_t(type)));
    const int    componentSize = tinygltf::GetComponentSizeInBytes(uint32_t(accessor->componentType));
    if (componentSize <= 0)
    {
        fail("Unsupported vertex component type");
        return false;
    }

    const size_t count = size_t(accessor->count);
    AccessorView base;
    if (accessor->bufferView >= 0)
    {
        if (!viewOf(
                accessor->bufferView,
                size_t(accessor->byteOffset),
                components * size_t(componentSize),
                count,
                base
            ))
            return false;
    }
    else if (accessor->byteOffset != 0)
    {
        fail("Accessor without a buffer view has an offset");
        return false;
    }

    values.assign(count * components, 0.0f);
    const auto copyElement = [&](const unsigned char* bytes, size_t destination) {
        for (size_t c = 0; c < components; ++c)
        {
            const float value =
                floatComponent(bytes + c * size_t(componentSize), accessor->componentType, accessor->normalized);
            if (!std::isfinite(value))
            {
                fail("Non-finite vertex attribute");
                return false;
            }
            values[destination * components + c] = value;
        }
        return true;
    };

    if (accessor->bufferView >= 0)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (!copyElement(base[i], i))
                return false;
        }
    }

    if (accessor->sparse.isSparse)
    {
        const tinygltf::Accessor::Sparse& sparse = accessor->sparse;
        if (sparse.count <= 0 || size_t(sparse.count) > count)
        {
            fail("Invalid sparse count");
            return false;
        }

        const int indexSize = tinygltf::GetComponentSizeInBytes(uint32_t(sparse.indices.componentType));
        if (indexSize <= 0)
        {
            fail("Invalid sparse index type");
            return false;
        }

        AccessorView indexView;
        if (!viewOf(
                sparse.indices.bufferView,
                size_t(sparse.indices.byteOffset),
                size_t(indexSize),
                size_t(sparse.count),
                indexView
            ))
            return false;

        AccessorView valueView;
        if (!viewOf(
                sparse.values.bufferView,
                size_t(sparse.values.byteOffset),
                components * size_t(componentSize),
                size_t(sparse.count),
                valueView
            ))
            return false;

        for (size_t i = 0; i < size_t(sparse.count); ++i)
        {
            const uint32_t destination = unsignedComponent(indexView[i], sparse.indices.componentType);
            if (destination >= count)
            {
                fail("Sparse index is out of range");
                return false;
            }
            if (!copyElement(valueView[i], destination))
                return false;
        }
    }
    return true;
}

bool GltfImporter::readIndices(int accessorIndex, size_t vertexCount, std::vector<uint32_t>& indices)
{
    if (accessorIndex < 0)
    {
        indices.resize(vertexCount);
        std::iota(indices.begin(), indices.end(), 0u);
        return true;
    }

    const tinygltf::Accessor* accessor = check(m_model->accessors, accessorIndex, "accessor");
    if (!accessor)
        return false;
    if (accessor->sparse.isSparse)
    {
        fail("Sparse index accessors are not supported");
        return false;
    }
    if (accessor->type != TINYGLTF_TYPE_SCALAR || accessor->normalized)
    {
        fail("Indices must be unnormalized scalar");
        return false;
    }
    if (accessor->componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        accessor->componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
        accessor->componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
    {
        fail("Indices must use unsigned integer components");
        return false;
    }

    const int    componentSize = tinygltf::GetComponentSizeInBytes(uint32_t(accessor->componentType));
    const size_t count = size_t(accessor->count);
    AccessorView view;
    if (!viewOf(accessor->bufferView, size_t(accessor->byteOffset), size_t(componentSize), count, view))
        return false;

    indices.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        indices[i] = unsignedComponent(view[i], accessor->componentType);
        if (indices[i] >= vertexCount)
        {
            fail("Triangle index is out of range");
            return false;
        }
    }
    return true;
}

//----------------------------------------------------------------------------
// GltfImporter
//----------------------------------------------------------------------------

bool GltfImporter::load(Scene& scene, const std::filesystem::path& path)
{
    m_scene = &scene;

    // Reset before loading so the diagnostics the load itself produces survive.
    scene.resetDerived();

    m_model = std::make_shared<tinygltf::Model>();

    if (!loadFile(path))
    {
        // Nothing usable was imported, so there is no model worth keeping.
        m_model.reset();
        return false;
    }

    return import(m_model->defaultScene >= 0 ? m_model->defaultScene : 0);
}

bool GltfImporter::derive(Scene& scene, int sceneIndex)
{
    m_scene = &scene;
    if (!m_model)
    {
        scene.errors = {"No glTF model is loaded"};
        return false;
    }

    // Validated before the reset so an out-of-range index leaves the scene intact.
    if (sceneIndex < 0 || size_t(sceneIndex) >= m_model->scenes.size())
    {
        scene.errors = {"Invalid glTF scene index: " + std::to_string(sceneIndex)};
        return false;
    }

    scene.resetDerived();

    return import(sceneIndex);
}
} // namespace snr