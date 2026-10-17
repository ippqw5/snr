import bsdf;

// The host/device structs live in a file the C++ side includes too, so their layouts cannot drift.
#include "scene_io.h.slang"
// Geometry and material evaluation, shared with the raster preview. The header reads the arrays
// declared below and takes the bindless heaps as parameters.
#include "material_eval.h.slang"

// A committed hit, resolved to the instance that produced it.
struct Hit
{
    float t;
    uint instance;
    uint primitive;
    float2 bary;
};

RaytracingAccelerationStructure sceneBVH;
StructuredBuffer<float3> positions;
StructuredBuffer<float3> normals;
StructuredBuffer<float2> baseColorUvs;
StructuredBuffer<float2> metallicRoughnessUvs;
StructuredBuffer<float2> normalUvs;
StructuredBuffer<float2> occlusionUvs;
StructuredBuffer<float2> emissiveUvs;
StructuredBuffer<float2> transmissionUvs;
StructuredBuffer<uint3> indices;
StructuredBuffer<Primitive> primitives;
StructuredBuffer<Instance> instances;
StructuredBuffer<GltfMaterial> materials;
StructuredBuffer<Light> lights;
// Per instance, one entry per triangle of its primitive: the light it maps to, or -1 when the triangle
// is not in the light list. Lets a textured emitter keep only the triangles its texture makes bright.
StructuredBuffer<int> triangleLights;
// Cumulative power of the lights, one entry per light plus a leading zero and a trailing total. Light
// sampling reads it so bright lights are picked more often than dim ones.
StructuredBuffer<float> lightCdf;
// Bindless texture and sampler heaps. The .Handle element is a 64-bit index into slang-rhi's
// bindless descriptor set, so a material names a texture and a sampler by slot and the texture unit
// does the wrapping, filtering and mip selection; see sampleTexture() in material_eval.h.slang.
uniform Texture2D<float4>.Handle images[SNR_MAX_TEXTURES];
uniform SamplerState.Handle samplers[SNR_MAX_SAMPLERS];
ConstantBuffer<Params> params;

// Index of the world-space light a hit emitted from, or -1 when it is not an emitter.
int lightIndexOf(Instance instance, uint localTriangle)
{
    return instance.lightBase == 0xffffffffu ? -1 : triangleLights[instance.lightBase + localTriangle];
}

bool traceScene(float3 origin, float3 direction, float tMax, bool anyHit, out Hit hit)
{
    hit = {};
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneBVH, anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, 0xff, ray);

    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            Instance instance = instances[query.CandidateInstanceID()];
            GltfMaterial material = materials[instance.material];
            Triangle triangle = triangleAt(instance, query.CandidatePrimitiveIndex());

            if (acceptSurfaceHit(instance, triangle, material, query.CandidateTriangleBarycentrics(), direction))
                query.CommitNonOpaqueTriangleHit();
        }
    }

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;

    hit.t = query.CommittedRayT();
    hit.instance = query.CommittedInstanceID();
    hit.primitive = query.CommittedPrimitiveIndex();
    hit.bary = query.CommittedTriangleBarycentrics();
    return true;
}
