#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct Triangle
{
    glm::vec4  p0, p1, p2;
    glm::vec4  n0, n1, n2;
    glm::vec4  uv01, uv2;
    glm::uvec4 info; // Material ID, emissive triangle ID, reserved, reserved
};

struct Material
{
    glm::vec4  baseColor{0.75f, 0.75f, 0.75f, 1.0f};
    glm::vec4  emission{0.0f};
    glm::ivec4 texture{-1, 0, 0, 0};         // Pixel Offset, width, height, filter
    glm::uvec4 settings{10497, 10497, 1, 0}; // Wrap S/T, double sided, reserved
};
static_assert(sizeof(Triangle) == 144);
static_assert(sizeof(Material) == 64);

struct Scene
{
    std::vector<Triangle>  triangles;
    std::vector<Material>  materials;
    std::vector<glm::vec4> texels;
    std::vector<uint32_t>  lights;

    glm::vec3                lower{0.0f}, upper{0.0f};
    glm::vec3                eye{0, 1, 3.6f}, target{0, 1, 0}, up{0, 1, 0};
    glm::vec3                environment{0.0f};
    float                    fov = 40.0f;
    std::vector<std::string> warnings;

    uint32_t addMaterial(glm::vec3 color, glm::vec3 emission = glm::vec3(0.0f));
    void     addTriangle(glm::vec3 a, glm::vec3 b, glm::vec3 c, uint32_t material);
    void     addQuad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material);
    void     finalize();
};

Scene makeCornellBox();
Scene makeFurnaceScene();
Scene loadGltf(const std::filesystem::path& path);
void  addStudio(Scene& scene);
