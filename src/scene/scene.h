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
using glm::uvec3;
using glm::uvec4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

// One image uploaded as a hardware texture. `levels` holds the full mip chain, level 0 first, each
// tightly packed RGBA8. `srgb` picks the sRGB texture format, so the sampler returns linear values
// for colour textures while data textures stay linear.
struct TextureData
{
    int                               width = 0;
    int                               height = 0;
    bool                              srgb = false;
    std::vector<std::vector<uint8_t>> levels;
};

enum class TextureWrap
{
    Repeat,
    ClampToEdge,
    MirrorRepeat,
};

// A distinct sampler state. Materials name one by index so identical samplers share a single object.
struct TextureSampler
{
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    bool        nearest = false;

    bool operator==(const TextureSampler&) const = default;
};

// An analytic, isotropic emitter. Intensity is radiant power per unit solid angle (RGB).
struct PointLight
{
    glm::vec3 position;
    glm::vec3 intensity;
};

struct Scene
{
    // Geometry in object space, split into the typed arrays the shader indexes: one entry per vertex
    // for the attributes, and three vertex ids per triangle in `indices`. Each primitive owns a
    // contiguous window of the vertex arrays and stores the offsets of its own vertices, so a vertex
    // shared by several triangles inside one primitive is stored once.
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals; // Zero when the source had no usable normal; the shader then
                                    // falls back to the geometric normal.
    // One UV array per material texture slot, because every slot may pick its own TEXCOORD set and its
    // own KHR_texture_transform; the transform is baked into these values.
    std::vector<glm::vec2>  baseColorUvs;
    std::vector<glm::vec2>  metallicRoughnessUvs;
    std::vector<glm::vec2>  normalUvs;
    std::vector<glm::vec2>  occlusionUvs;
    std::vector<glm::vec2>  emissiveUvs;
    std::vector<glm::vec2>  transmissionUvs;
    std::vector<glm::uvec3> indices; // Vertex ids local to the owning primitive's window

    // Unique geometry in object space, and the instances that place it in the world.
    std::vector<Primitive> primitives;
    std::vector<Instance>  instances;

    std::vector<GltfMaterial> materials;
    std::vector<PointLight>   pointLights; // Retained analytic emitters, rebuilt into lights by finalize()
    std::vector<Light>        lights;      // World-space triangles first, then point lights
    // Maps an instance's local triangle id to its light in `lights`, or -1 when that triangle emits
    // nothing. An instance owns a contiguous run of entries starting at its lightBase, so a hit's
    // light is triangleLights[lightBase + localTriangle]. Emitters whose texture is black are left out
    // of `lights` so they do not dilute next-event estimation; those hits just carry no light index.
    std::vector<int32_t> triangleLights;
    // Cumulative power of the lights, one entry per light plus a trailing total. Light sampling picks
    // by brightness, so a small emitter does not take as many samples as a large bright one.
    std::vector<float>          lightCdf;
    std::vector<TextureData>    textures;
    std::vector<TextureSampler> samplers;

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
    std::string              name; // Selected glTF scene name, when supplied by the asset.

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
    // Appends a quad as two triangles sharing four vertices, and registers it as one primitive.
    void addQuad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material);
    // Registers a triangle range in `indices`, drawing on the vertex window that starts at
    // `vertexBase`, as a primitive and returns its index.
    uint32_t addPrimitive(
        uint32_t firstTriangle,
        uint32_t triangleCount,
        uint32_t vertexBase,
        uint32_t vertexCount,
        uint32_t material
    );
    // Adds an instance of `primitive` placed by `transform` and returns its index.
    uint32_t addInstance(const glm::mat4& transform, uint32_t primitive);

    // Derives the world-space lights and the scene bounds. Call once after all geometry and
    // instances are in place.
    void finalize();

private:
    void buildLights();
    void computeBounds();
};

void addStudio(Scene& scene);
} // namespace snr