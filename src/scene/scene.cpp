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
} // namespace

uint32_t Scene::addMaterial(vec3 color, vec3 emission)
{
    GltfMaterial material;
    material.pbrBaseColorFactor = vec4(color, 1);
    material.emissiveFactor = vec4(emission, 0);
    materials.push_back(material);
    return uint32_t(materials.size() - 1);
}

void Scene::addTriangle(vec3 a, vec3 b, vec3 c, uint32_t material)
{
    const vec3 cross = glm::cross(b - a, c - a);
    assert(glm::dot(cross, cross) > 0.0f);

    Triangle triangle = {};
    triangle.p0 = vec4(a, 0);
    triangle.p1 = vec4(b, 0);
    triangle.p2 = vec4(c, 0);
    triangle.n0 = triangle.n1 = triangle.n2 = vec4(glm::normalize(cross), 0);
    looseTriangles.push_back(triangle);
    looseMaterials.push_back(material);
}

void Scene::addQuad(vec3 a, vec3 b, vec3 c, vec3 d, uint32_t material)
{
    addTriangle(a, b, c, material);
    addTriangle(a, c, d, material);
}

uint32_t Scene::addPrimitive(uint32_t firstTriangle, uint32_t triangleCount, uint32_t material)
{
    assert(triangleCount > 0);
    assert(size_t(firstTriangle) + triangleCount <= triangles.size());
    primitives.push_back(Primitive{firstTriangle, triangleCount, material, 0});
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
    if (!looseTriangles.empty())
    {
        assert(looseTriangles.size() == looseMaterials.size());

        // Group the authored triangles by material, in order of first use, so each primitive
        // ends up with a single material.
        std::vector<uint32_t> used;
        for (uint32_t material : looseMaterials)
        {
            if (std::find(used.begin(), used.end(), material) == used.end())
                used.push_back(material);
        }

        for (uint32_t material : used)
        {
            const uint32_t first = uint32_t(triangles.size());
            for (size_t i = 0; i < looseTriangles.size(); ++i)
            {
                if (looseMaterials[i] == material)
                    triangles.push_back(looseTriangles[i]);
            }
            const uint32_t primitive = addPrimitive(first, uint32_t(triangles.size()) - first, material);
            addInstance(glm::mat4(1.0f), primitive);
        }

        looseTriangles.clear();
        looseMaterials.clear();
    }

    buildLights();
    computeBounds();
}

void Scene::buildLights()
{
    lights.clear();
    for (Instance& instance : instances)
    {
        const Primitive& primitive = primitives[instance.primitive];
        const vec3       emission = vec3(materials[primitive.material].emissiveFactor);
        if (glm::dot(emission, emission) <= 0.0f)
        {
            instance.lightBase = noLight;
            continue;
        }

        // Every triangle of an emissive primitive emits, so a hit's light is lightBase + triangle.
        instance.lightBase = uint32_t(lights.size());
        for (uint32_t k = 0; k < primitive.triangleCount; ++k)
        {
            const Triangle& triangle = triangles[primitive.firstTriangle + k];
            Light           light = {};
            light.p0 = vec4(transformPoint(instance.transformRows, vec3(triangle.p0)), 0.0f);
            light.p1 = vec4(transformPoint(instance.transformRows, vec3(triangle.p1)), 0.0f);
            light.p2 = vec4(transformPoint(instance.transformRows, vec3(triangle.p2)), 0.0f);
            light.info = uvec4(primitive.material, 0, 0, 0);
            lights.push_back(light);
        }
    }
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
            const Triangle& triangle = triangles[primitive.firstTriangle + k];
            for (vec3 point : {vec3(triangle.p0), vec3(triangle.p1), vec3(triangle.p2)})
            {
                const vec3 world = transformPoint(instance.transformRows, point);
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
    triangles.clear();
    primitives.clear();
    instances.clear();
    materials.clear();
    lights.clear();
    texels.clear();
    looseTriangles.clear();
    looseMaterials.clear();

    lower = upper = vec3(0.0f);
    eye = vec3(0.0f, 1.0f, 3.6f);
    target = vec3(0.0f, 1.0f, 0.0f);
    up = vec3(0.0f, 1.0f, 0.0f);
    environment = vec3(0.0f);
    fov = 40.0f;

    errors.clear();
    warnings.clear();
    currentScene = -1;
}

static void addBox(Scene& scene, vec3(center), vec3 size, float angle, uint32_t material)
{
    vec3  p[8];
    float c = std::cos(angle), s = std::sin(angle);
    for (int i = 0; i < 8; i++)
    {
        vec3 q = (vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1) - 0.5f) * size;
        p[i] = center + vec3(c * q.x + s * q.z, q.y, -s * q.x + c * q.z);
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
    auto  white = s.addMaterial(vec3(0.73f));
    auto  red = s.addMaterial(vec3(0.63f, 0.065f, 0.05f));
    auto  green = s.addMaterial(vec3(0.14f, 0.45f, 0.091f));
    auto  light = s.addMaterial(vec3(0), vec3(14.0f, 12.8f, 10.5f));
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
    auto  matte = s.addMaterial(vec3(0.6f, 0.4f, 0.2f));
    s.addQuad({-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {-100, 100, 0}, matte);
    s.eye = {0, 0, 2};
    s.target = {0, 0, 0};
    s.environment = vec3(1);
    s.finalize();
    return s;
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
    auto light = scene.addMaterial(vec3(0), vec3(9));
    vec3 p = center + vec3(-r, 3 * r, r);
    scene.addQuad(p + vec3(-r, 0, -r), p + vec3(r, 0, -r),
                  p + vec3(r, 0, r), p + vec3(-r, 0, r), light);
    scene.environment = vec3(0.06f);
    scene.finalize();
}
} // namespace snr