#include "./scene.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace snr
{
static void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

uint32_t Scene::addMaterial(vec3 color, vec3 emission)
{
    Material material;
    material.baseColor = vec4(color, 1);
    material.emission = vec4(emission, 0);
    materials.push_back(material);
    return uint32_t(materials.size() - 1);
}

void Scene::addTriangle(vec3 a, vec3 b, vec3 c, uint32_t material)
{
    vec3 normal = glm::cross(b - a, c - a);
    normal = glm::normalize(normal);
    Triangle triangle = {};
    triangle.p0 = vec4(a, 0);
    triangle.p1 = vec4(b, 0);
    triangle.p2 = vec4(c, 0);
    triangle.n0 = triangle.n1 = triangle.n2 = vec4(normal, 0);
    triangle.info = uvec4(material, UINT32_MAX, 0, 0);
    triangles.push_back(triangle);
}

void Scene::addQuad(vec3 a, vec3 b, vec3 c, vec3 d, uint32_t material)
{
    addTriangle(a, b, c, material);
    addTriangle(a, c, d, material);
}

void Scene::finalize()
{
    require(!triangles.empty(), "Scene has no non-degenerate triangles");
    require(triangles.size() < UINT32_MAX / 3, "Scene exceeds 32-bit geometry limits");
    lower = vec3(std::numeric_limits<float>::max());
    upper = -lower;
    lights.clear();
    for (size_t i = 0; i < triangles.size(); ++i)
    {
        auto& t = triangles[i];
        for (vec3 p : {vec3(t.p0), vec3(t.p1), vec3(t.p2)})
        {
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                    "Non-finite vertex position");
            lower = min(lower, p);
            upper = max(upper, p);
        }
        t.info.y = UINT32_MAX;
        const auto emission = vec3(materials.at(t.info.x).emission);
        if (dot(emission, emission) > 0)
        {
            t.info.y = uint32_t(lights.size());
            lights.push_back(uint32_t(i));
        }
    }
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
