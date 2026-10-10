// Structs shared between the host and the shaders.
//
// Both sides include this one file, so the host/device layouts cannot drift and each struct's size
// is asserted in a single place instead of two. In C++ the structs live in namespace snr and Slang's
// built-in vector types are aliased to their glm equivalents; Slang has no namespaces, so
// SNR_IO_BEGIN() expands to nothing there. Note that Slang defines __SLANG__, not __cplusplus, so
// __cplusplus selects the host branch reliably.

#ifndef SNR_SCENE_IO_H
#define SNR_SCENE_IO_H

#ifdef __cplusplus
#include <cstdint>
#include <glm/glm.hpp>

namespace snr
{
using uint = uint32_t;
using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using int4 = glm::ivec4;
using uint4 = glm::uvec4;
} // namespace snr

#define SNR_IO_BEGIN() namespace snr {
#define SNR_IO_END() }
// Slang's sizeof does not accept a type operand, so the size checks are host-only: the shared
// definition is what actually keeps the layouts equal.
#define SNR_IO_STATIC_ASSERT(condition) static_assert(condition)
#else
#define SNR_IO_BEGIN()
#define SNR_IO_END()
#define SNR_IO_STATIC_ASSERT(condition)
#endif

SNR_IO_BEGIN()

// Float4 fields give the host and Slang identical structured-buffer layout.
// The UV fields hold one coordinate pair per triangle corner: .xy and .zw for corners 0 and 1,
// then .xy for corner 2. Each field is named after the material texture slot it feeds.
struct Triangle
{
    float4 p0, p1, p2;
    float4 n0, n1, n2;
    float4 baseColorUv01, baseColorUv2;
    float4 metallicRoughnessUv01, metallicRoughnessUv2;
};
SNR_IO_STATIC_ASSERT(sizeof(Triangle) == 160);

// A unique primitive: one triangle range in object space with a single material.
struct Primitive
{
    uint firstTriangle;
    uint triangleCount;
    uint material;
    uint reserved;
};
SNR_IO_STATIC_ASSERT(sizeof(Primitive) == 16);

// Places one primitive in the world. The matrix rows are spelled out so the host and Slang agree on
// layout without relying on either side's matrix convention.
struct Instance
{
    float4 transformRows[3]; // Object-to-world affine
    float4 normalRows[3];    // Inverse transpose of the object-to-world 3x3
    uint   primitive;
    uint   material;
    uint   lightBase; // First world-space light of this instance, or noLight
    uint   flags;
};
SNR_IO_STATIC_ASSERT(sizeof(Instance) == 112);

// A world-space emissive triangle, used for next-event estimation.
struct Light
{
    float4 p0, p1, p2;
    uint4  info; // Material index, reserved, reserved, reserved
};
SNR_IO_STATIC_ASSERT(sizeof(Light) == 64);

// Sentinel for "this instance emits no light".
static const uint noLight = 0xffffffffu;

// The material record the shader reads from its DDA, named for the glTF material it mirrors. Both
// textures are already resolved: instead of an index into the glTF texture array they hold a texel
// offset into Scene::texels plus that image's size, and the sampler state glTF keeps in samplers[]
// is inlined next to its texture.
//
// This is deliberately not what the BSDF sees. materialAt() turns one of these into a
// SurfaceMaterial once the hit's UVs are known; see shaders/bsdf.slang.
struct GltfMaterial
{
    float4 pbrBaseColorFactor = float4(0.75f, 0.75f, 0.75f, 1.0f);      // pbrMetallicRoughness.baseColorFactor
    float4 emissiveFactor = float4(0.0f);                               // emissiveFactor, already scaled
    float4 pbrMetallicRoughnessFactor = float4(0.0f, 1.0f, 0.0f, 0.0f); // Metallic, Roughness, reserved
    uint4  flags = uint4(1, 0, 0, 0);                                   // Double sided, PBR enabled, reserved
    int4   pbrBaseColorTexture = int4(-1, 0, 0, 0);                     // Texel offset, width, height, reserved
    uint4  pbrBaseColorSampler = uint4(10497, 10497, 0, 0);             // Wrap S, Wrap T, point filter, reserved
    int4   pbrMetallicRoughnessTexture = int4(-1, 0, 0, 0);
    uint4  pbrMetallicRoughnessSampler = uint4(10497, 10497, 0, 0);
};
SNR_IO_STATIC_ASSERT(sizeof(GltfMaterial) == 128);

// Per-dispatch constants for the path tracer.
struct Params
{
    uint4  image;
    uint4  integrator;
    float4 eye;
    float4 forward;
    float4 right;
    float4 up;
    float4 environment;
};
SNR_IO_STATIC_ASSERT(sizeof(Params) == 112);

SNR_IO_END()

#endif // SNR_SCENE_IO_H
