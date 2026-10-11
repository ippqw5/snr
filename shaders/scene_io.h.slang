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

// Vertex attributes are not a struct: positions/normals/UVs/indices are each bound as their own
// typed array. Each primitive owns a contiguous window of the vertex arrays, and its indices are
// local to that window, so the arrays keep the tight glTF element sizes (float3 is 12 bytes, float2
// is 8) instead of being padded to float4 -- the padding was only ever needed to make a float3
// struct member unambiguous.

// A unique primitive: one triangle range in object space with a single material, drawing on a
// contiguous window [vertexBase, vertexBase + vertexCount) of the shared vertex arrays.
// firstTriangle is an ordinal in the global index buffer, so its first index is firstTriangle * 3,
// and each index there is an offset from vertexBase. The window exists because the acceleration
// structure build derives a primitive's triangle count from its vertex and index counts, so a
// primitive must not reference vertices beyond its own triangles.
struct Primitive
{
    uint firstTriangle;
    uint triangleCount;
    uint material;
    uint vertexBase;
    uint vertexCount;
};
SNR_IO_STATIC_ASSERT(sizeof(Primitive) == 20);

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

// A world-space emissive triangle, used for next-event estimation. The UVs let light sampling read a
// spatially varying emissive texture at the sampled point instead of assuming a flat factor. They are
// packed into float4 slots so the layout does not depend on how either side aligns a float2.
struct Light
{
    float4 p0, p1, p2;
    uint4  info; // Material index, reserved, reserved, reserved
    float4 uv01; // Emissive texture UV of p0 in .xy and of p1 in .zw
    float4 uv2;  // Emissive texture UV of p2 in .xy
};
SNR_IO_STATIC_ASSERT(sizeof(Light) == 96);

// Sentinel for "this instance emits no light".
static const uint noLight = 0xffffffffu;

// glTF alpha modes, stored in GltfMaterial::flags.z. OPAQUE ignores the base color alpha, MASK
// discards a fragment whose alpha is below the cutoff, and BLEND keeps it for the sorted transparent
// pass.
static const uint alphaModeOpaque = 0;
static const uint alphaModeMask = 1;
static const uint alphaModeBlend = 2;

// Upper bounds on the bindless texture and sampler arrays the shader declares. The host sizes its
// descriptor pool from these and refuses a model that needs more, so both sides stay in step.
static const int SNR_MAX_TEXTURES = 1024;
static const int SNR_MAX_SAMPLERS = 32;

// The material record the shader reads from its DDA, named for the glTF material it mirrors. Each
// texture slot holds an index into the shader's images[] array (-1 when the slot is empty) and an
// index into samplers[], so the wrap mode and filtering live in a real sampler instead of inlined
// in the record.
//
// This is deliberately not what the BSDF sees. materialAt() turns one of these into a
// SurfaceMaterial once the hit's UVs are known; see shaders/bsdf.slang.
struct GltfMaterial
{
    float4 pbrBaseColorFactor = float4(0.75f, 0.75f, 0.75f, 1.0f);      // pbrMetallicRoughness.baseColorFactor
    float4 emissiveFactor = float4(0.0f);                               // emissiveFactor, scaled by emissiveStrength
    float4 pbrMetallicRoughnessFactor = float4(0.0f, 1.0f, 0.0f, 0.0f); // Metallic, Roughness, alphaCutoff, reserved
    uint4  flags = uint4(1, 0, 0, 0);                                   // Double sided, PBR enabled, alpha mode, reserved
    int    pbrBaseColorTexture = -1;                                    // Index into images[]
    int    pbrBaseColorSampler = 0;                                     // Index into samplers[]
    int    pbrMetallicRoughnessTexture = -1;
    int    pbrMetallicRoughnessSampler = 0;
    int    normalTexture = -1;
    int    normalSampler = 0;
    int    occlusionTexture = -1;
    int    occlusionSampler = 0;
    int    emissiveTexture = -1;
    int    emissiveSampler = 0;
    float  normalScale = 1.0f;       // normalTexture.scale
    float  occlusionStrength = 1.0f; // occlusionTexture.strength
};
SNR_IO_STATIC_ASSERT(sizeof(GltfMaterial) == 112);

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

// Channels the raster preview can draw instead of its shaded colour, held in RasterParams::counts.y.
// Shaded is the normal preview; the rest are debug views of one attachment or one material input.
static const uint rasterDebugShaded = 0;
static const uint rasterDebugDepth = 1;
static const uint rasterDebugObjectId = 2;
static const uint rasterDebugBaseColor = 3;
static const uint rasterDebugNormal = 4;

// Per-frame constants for the raster preview. The view-projection is stored as its four rows rather
// than as a matrix, so the host and the shader cannot disagree on the storage order: clip is
// (dot(rowX.xyz, p) + rowX.w, dot(rowY.xyz, p) + rowY.w, ...) with p in world space. Each draw
// carries its own instance index (DrawArguments::startInstanceLocation) instead of a constant, so
// nothing in here is per-draw.
struct RasterParams
{
    float4 viewProjRows[4]; // Rows of the world-to-clip matrix
    float4 eye;             // Eye position, w holds tan(fov / 2)
    float4 environment;     // Constant environment radiance; w is the eye-to-target distance, the
                            // reference the depth debug view normalises by
    uint4  counts;          // Number of lights, the debug view (rasterDebug*), rest reserved
};
SNR_IO_STATIC_ASSERT(sizeof(RasterParams) == 112);

// Per-frame constants for the windowed present pass, which tone maps the film straight into a
// swapchain image. `config.y` is 1 when that image's format is sRGB, so the shader writes linear
// values and leaves the encode to the hardware instead of double-encoding.
struct PresentParams
{
    uint4  image;  // Width and height of the target, rest reserved
    float4 config; // Exposure in stops, sRGB flag, rest reserved
};
SNR_IO_STATIC_ASSERT(sizeof(PresentParams) == 32);

SNR_IO_END()

#endif // SNR_SCENE_IO_H
