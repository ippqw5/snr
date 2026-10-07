#include "./gltf_loader.h"
#include "./document.h"
#include "./geometry.h"
#include "./materials.h"

namespace snr
{
Scene loadGltf(const std::filesystem::path& path)
{
    Scene      scene;
    const auto model = readDocument(path, scene.warnings);
    const auto materials = importMaterials(model, scene);

    importGeomtry(model, materials, scene);
    scene.finalize();
    scene.target = (scene.lower + scene.upper) * 0.5f;
    float radius = glm::length(scene.upper - scene.lower) * 0.5f;
    scene.eye = scene.target + glm::normalize(glm::vec3(1.3f, 0.8f, 1.8f)) * radius * 3.6f;
    scene.environment = vec3(0.35f);
    return scene;
}
} // namespace snr