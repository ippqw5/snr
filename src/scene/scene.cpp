#include "./scene.h"
#include "glm/ext/quaternion_geometric.hpp"
#include "glm/ext/vector_float3.hpp"
#include "glm/matrix.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <cmath>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

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
    triangles.push_back(
        {
            glm::vec4(a, 0),
            glm::vec4(b, 0),
            glm::vec4(c, 0),
            glm::vec4(normal, 0),
            glm::vec4(normal, 0),
            glm::vec4(normal, 0),
            glm::vec4(0),
            glm::vec4(0),
            glm::uvec4(material, UINT32_MAX, 0, 0),
        }
    );
}

void Scene::addQuad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material)
{
    addTriangle(a, b, c, material);
    addTriangle(a, c, b, material);
}

void Scene::finalize()
{
    require(!triangles.empty(), "Scene has no non-degenerate triangles");
    require(triangles.size() < UINT32_MAX / 3, "Scene exceeds 32-bit geometry limits");
    lower = glm::vec3(std::numeric_limits<float>::max());
    upper = -lower;
    lights.clear();
    for (size_t i = 0; i < triangles.size(); ++i)
    {
        auto& t = triangles[i];
        for (glm::vec3 p : {glm::vec3(t.p0), glm::vec3(t.p1), glm::vec3(t.p2)})
        {
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Non-finite vertex position");
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
    s.addQuad({-0.33f, 1.985f, -0.3f}, {0.33f, 1.985f, -0.3f}, {0.33f, 1.985f, 0.25f}, {-0.33f, 1.985f, 0.25f}, light);

    addBox(s, {-0.4f, 0.32f, 0.35f}, {0.62f, 0.64f, 0.62f}, -0.28f, white);
    addBox(s, {0.36f, 0.63f, -0.3f}, {0.62f, 1.26f, 0.62f}, 0.3f, white);
    s.finalize();
    return s;
}

Scene makeFurnaceScene()
{
    Scene s;
    auto  matte = s.addMaterial(glm::vec3(0.6f, 0.4f, 0.2f));
    s.addQuad({-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {-100, 100, 0}, matte);
    s.eye = {0, 0, 2};
    s.target = {0, 0, 0};
    s.environment = glm::vec3(1);
    s.finalize();
    return s;
}

static fs::path localUri(const fs::path& parent, const char* uri)
{
    require(uri && !std::strchr(uri, ':') && !std::strchr(uri, '\\'), "Only relative local assets URIs are supported");

    std::string decoded(uri);
    cgltf_decode_uri(decoded.data());
    decoded.resize(std::strlen(decoded.c_str()));

    fs::path relative(decoded);
    require(!relative.is_absolute(), "Absolute asset URIs are not supported");

    for (const auto& part : relative)
    {
        require(part != "..", "Asset URIs must stay inside the model directory");
    }

    const auto root = fs::weakly_canonical(parent);
    const auto result = fs::weakly_canonical(parent / relative);

    auto mismatch = std::mismatch(root.begin(), root.end(), result.begin(), result.end());
    require(mismatch.first == root.end(), "Asset symlink escapes the model directory");
    return result;
}

static float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

static glm::ivec4 loadTexture(Scene& scene, const cgltf_texture* texture, const fs::path& parent)
{
    require(texture && texture->image, "Missing texture image (compressed textures unsupported)");

    const auto* image = texture->image;
    int         width = 0, height = 0, channels = 0;
    stbi_uc*    pixels = nullptr;
    if (image->buffer_view)
    {
        const auto* view = image->buffer_view;
        require(view->size <= INT32_MAX, "Texture exceeds decoder limits");
        const auto* bytes = cgltf_buffer_view_data(view);
        require(bytes, "Image buffer view has no data");
        pixels = stbi_load_from_memory(bytes, int(view->size), &width, &height, &channels, 4);
    }
    else
    {
        auto path = localUri(parent, image->uri);
        pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    }

    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> owner(pixels, stbi_image_free);
    require(pixels != nullptr, "Could not decode PNG/JPEG texture");
    require(width > 0 && height > 0 && size_t(width) * size_t(height) <= 16777216,
            "Texture exceeds 16 million pixel limits");
    require(scene.texels.size() + size_t(width) * height <= INT32_MAX, "Texture atlas too large");

    int offset = int(scene.texels.size());
    for (size_t i = 0; i < size_t(width) * height; ++i)
    {
        scene.texels.emplace_back(
            srgbToLinear(pixels[i * 4] / 255.f), srgbToLinear(pixels[i * 4 + 1] / 255.f),
            srgbToLinear(pixels[i * 4 + 2] / 255.f), pixels[i * 4 + 3] / 255.f
        );
    }

    bool nearest = texture->sampler && texture->sampler->mag_filter == cgltf_filter_type_nearest;
    return {offset, width, height, nearest ? 1 : 0};
}

static std::vector<float> unpack(const cgltf_accessor* accessor, cgltf_type type)
{
    require(accessor && accessor->type == type, "Missing or incorrectly typed vertex attribute");
    require(accessor->count < UINT32_MAX / 4, "Vertex attribute is too large");

    std::vector<float> values(accessor->count * cgltf_num_components(type));
    require(cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) == values.size(),
            "Could not unpack vertex attribute");
    for (float value : values)
    {
        // require(std::isfinite(value), "Non-finite vertex attribute");
    }
    return values;
}

Scene loadGltf(const fs::path& input)
{
    fs::path      path = fs::absolute(input);
    cgltf_options options = {};
    cgltf_data*   raw = nullptr;
    require(cgltf_parse_file(&options, path.c_str(), &raw) == cgltf_result_success,
            "Could not parse glTF: " + path.string());

    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    require(data->asset.version && std::string(data->asset.version) == "2.0", "glTF 2.0 required");
    require(cgltf_validate(data.get()) == cgltf_result_success, "Invalid glTF structure");

    for (size_t i = 0; i < data->extensions_required_count; i++)
    {
        std::string name(data->extensions_required[i]);
        require(name == "KHR_texture_transform" || name == "KHR_materials_emissive_strength",
                "Unsupported required glTF extension: " + name);
    }

    for (size_t i = 0; i < data->buffers_count; i++)
    {
        const char* uri = data->buffers[i].uri;
        if (uri && std::strncmp(uri, "data:", 5) != 0)
        {
            localUri(path.parent_path(), uri);
        }
    }

    require(cgltf_load_buffers(&options, data.get(), path.c_str()) == cgltf_result_success,
            "Could not load gltf buffers");
    require(cgltf_validate(data.get()) == cgltf_result_success, "Invalid glTF buffer data");

    Scene scene;
    scene.warnings.push_back("glTf materials use diffuse base color; metallic/roughness are not yet shaded");
    if (data->animations_count)
    {
        scene.warnings.push_back("Animations are not evaluated; rendering the static node transform");
    }
    std::unordered_map<const cgltf_texture*, glm::ivec4> textures;
    for (size_t i = 0; i < data->materials_count; ++i)
    {
        const auto& m = data->materials[i];
        require(m.alpha_mode == cgltf_alpha_mode_opaque, "Alpha MASK/BLEND materials are not supported yet");
        require(!m.has_transmission && !m.has_volume && !m.has_pbr_specular_glossiness &&
                    !m.has_clearcoat && !m.has_sheen && !m.has_iridescence && !m.has_anisotropy && !m.has_specular && !m.has_diffuse_transmission && !m.unlit,
                "Unsupported glTF material extension");
        require(!m.emissive_texture.texture && !m.normal_texture.texture && !m.occlusion_texture.texture, "Emissive, normal, and occlusion textures are not supported yet");

        Material material;
        material.baseColor = glm::make_vec4(m.pbr_metallic_roughness.base_color_factor);
        material.emission = glm::vec4(glm::make_vec3(m.emissive_factor) * (m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f), 0);

        material.settings.z = m.double_sided ? 1 : 0;
        const auto& view = m.pbr_metallic_roughness.base_color_texture;

        if (view.texture)
        {
            auto found = textures.find(view.texture);
            if (found == textures.end())
            {
                found = textures.emplace(view.texture, loadTexture(scene, view.texture, path.parent_path())).first;
            }
            material.texture = found->second;
            if (view.texture->sampler)
            {
                material.settings.x = view.texture->sampler->wrap_s;
                material.settings.y = view.texture->sampler->wrap_t;
            }
        }
        scene.materials.push_back(material);
    }

    uint32_t defaultMaterial = scene.addMaterial(glm::vec3(1));
    scene.materials.back().settings.z = 0;
    auto* selected = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    require(selected, "glTF has no scene");

    std::vector<const cgltf_node*> nodes(selected->nodes, selected->nodes + selected->nodes_count);
    std::vector<const cgltf_node*> visited;
    for (size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const auto* node = nodes[ni];
        require(std::find(visited.begin(), visited.end(), node) == visited.end(),
                "Repeated or cyclic glTF node");
        visited.push_back(node);
        for (size_t i = 0; i < node->children_count; ++i)
        {
            nodes.push_back(node->children[i]);
        }

        if (!node->mesh)
            continue;

        glm::mat4 transform;
        cgltf_node_transform_world(node, glm::value_ptr(transform));
        float determinant = glm::determinant(glm::mat3(transform));
        require(std::isfinite(determinant) && std::abs(determinant) > 1e-12f, "Singular node transform");

        glm::mat3 normalTransform = glm::transpose(glm::inverse(glm::mat3(transform)));
        for (size_t pi = 0; pi < node->mesh->primitives_count; ++pi)
        {
            const auto& p = node->mesh->primitives[pi];
            require(p.type == cgltf_primitive_type_triangles, "Only TRIANGLES glTF primitives are supported");
            require(!p.has_draco_mesh_compression && !p.targets_count, "Draco and morph targets are not supported");
            uint32_t           material = p.material ? uint32_t(p.material - data->materials) : defaultMaterial;
            cgltf_texture_view view = {};
            if (p.material)
            {
                view = p.material->pbr_metallic_roughness.base_color_texture;
            }

            int                   uvIndex = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
            const cgltf_accessor *positions = nullptr, *normals = nullptr, *uvs = nullptr;

            for (size_t ai = 0; ai < p.attributes_count; ++ai)
            {
                const auto& a = p.attributes[ai];
                if (a.type == cgltf_attribute_type_position)
                    positions = a.data;
                if (a.type == cgltf_attribute_type_normal)
                    normals = a.data;
                if (a.type == cgltf_attribute_type_texcoord && a.index == uvIndex)
                    uvs = a.data;

                require(a.type != cgltf_attribute_type_color, "Vertex colors are not supported yet");
            }

            auto ps = unpack(positions, cgltf_type_vec3);
            auto ns = normals ? unpack(normals, cgltf_type_vec3) : std::vector<float>();
            auto ts = uvs ? unpack(uvs, cgltf_type_vec2) : std::vector<float>();

            size_t count = p.indices ? p.indices->count : positions->count;
            for (size_t k = 0; k < count; k += 3)
            {
                glm::vec3 vertices[3], shadingNormals[3];
                glm::vec2 uv[3] = {};
                for (int j = 0; j < 3; ++j)
                {
                    int    corner = determinant < 0 && j != 0 ? 3 - j : j;
                    size_t index = p.indices ? cgltf_accessor_read_index(p.indices, k + corner) : k + corner;

                    vertices[j] = glm::vec3(transform * glm::vec4(glm::make_vec3(ps.data() + index * 3), 1));
                    if (normals)
                    {
                        glm::vec3 n = normalTransform * glm::make_vec3(ns.data() + index * 3);
                        shadingNormals[j] = glm::normalize(n);
                    }
                    if (uvs)
                    {
                        uv[j] = glm::make_vec2(ts.data() + index * 2);
                    }
                    if (view.has_transform)
                    {
                        const auto& t = view.transform;
                        glm::vec2   q = uv[j] * glm::make_vec2(t.scale);
                        uv[j] = glm::make_vec2(t.offset) +
                                glm::vec2(std::cos(t.rotation) * q.x - std::sin(t.rotation) * q.y,
                                          std::sin(t.rotation) * q.x + std::cos(t.rotation) * q.y);
                    }
                }

                if (glm::length(glm::cross(vertices[1] - vertices[0], vertices[2] - vertices[0])) <= 1e-12f)
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
    scene.eye = scene.target + glm::normalize(glm::vec3(1.3f, 0.8f, 1.8f) * radius * 3.6f);
    scene.environment = glm::vec3(0.35f);
    return scene;
}

void addStudio(Scene& scene)
{
    glm::vec3 center = (scene.lower + scene.upper) * 0.5f;

    float r = glm::length(scene.upper - scene.lower) * 0.5f;
    float y = scene.lower.y - r * 0.01f;

    auto ground = scene.addMaterial(glm::vec3(0.55f));
    scene.addQuad(
        center + glm::vec3(-4 * r, y - center.y, 4 * r),
        center + glm::vec3(4 * r, y - center.y, 4 * r),
        center + glm::vec3(4 * r, y - center.y, -4 * r),
        center + glm::vec3(-4 * r, y - center.y, -4 * r),
        ground
    );

    auto      light = scene.addMaterial(glm::vec3(0), glm::vec3(9));
    glm::vec3 p = center + glm::vec3(-r, 3 * r, r);
    scene.addQuad(p + glm::vec3(-r, 0, -r), p + glm::vec3(r, 0, -r), p + glm::vec3(r, 0, r), p + glm::vec3(-r, 0, r), light);

    scene.environment = glm::vec3(0.06f);
    scene.finalize();
}