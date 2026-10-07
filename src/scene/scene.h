#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace snr
{
using glm::ivec4;
using glm::uvec4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

// Float4 fields give the host and Slang identical structured-buffer layout.
struct Triangle
{
    vec4 p0, p1, p2;
    vec4 n0, n1, n2;
    vec4 uv01, uv2;
    vec4 mrUV01, mrUV2;
};
static_assert(sizeof(Triangle) == 160);

// A unique primitive: one triangle range in object space with a single material.
struct Primitive
{
    uint32_t firstTriangle;
    uint32_t triangleCount;
    uint32_t material;
    uint32_t reserved;
};
static_assert(sizeof(Primitive) == 16);

// Places one primitive in the world. The matrix rows are spelled out so the host and Slang
// agree on layout without relying on either side's matrix convention.
struct Instance
{
    vec4     transformRows[3]; // Object-to-world affine
    vec4     normalRows[3];    // Inverse transpose of the object-to-world 3x3
    uint32_t primitive;
    uint32_t material;
    uint32_t lightBase; // First world-space light of this instance, or noLight
    uint32_t flags;
};
static_assert(sizeof(Instance) == 112);

// A world-space emissive triangle, used for next-event estimation.
struct Light
{
    vec4  p0, p1, p2;
    uvec4 info; // Material index, reserved, reserved, reserved
};
static_assert(sizeof(Light) == 64);

constexpr uint32_t noLight = UINT32_MAX;

struct Material
{
    vec4  baseColor{0.75f, 0.75f, 0.75f, 1.0f};
    vec4  emission{0.0f};
    ivec4 texture{-1, 0, 0, 0};         // Pixel Offset, width, height, filter
    uvec4 settings{10497, 10497, 1, 0}; // Wrap S/T, double sided, PBR enabled
    vec4  pbr{0, 1, 0, 0};              // Metallic, Roughness, reserved, reserved
    ivec4 mrTexture{-1, 0, 0, 0};
    uvec4 mrSettings{10497, 10497, 0, 0};
};
static_assert(sizeof(Material) == 112);

struct Scene
{
    // Unique geometry in object space, and the instances that place it in the world.
    std::vector<Triangle>  triangles;
    std::vector<Primitive> primitives;
    std::vector<Instance>  instances;

    std::vector<Material> materials;
    std::vector<Light>    lights; // World space, addressed through Instance::lightBase
    std::vector<vec4>     texels;

    // Staging for the built-in scene builders. finalize() folds it into the arrays above.
    std::vector<Triangle> looseTriangles;
    std::vector<uint32_t> looseMaterials;

    vec3  lower{0.0f}, upper{0.0f};
    vec3  eye{0, 1, 3.6f}, target{0, 1, 0}, up{0, 1, 0};
    vec3  environment{0.0f};
    float fov = 40.0f;

    uint32_t addMaterial(glm::vec3 color, glm::vec3 emission = glm::vec3(0.0f));
    void     addTriangle(glm::vec3 a, glm::vec3 b, glm::vec3 c, uint32_t material);
    void     addQuad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material);
    // Registers a triangle range in `triangles` as a primitive and returns its index.
    uint32_t addPrimitive(uint32_t firstTriangle, uint32_t triangleCount, uint32_t material);
    // Adds an instance of `primitive` placed by `transform` and returns its index.
    uint32_t addInstance(const glm::mat4& transform, uint32_t primitive);

    void finalize();

private:
    void buildLights();
    void computeBounds();
};

Scene makeCornellBox();
Scene makeFurnaceScene();
void  addStudio(Scene& scene);
} // namespace snr