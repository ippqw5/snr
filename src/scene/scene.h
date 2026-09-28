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

// Float4 field gives the host and Slang identical structured-buffer layout.
struct Triangle
{
    vec4  p0, p1, p2;
    vec4  n0, n1, n2;
    vec4  uv01, uv2;
    vec4  mrUV01, mrUV2;
    uvec4 info; // Material ID, emissive triangle ID, reserved, reserved
};

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
static_assert(sizeof(Triangle) == 176);
static_assert(sizeof(Material) == 112);

struct Scene
{
    std::vector<Triangle> triangles;
    std::vector<Material> materials;
    std::vector<vec4>     texels;
    std::vector<uint32_t> lights;

    vec3                     lower{0.0f}, upper{0.0f};
    vec3                     eye{0, 1, 3.6f}, target{0, 1, 0}, up{0, 1, 0};
    vec3                     environment{0.0f};
    float                    fov = 40.0f;
    std::vector<std::string> warnings;

    uint32_t addMaterial(glm::vec3 color, glm::vec3 emission = glm::vec3(0.0f));
    void     addTriangle(glm::vec3 a, glm::vec3 b, glm::vec3 c, uint32_t material);
    void     addQuad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material);
    void     finalize();
};

Scene makeCornellBox();
Scene makeFurnaceScene();
void  addStudio(Scene& scene);
} // namespace snr
