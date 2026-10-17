#include "./scene.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace snr
{
namespace
{
// The rows are stored explicitly, matching how the shader transforms a point.
vec3 transformPoint(const vec4 rows[3], vec3 point)
{
    return vec3(
        glm::dot(vec3(rows[0]), point) + rows[0].w,
        glm::dot(vec3(rows[1]), point) + rows[1].w,
        glm::dot(vec3(rows[2]), point) + rows[2].w
    );
}

void setTransform(Instance& instance, const glm::mat4& transform)
{
    for (int row = 0; row < 3; ++row)
    {
        instance.transformRows[row] =
            vec4(transform[0][row], transform[1][row], transform[2][row], transform[3][row]);
    }

    const glm::mat3 linear(transform);
    // A singular transform has no inverse, so keep the linear part instead of producing infinities.
    const glm::mat3 normal =
        std::abs(glm::determinant(linear)) > 0.0f ? glm::transpose(glm::inverse(linear)) : linear;
    for (int row = 0; row < 3; ++row)
    {
        instance.normalRows[row] = vec4(normal[0][row], normal[1][row], normal[2][row], 0.0f);
    }
}

// Folds a texture coordinate into [0, 1) the way the sampler would, so the host test below agrees
// with the texture unit about which texel a UV reads.
float wrapCoordinate(float coordinate, TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return glm::clamp(coordinate, 0.0f, 1.0f);
    case TextureWrap::MirrorRepeat: {
        const float period = std::floor(coordinate);
        const float fraction = coordinate - period;
        return (int(period) & 1) == 0 ? fraction : 1.0f - fraction;
    }
    default:
        return coordinate - std::floor(coordinate);
    }
}

// Nearest read of the emissive texture's base level, matching the sampler's wrap. This only decides
// whether a triangle emits at all; the shader still samples the texture itself, with filtering.
vec3 sampleEmissive(const TextureData& texture, const TextureSampler& sampler, vec2 uv)
{
    if (texture.levels.empty() || texture.width <= 0 || texture.height <= 0)
        return vec3(0.0f);

    const float                 u = wrapCoordinate(uv.x, sampler.wrapU);
    const float                 v = wrapCoordinate(uv.y, sampler.wrapV);
    const int                   x = glm::clamp(int(u * float(texture.width)), 0, texture.width - 1);
    const int                   y = glm::clamp(int(v * float(texture.height)), 0, texture.height - 1);
    const std::vector<uint8_t>& level = texture.levels.front();
    const size_t                offset = (size_t(y) * size_t(texture.width) + size_t(x)) * 4;
    if (offset + 3 >= level.size())
        return vec3(0.0f);

    vec3 color(
        float(level[offset + 0]) / 255.0f,
        float(level[offset + 1]) / 255.0f,
        float(level[offset + 2]) / 255.0f
    );
    // The texture unit decodes sRGB before the shader sees it, so decode here too or a dark sRGB texel
    // would look brighter on the host than it does in the shader.
    return texture.srgb ? glm::pow(color, vec3(2.2f)) : color;
}
} // namespace

uint32_t Scene::addMaterial(vec3 color, vec3 emission)
{
    GltfMaterial material;
    material.pbrBaseColorFactor = vec4(color, 1);
    material.emissiveFactor = vec4(emission, 0);
    materials.push_back(material);
    return uint32_t(materials.size() - 1);
}

void Scene::addQuad(vec3 a, vec3 b, vec3 c, vec3 d, uint32_t material)
{
    const vec3 normal = glm::normalize(glm::cross(b - a, c - a));

    // A quad needs four vertices, not six: the two triangles share the edge a-c.
    const uint32_t vertexBase = uint32_t(positions.size());
    for (vec3 point : {a, b, c, d})
    {
        positions.push_back(point);
        normals.push_back(normal);
        baseColorUvs.push_back(vec2(0.0f));
        metallicRoughnessUvs.push_back(vec2(0.0f));
        normalUvs.push_back(vec2(0.0f));
        occlusionUvs.push_back(vec2(0.0f));
        emissiveUvs.push_back(vec2(0.0f));
        transmissionUvs.push_back(vec2(0.0f));
    }

    const uint32_t first = uint32_t(indices.size());
    indices.push_back(uvec3(0, 1, 2));
    indices.push_back(uvec3(0, 2, 3));
    addInstance(glm::mat4(1.0f), addPrimitive(first, 2, vertexBase, 4, material));
}

uint32_t Scene::addPrimitive(
    uint32_t firstTriangle,
    uint32_t triangleCount,
    uint32_t vertexBase,
    uint32_t vertexCount,
    uint32_t material
)
{
    assert(triangleCount > 0);
    assert(size_t(firstTriangle) + triangleCount <= indices.size());
    assert(size_t(vertexBase) + vertexCount <= positions.size());
    primitives.push_back(Primitive{firstTriangle, triangleCount, material, vertexBase, vertexCount});
    return uint32_t(primitives.size() - 1);
}

uint32_t Scene::addInstance(const glm::mat4& transform, uint32_t primitive)
{
    assert(primitive < primitives.size());
    Instance instance = {};
    setTransform(instance, transform);
    instance.primitive = primitive;
    instance.material = primitives[primitive].material;
    instance.lightBase = noLight;
    instances.push_back(instance);
    return uint32_t(instances.size() - 1);
}

void Scene::finalize()
{
    buildLights();
    computeBounds();
}

void Scene::buildLights()
{
    lights.clear();
    triangleLights.clear();
    lightCdf.clear();

    // Power of each emitted light, parallel to `lights`, used below to sample lights by brightness.
    std::vector<float> weights;
    for (Instance& instance : instances)
    {
        const Primitive&    primitive = primitives[instance.primitive];
        const GltfMaterial& material = materials[primitive.material];
        const vec3          emission = vec3(material.emissiveFactor);
        if (glm::dot(emission, emission) <= 0.0f)
        {
            instance.lightBase = noLight;
            continue;
        }

        // A texture can leave most of a primitive black, and every extra light dilutes next-event
        // estimation. Keep a triangle only when its texture is not black where it is sampled; a
        // pruned triangle still emits on a direct hit, it is just not in the light list.
        const bool            textured = material.emissiveTexture >= 0 && material.emissiveTexture < int(textures.size()) && !samplers.empty();
        const TextureData*    texture = textured ? &textures[material.emissiveTexture] : nullptr;
        const TextureSampler* sampler = textured ? &samplers[size_t(material.emissiveSampler) % samplers.size()] : nullptr;

        // Reserve this instance's window in the remap table before appending any entry.
        const uint32_t windowBase = uint32_t(triangleLights.size());
        for (uint32_t k = 0; k < primitive.triangleCount; ++k)
        {
            const glm::uvec3& triangle = indices[primitive.firstTriangle + k];
            const vec2        uv0 = emissiveUvs[primitive.vertexBase + triangle.x];
            const vec2        uv1 = emissiveUvs[primitive.vertexBase + triangle.y];
            const vec2        uv2 = emissiveUvs[primitive.vertexBase + triangle.z];
            vec3              emitted = emission;
            if (texture != nullptr)
            {
                emitted = sampleEmissive(*texture, *sampler, (uv0 + uv1 + uv2) / 3.0f) * emission;
                if (glm::dot(emitted, emitted) <= 0.0f)
                {
                    triangleLights.push_back(-1);
                    continue;
                }
            }

            const vec3 p0 = transformPoint(instance.transformRows, positions[primitive.vertexBase + triangle.x]);
            const vec3 p1 = transformPoint(instance.transformRows, positions[primitive.vertexBase + triangle.y]);
            const vec3 p2 = transformPoint(instance.transformRows, positions[primitive.vertexBase + triangle.z]);

            Light light = {};
            light.p0 = vec4(p0, 0.0f);
            light.p1 = vec4(p1, 0.0f);
            light.p2 = vec4(p2, 0.0f);
            light.info = uvec4(primitive.material, 0, 0, 0);
            light.uv01 = vec4(uv0, uv1);
            light.uv2 = vec4(uv2, 0.0f, 0.0f);
            triangleLights.push_back(int32_t(lights.size()));
            lights.push_back(light);

            // Emitted radiance times the world-space area, so a light's chance of being sampled follows
            // the light it actually casts.
            const vec3  luminance(0.2126f, 0.7152f, 0.0722f);
            const float area = 0.5f * glm::length(glm::cross(p1 - p0, p2 - p0));
            weights.push_back(glm::dot(emitted, luminance) * area);
        }

        // Mark the window addressable only when something in it emitted; a fully black emitter stays
        // unlisted and its -1 entries are never read.
        const bool anyEmitted = std::any_of(
            triangleLights.begin() + windowBase,
            triangleLights.end(),
            [](int32_t index) { return index >= 0; }
        );
        instance.lightBase = anyEmitted ? windowBase : noLight;
    }

    // Analytic lights follow the triangle prefix, leaving triangle-to-light indices unchanged.
    for (const PointLight& point : pointLights)
    {
        Light light = {};
        light.p0 = vec4(point.position, 0);
        light.p1 = vec4(point.intensity, 0);
        light.info = uvec4(0, lightTypePoint, 0, 0);
        lights.push_back(light);
        // Triangle weights omit a common pi factor. A point's flux is 4*pi*intensity.
        weights.push_back(4.0f * glm::dot(point.intensity, vec3(0.2126f, 0.7152f, 0.0722f)));
    }

    // Prefix sum of the powers. The shader samples a uniform value in [0, lightCdf.back()) and reads
    // the light off the interval it lands in; the trailing entry is the total.
    lightCdf.reserve(lights.size() + 1);
    lightCdf.push_back(0.0f);
    for (float weight : weights)
        lightCdf.push_back(lightCdf.back() + weight);
}

void Scene::computeBounds()
{
    lower = vec3(std::numeric_limits<float>::max());
    upper = -lower;
    for (const Instance& instance : instances)
    {
        const Primitive& primitive = primitives[instance.primitive];
        for (uint32_t k = 0; k < primitive.triangleCount; ++k)
        {
            const glm::uvec3& triangle = indices[primitive.firstTriangle + k];
            for (uint32_t corner : {triangle.x, triangle.y, triangle.z})
            {
                const vec3 world =
                    transformPoint(instance.transformRows, positions[primitive.vertexBase + corner]);
                assert(std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z));
                lower = min(lower, world);
                upper = max(upper, world);
            }
        }
    }
}

//----------------------------------------------------------------------------
// glTF loading
//----------------------------------------------------------------------------

bool Scene::loadGltf(const std::filesystem::path& path)
{
    return importer.load(*this, path);
}

bool Scene::parse()
{
    return importer.derive(*this, currentScene);
}

bool Scene::setCurrentScene(int index)
{
    return importer.derive(*this, index);
}

void Scene::resetDerived()
{
    positions.clear();
    normals.clear();
    baseColorUvs.clear();
    metallicRoughnessUvs.clear();
    normalUvs.clear();
    occlusionUvs.clear();
    emissiveUvs.clear();
    transmissionUvs.clear();
    indices.clear();
    primitives.clear();
    instances.clear();
    materials.clear();
    pointLights.clear();
    lights.clear();
    triangleLights.clear();
    lightCdf.clear();
    textures.clear();
    samplers.clear();

    lower = upper = vec3(0.0f);
    eye = vec3(0.0f, 1.0f, 3.6f);
    target = vec3(0.0f, 1.0f, 0.0f);
    up = vec3(0.0f, 1.0f, 0.0f);
    environment = vec3(0.0f);
    fov = 40.0f;

    errors.clear();
    warnings.clear();
    currentScene = -1;
    name.clear();
}

void addStudio(Scene& scene)
{
    vec3 center = (scene.lower + scene.upper) * 0.5f;

    float r = length(scene.upper - scene.lower) * 0.5f;
    float y = scene.lower.y - r * 0.01f;

    auto ground = scene.addMaterial(vec3(0.55f));
    scene.addQuad(center + vec3(-4 * r, y - center.y, 4 * r),
                  center + vec3(4 * r, y - center.y, 4 * r),
                  center + vec3(4 * r, y - center.y, -4 * r),
                  center + vec3(-4 * r, y - center.y, -4 * r), ground);
    const vec3 p = center + vec3(-r, 3 * r, r);
    // Match the former quad's far-field intensity along its normal: radiance * area.
    // Scaling by r^2 keeps illumination comparable across model sizes.
    scene.pointLights.push_back({p, vec3(36.0f * r * r)});
    scene.environment = vec3(0.06f);
    scene.finalize();
}
} // namespace snr