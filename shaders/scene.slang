import bsdf;

// The host/device structs live in a file the C++ side includes too, so their layouts cannot drift.
#include "scene_io.h.slang"

// A committed hit, resolved to the instance that produced it.
struct Hit
{
    float t;
    uint instance;
    uint primitive;
    float2 bary;
};

// The vertex attributes of one triangle, gathered from the shared arrays below. Positions, normals
// and UVs are each stored once per vertex, so a vertex shared by several triangles is read once from
// every triangle that uses it. This type is local to the shader: only the id list crosses the
// host/device boundary, so the arrays keep their tight element sizes.
struct Triangle
{
    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float2 baseColorUv0, baseColorUv1, baseColorUv2;
    float2 metallicRoughnessUv0, metallicRoughnessUv1, metallicRoughnessUv2;
    float2 normalUv0, normalUv1, normalUv2;
    float2 occlusionUv0, occlusionUv1, occlusionUv2;
    float2 emissiveUv0, emissiveUv1, emissiveUv2;
};

RaytracingAccelerationStructure sceneBVH;
StructuredBuffer<float3> positions;
StructuredBuffer<float3> normals;
StructuredBuffer<float2> baseColorUvs;
StructuredBuffer<float2> metallicRoughnessUvs;
StructuredBuffer<float2> normalUvs;
StructuredBuffer<float2> occlusionUvs;
StructuredBuffer<float2> emissiveUvs;
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
// does the wrapping, filtering and mip selection; see sampleTexture() below.
uniform Texture2D<float4>.Handle images[SNR_MAX_TEXTURES];
uniform SamplerState.Handle samplers[SNR_MAX_SAMPLERS];
ConstantBuffer<Params> params;

float3 transformPoint(Instance instance, float3 point)
{
    return float3(dot(instance.transformRows[0].xyz, point) + instance.transformRows[0].w,
                  dot(instance.transformRows[1].xyz, point) + instance.transformRows[1].w,
                  dot(instance.transformRows[2].xyz, point) + instance.transformRows[2].w);
}

float3 transformNormal(Instance instance, float3 normal)
{
    return float3(dot(instance.normalRows[0].xyz, normal),
                  dot(instance.normalRows[1].xyz, normal),
                  dot(instance.normalRows[2].xyz, normal));
}

Triangle triangleAt(Instance instance, uint localTriangle)
{
    Primitive primitive = primitives[instance.primitive];
    const uint3 ids = indices[primitive.firstTriangle + localTriangle] + primitive.vertexBase;

    Triangle triangle;
    triangle.p0 = positions[ids.x];
    triangle.p1 = positions[ids.y];
    triangle.p2 = positions[ids.z];
    triangle.n0 = normals[ids.x];
    triangle.n1 = normals[ids.y];
    triangle.n2 = normals[ids.z];
    triangle.baseColorUv0 = baseColorUvs[ids.x];
    triangle.baseColorUv1 = baseColorUvs[ids.y];
    triangle.baseColorUv2 = baseColorUvs[ids.z];
    triangle.metallicRoughnessUv0 = metallicRoughnessUvs[ids.x];
    triangle.metallicRoughnessUv1 = metallicRoughnessUvs[ids.y];
    triangle.metallicRoughnessUv2 = metallicRoughnessUvs[ids.z];
    triangle.normalUv0 = normalUvs[ids.x];
    triangle.normalUv1 = normalUvs[ids.y];
    triangle.normalUv2 = normalUvs[ids.z];
    triangle.occlusionUv0 = occlusionUvs[ids.x];
    triangle.occlusionUv1 = occlusionUvs[ids.y];
    triangle.occlusionUv2 = occlusionUvs[ids.z];
    triangle.emissiveUv0 = emissiveUvs[ids.x];
    triangle.emissiveUv1 = emissiveUvs[ids.y];
    triangle.emissiveUv2 = emissiveUvs[ids.z];
    return triangle;
}

// World-space normal taken from the transformed edges, so mirrored instances still face correctly.
float3 geometricNormal(Instance instance, Triangle triangle)
{
    float3 a = transformPoint(instance, triangle.p0);
    float3 b = transformPoint(instance, triangle.p1);
    float3 c = transformPoint(instance, triangle.p2);
    return normalize(cross(b - a, c - a));
}

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

            if (material.flags.x != 0 || dot(geometricNormal(instance, triangle), direction) < 0)
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

int wrapTexel(int x, int size, uint mode)
{
    if (mode == 33071)
        return clamp(x, 0, size - 1);
    int period = mode == 33648 ? size * 2 : size;
    x = ((x % period) + period) % period;
    return mode == 33648 && x >= size ? period - x - 1 : x;
}

float3 fetchTexel(int4 textureInfo, uint2 wrap, int2 p)
{
    int x = wrapTexel(p.x, textureInfo.y, wrap.x);
    int y = wrapTexel(p.y, textureInfo.z, wrap.y);
    return texels[textureInfo.x + y * textureInfo.y + x].rgb;
}

float wrapCoordinate(float uv, uint mode)
{
    if (mode == 33071)
        return saturate(uv);
    return mode == 33648 ? 1 - abs(frac(uv * 0.5) * 2 - 1) : frac(uv);
}

// sampler holds the wrap modes in .xy and the point-filter flag in .z.
float3 sampleTexture(int4 textureInfo, uint4 sampler, float4 uv01, float4 uv2, float2 bary)
{
    if (textureInfo.x < 0)
        return 1;
    float2 uv = uv01.xy * (1 - bary.x - bary.y) + uv01.zw * bary.x + uv2.xy * bary.y;
    uv = float2(wrapCoordinate(uv.x, sampler.x), wrapCoordinate(uv.y, sampler.y));
    float2 p = uv * float2(textureInfo.yz);
    if (sampler.z == 1)
        return fetchTexel(textureInfo, sampler.xy, int2(floor(p)));

    p -= 0.5;
    int2 lo = int2(floor(p));
    float2 f = frac(p);
    return lerp(lerp(fetchTexel(textureInfo, sampler.xy, lo),
                    fetchTexel(textureInfo, sampler.xy, lo + int2(1, 0)), f.x),
                lerp(fetchTexel(textureInfo, sampler.xy, lo + int2(0, 1)),
                    fetchTexel(textureInfo, sampler.xy, lo + int2(1, 1)), f.x), f.y);
}

SurfaceMaterial materialAt(Triangle triangle, GltfMaterial material, float2 bary)
{
    SurfaceMaterial result = {};
    result.baseColor = material.pbrBaseColorFactor.rgb * sampleTexture(material.pbrBaseColorTexture,
        material.pbrBaseColorSampler, triangle.baseColorUv01, triangle.baseColorUv2, bary);

    float3 mr = sampleTexture(material.pbrMetallicRoughnessTexture, material.pbrMetallicRoughnessSampler,
        triangle.metallicRoughnessUv01, triangle.metallicRoughnessUv2, bary);
    result.metallic = saturate(material.pbrMetallicRoughnessFactor.x * mr.b);
    result.roughness = saturate(material.pbrMetallicRoughnessFactor.y * mr.g);
    result.diffuseOnly = material.flags.y == 0;
    return result;
}

struct SurfaceInteraction
{
    float3 position;
    float3 geometricNormal;
    float3 normal;
    SurfaceMaterial material;
};

SurfaceInteraction surfaceAt(Instance instance, Triangle triangle, GltfMaterial material, float2 bary, float3 wo, float footprint)
{
    SurfaceInteraction surface = {};

    // Interpolating in object space and transforming once is equivalent to transforming the
    // vertices, because the placement is affine.
    float3 objectPosition = triangle.p0 * (1 - bary.x - bary.y) + triangle.p1 * bary.x + triangle.p2 * bary.y;
    surface.position = transformPoint(instance, objectPosition);

    float3 ng = geometricNormal(instance, triangle);
    float3 normal = transformNormal(instance,
        triangle.n0 * (1 - bary.x - bary.y) + triangle.n1 * bary.x + triangle.n2 * bary.y);
    normal = dot(normal, normal) > 1e-12 ? normalize(normal) : ng;
    if (dot(normal, ng) < 0)
        normal = -normal;
    // Perturb the interpolated normal with the normal map, in the same orientation.
    normal = applyNormalMap(normal, material, triangle, instance, bary, footprint);
    // A perturbed normal can tilt past the surface plane; keep it on the side geometry faces.
    if (dot(normal, ng) < 0)
        normal = -normal;
    if (dot(ng, wo) < 0)
    {
        ng = -ng;
        normal = -normal;
    }

    surface.geometricNormal = ng;
    surface.normal = dot(normal, wo) > 0 ? normal : ng;
    surface.material = materialAt(triangle, material, bary);
    return surface;
}

float lightPdf(Light light, float3 from, float3 to)
{
    float3 delta = to - from;
    float distanceSquared = dot(delta, delta);
    float3 crossEdges = cross(light.p1.xyz - light.p0.xyz, light.p2.xyz - light.p0.xyz);
    float twiceArea = length(crossEdges);
    float cosine = dot(crossEdges / twiceArea, -normalize(delta));
    if (materials[light.info.x].flags.x != 0)
        cosine = abs(cosine);
    if (cosine <= 0 || params.integrator.z == 0)
        return 0;

    return distanceSquared / (cosine * 0.5f * twiceArea * params.integrator.z);
}