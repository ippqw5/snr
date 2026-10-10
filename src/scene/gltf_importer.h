#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf
{
class Model;
struct Mesh;
struct Primitive;
struct Texture;
struct TextureInfo;
struct Material;
} // namespace tinygltf

namespace snr
{
struct Scene;

// Loads a glTF model and derives the renderer data Scene consumes from it.
//
// Neither entry point throws: a fatal problem returns false and is described by scene.errors, while
// recoverable ones are appended to scene.warnings and the offending primitive is skipped. The
// loaded model is kept so that derive() can run again.
//
// Scene owns one of these, so the loaded model lives exactly as long as the scene does.
class GltfImporter
{
public:
    // Loads `path` into a new model and derives `scene` from it.
    bool load(Scene& scene, const std::filesystem::path& path);

    // Derives `scene` from the kept model, building glTF scene `sceneIndex`. Returns false and
    // leaves `scene` untouched when no model is loaded or the index is out of range.
    bool derive(Scene& scene, int sceneIndex);

private:
    // Which TEXCOORD set a texture samples, and the UV transform baked into the vertices.
    struct TextureCoordinates
    {
        int       set = -1;
        glm::mat3 transform{1.0f};

        glm::vec2 apply(glm::vec2 uv) const;
    };

    // Base colour and metallic-roughness each pick their own UV set and transform.
    struct MaterialBindings
    {
        TextureCoordinates baseColor, metallicRoughness;
    };

    struct TexelBlock
    {
        size_t offset = 0;
        int    width = 0;
        int    height = 0;
    };

    // A strided view over the bytes an accessor reads.
    struct AccessorView
    {
        std::span<const unsigned char> bytes;
        size_t                         stride = 0;

        const unsigned char* operator[](size_t index) const
        {
            return bytes.data() + index * stride;
        }
    };

    // File loading
    bool loadFile(const std::filesystem::path& path);
    bool validateExtensions();
    bool selectScene();

    // Derived data
    bool                  import(int sceneIndex);
    void                  importMaterials();
    void                  importGeometry();
    void                  visitNode(int nodeIndex, const glm::mat4& parent, int depth);
    void                  emitMesh(const tinygltf::Mesh& mesh, int meshIndex, const glm::mat4& world);
    uint32_t              primitiveFor(int meshIndex, int primitiveIndex, const tinygltf::Primitive& primitive);
    uint32_t              buildPrimitive(const tinygltf::Primitive& primitive);
    bool                  readUv(const tinygltf::Primitive& primitive, const TextureCoordinates& coordinates, size_t vertexCount, std::vector<glm::vec2>& values);
    std::vector<uint32_t> trianglesOf(const tinygltf::Primitive& primitive, const std::vector<uint32_t>& indices);
    bool                  frameCamera();

    // Materials and textures
    TextureCoordinates textureCoordinates(const tinygltf::TextureInfo& info);
    void               bindTexture(
        const tinygltf::TextureInfo& info,
        bool                         srgb,
        TextureCoordinates&          coordinates,
        glm::ivec4&                  texture,
        glm::uvec4&                  sampler
    );
    bool texelsFor(int imageIndex, bool srgb, TexelBlock& block);
    int  imageOf(const tinygltf::Texture& texture);
    void reportUnsupportedMaterial(const tinygltf::Material& material);

    // Accessors
    bool readAttribute(int accessorIndex, int type, std::vector<float>& values);
    bool readIndices(int accessorIndex, size_t vertexCount, std::vector<uint32_t>& indices);
    bool viewOf(int bufferView, size_t offset, size_t elementSize, size_t count, AccessorView& view);

    // Diagnostics
    void fail(std::string message);
    void warn(std::string message);
    void ignore(std::string feature);
    template <typename T>
    const T* check(const std::vector<T>& values, int index, const char* description);

    // The scene being filled. Every entry point rebinds it from its own argument before anything
    // else reads it, so it always agrees with the scene the caller asked for -- even for a copy of
    // a scene that was loaded earlier.
    Scene* m_scene = nullptr;

    // The glTF model the importer was loaded from, kept so that derive() can run again.
    std::shared_ptr<tinygltf::Model> m_model;

    std::vector<std::string>                 m_ignored;
    std::vector<MaterialBindings>            m_bindings;
    std::vector<uint32_t>                    m_materialIds;
    std::unordered_map<uint64_t, TexelBlock> m_texelBlocks;
    std::unordered_map<uint64_t, uint32_t>   m_primitiveCache;
    MaterialBindings                         m_defaultBindings;
    uint32_t                                 m_defaultMaterial = 0;
    size_t                                   m_degenerateTriangles = 0;
};
} // namespace snr
