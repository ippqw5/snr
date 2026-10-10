#pragma once

#include "shaders/scene_io.h.slang"

#include "./gltf_importer.h"

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

struct Scene
{
    // Unique geometry in object space, and the instances that place it in the world.
    std::vector<Triangle>  triangles;
    std::vector<Primitive> primitives;
    std::vector<Instance>  instances;

    std::vector<GltfMaterial> materials;
    std::vector<Light>        lights; // World space, addressed through Instance::lightBase
    std::vector<vec4>         texels;

    // Staging for the built-in scene builders. finalize() folds it into the arrays above.
    std::vector<Triangle> looseTriangles;
    std::vector<uint32_t> looseMaterials;

    vec3  lower{0.0f}, upper{0.0f};
    vec3  eye{0, 1, 3.6f}, target{0, 1, 0}, up{0, 1, 0};
    vec3  environment{0.0f};
    float fov = 40.0f;

    // glTF provenance, left empty by the built-in scenes. The importer owns the loaded model, so
    // the scene can be re-derived from it at any time.
    GltfImporter             importer;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    int                      currentScene = -1;

    // Loads a .gltf/.glb and derives everything above. Returns false when nothing usable could be
    // imported, in which case errors describes why; warnings always describe recoverable problems.
    bool loadGltf(const std::filesystem::path& path);
    // Re-derives everything above from the model the importer holds.
    bool parse();
    // Selects which glTF scene is derived, then re-parses.
    bool setCurrentScene(int index);
    // Clears the derived data, the diagnostics and the scene selection, leaving the importer's
    // loaded model alone. Called by the importer before it derives.
    void resetDerived();

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