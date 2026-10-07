#pragma once

#include "../scene/scene.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <tiny_gltf.h>

namespace snr
{
// A loaded glTF file together with the renderer data derived from it.
//
// The tinygltf::Model is the source of truth and stays alive for the lifetime of the object;
// everything the renderer consumes is derived from it by parse(), so the model can be edited and
// re-derived. Loading never throws: fatal problems make load() return false and are described by
// errors(), while recoverable ones are reported through warnings() and the offending primitive is
// skipped.
class GltfScene
{
public:
    // Loads `path` and derives the scene. Returns false when nothing usable could be imported.
    bool load(const std::filesystem::path& path);
    // Rebuilds the derived scene from model(). Call after changing the model.
    void parse();

    bool                   loaded() const;
    const tinygltf::Model& model() const;
    tinygltf::Model&       model();
    const Scene&           scene() const;
    Scene&                 scene();

    int currentScene() const;
    // Selects which glTF scene is derived and re-parses. Returns false for an invalid index.
    bool setCurrentScene(int index);

    const std::vector<std::string>& errors() const;
    const std::vector<std::string>& warnings() const;

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
    bool loadGltfFile(const std::filesystem::path& path);
    bool selectScene();
    bool validateExtensions();

    // Derived data
    void     importMaterials();
    void     importGeometry();
    void     visitNode(int nodeIndex, const glm::mat4& parent, int depth);
    void     emitMesh(const tinygltf::Mesh& mesh, int meshIndex, const glm::mat4& world);
    uint32_t primitiveFor(int meshIndex, int primitiveIndex, const tinygltf::Primitive& primitive);
    uint32_t buildPrimitive(const tinygltf::Primitive& primitive);
    bool     readUv(const tinygltf::Primitive& primitive, const TextureCoordinates& coordinates, size_t vertexCount, std::vector<glm::vec2>& values);
    std::vector<uint32_t> trianglesOf(const tinygltf::Primitive& primitive, const std::vector<uint32_t>& indices);
    bool     frameCamera();

    // Materials and textures
    TextureCoordinates textureCoordinates(const tinygltf::TextureInfo& info);
    ivec4 bindTexture(const tinygltf::TextureInfo& info, bool srgb, TextureCoordinates& coordinates, int& wrapS, int& wrapT);
    bool  texelsFor(int imageIndex, bool srgb, TexelBlock& block);
    int   imageOf(const tinygltf::Texture& texture);
    void  reportUnsupportedMaterial(const tinygltf::Material& material);

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

    tinygltf::Model m_model;
    Scene           m_scene;

    std::vector<std::string>                 m_errors, m_warnings, m_ignored;
    std::vector<MaterialBindings>            m_bindings;
    std::vector<uint32_t>                    m_materialIds;
    std::unordered_map<uint64_t, TexelBlock> m_texelBlocks;
    std::unordered_map<uint64_t, uint32_t>   m_primitiveCache;
    MaterialBindings                         m_defaultBindings;
    uint32_t                                 m_defaultMaterial = 0;
    int                                      m_currentScene = -1;
    size_t                                   m_degenerateTriangles = 0;
    bool                                     m_valid = false;
};
} // namespace snr