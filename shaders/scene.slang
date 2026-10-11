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

// A ray tracer has no screen-space derivatives, so Sample()'s implicit LOD cannot pick a mip. The
// footprint of the ray cone at the hit is converted into texture space with the triangle's texel
// density (UV area per world area) and passed to SampleGrad as an isotropic gradient.
float2 textureGradient(float footprint, float3 edge1, float3 edge2, float2 uv0, float2 uv1, float2 uv2)
{
    float worldArea = length(cross(edge1, edge2));
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float uvArea = abs(duv1.x * duv2.y - duv1.y * duv2.x);
    float density = sqrt(max(uvArea, 1e-20) / max(worldArea, 1e-20));
    float g = footprint * density;
    return float2(g, g);
}

float3 sampleTexture(int textureIndex, int samplerIndex, float2 uv0, float2 uv1, float2 uv2, float2 bary, float2 gradient)
{
    if (textureIndex < 0)
        return 1;
    float2 uv = uv0 * (1 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
    Texture2D<float4> image = images[uint(textureIndex)];
    SamplerState sampler = samplers[uint(samplerIndex)];
    return image.SampleGrad(sampler, uv, gradient, gradient).rgb;
}

SurfaceMaterial materialAt(Instance instance, Triangle triangle, GltfMaterial material, float2 bary, float footprint)
{
    float3 a = transformPoint(instance, triangle.p0);
    float3 b = transformPoint(instance, triangle.p1);
    float3 c = transformPoint(instance, triangle.p2);
    float3 edge1 = b - a;
    float3 edge2 = c - a;

    SurfaceMaterial result = {};
    float2 baseColorGradient = textureGradient(footprint, edge1, edge2,
        triangle.baseColorUv0, triangle.baseColorUv1, triangle.baseColorUv2);
    result.baseColor = material.pbrBaseColorFactor.rgb * sampleTexture(material.pbrBaseColorTexture,
        material.pbrBaseColorSampler, triangle.baseColorUv0, triangle.baseColorUv1, triangle.baseColorUv2,
        bary, baseColorGradient);

    float2 metallicRoughnessGradient = textureGradient(footprint, edge1, edge2,
        triangle.metallicRoughnessUv0, triangle.metallicRoughnessUv1, triangle.metallicRoughnessUv2);
    float3 mr = sampleTexture(material.pbrMetallicRoughnessTexture, material.pbrMetallicRoughnessSampler,
        triangle.metallicRoughnessUv0, triangle.metallicRoughnessUv1, triangle.metallicRoughnessUv2,
        bary, metallicRoughnessGradient);
    result.metallic = saturate(material.pbrMetallicRoughnessFactor.x * mr.b);
    result.roughness = saturate(material.pbrMetallicRoughnessFactor.y * mr.g);

    // Occlusion is the red channel of its own map; the strength lerps between no effect and full.
    float2 occlusionGradient = textureGradient(footprint, edge1, edge2,
        triangle.occlusionUv0, triangle.occlusionUv1, triangle.occlusionUv2);
    float occlusion = sampleTexture(material.occlusionTexture, material.occlusionSampler,
        triangle.occlusionUv0, triangle.occlusionUv1, triangle.occlusionUv2, bary, occlusionGradient).r;
    result.occlusion = lerp(1.0, occlusion, material.occlusionStrength);

    // Emission is the emissive factor times its texture; light sampling computes the same value, so a
    // directly hit emitter and a sampled one agree for multiple importance sampling.
    float2 emissiveGradient = textureGradient(footprint, edge1, edge2,
        triangle.emissiveUv0, triangle.emissiveUv1, triangle.emissiveUv2);
    result.emission = material.emissiveFactor.rgb * sampleTexture(material.emissiveTexture,
        material.emissiveSampler, triangle.emissiveUv0, triangle.emissiveUv1, triangle.emissiveUv2,
        bary, emissiveGradient);

    result.diffuseOnly = material.flags.y == 0;
    return result;
}

// Emitted radiance of a light triangle at barycentric (bary.x, bary.y), for next-event estimation.
// The same expression materialAt() uses, so the two estimators agree; the mip is level 0 because a
// light sample is a single point with no footprint to blur by.
float3 lightEmission(Light light, float2 bary)
{
    GltfMaterial material = materials[light.info.x];
    return material.emissiveFactor.rgb * sampleTexture(material.emissiveTexture, material.emissiveSampler,
        light.uv01.xy, light.uv01.zw, light.uv2.xy, bary, float2(0));
}

// Mikkelsen's derivative-free tangent frame. A ray tracer has no ddx/ddy, so the frame comes from the
// triangle's own world-space edges and its normal-map UVs: dP/du and dP/dv give the tangents, which
// are orthogonalised against the interpolated shading normal. The sampled tangent-space normal then
// perturbs that normal.
float3 applyNormalMap(float3 normal, GltfMaterial material, Triangle triangle, Instance instance, float2 bary, float footprint)
{
    if (material.normalTexture < 0)
        return normal;

    float2 uv0 = triangle.normalUv0;
    float2 uv1 = triangle.normalUv1;
    float2 uv2 = triangle.normalUv2;
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
    // Degenerate UVs leave the tangent direction undefined; keep the interpolated normal.
    if (abs(determinant) < 1e-12)
        return normal;

    float3 a = transformPoint(instance, triangle.p0);
    float3 b = transformPoint(instance, triangle.p1);
    float3 c = transformPoint(instance, triangle.p2);
    float3 edge1 = b - a;
    float3 edge2 = c - a;
    float invDet = 1 / determinant;
    float3 tangent = normalize((edge1 * duv2.y - edge2 * duv1.y) * invDet);
    // Re-orthogonalise against the normal being perturbed, then rebuild a right-handed frame.
    tangent = normalize(tangent - normal * dot(normal, tangent));
    float3 bitangent = cross(normal, tangent);

    float2 gradient = textureGradient(footprint, edge1, edge2, uv0, uv1, uv2);
    float3 sampled = sampleTexture(material.normalTexture, material.normalSampler, uv0, uv1, uv2, bary, gradient);
    float3 tangentNormal = float3((sampled.xy * 2 - 1) * material.normalScale, sampled.z * 2 - 1);
    return normalize(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal);
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
    surface.material = materialAt(instance, triangle, material, bary, footprint);
    return surface;
}

// Picks a light from the cumulative power distribution. `target` is uniform in [0, total); the light
// is the interval it lands in. Lights with no power own an empty interval and are never chosen.
uint sampleLightIndex(float target)
{
    uint low = 0;
    uint high = params.integrator.z;
    while (low < high)
    {
        uint mid = (low + high) / 2;
        if (lightCdf[mid] <= target)
            low = mid + 1;
        else
            high = mid;
    }
    return low == 0 ? 0 : low - 1;
}

// Probability that sampleLightIndex() returns `index`, the width of its interval over the total.
float selectProbability(uint index)
{
    float total = lightCdf[params.integrator.z];
    return total > 0 ? (lightCdf[index + 1] - lightCdf[index]) / total : 0;
}

float lightPdf(uint lightIndex, Light light, float3 from, float3 to)
{
    float3 delta = to - from;
    float distanceSquared = dot(delta, delta);
    float3 crossEdges = cross(light.p1.xyz - light.p0.xyz, light.p2.xyz - light.p0.xyz);
    float twiceArea = length(crossEdges);
    float cosine = dot(crossEdges / twiceArea, -normalize(delta));
    if (materials[light.info.x].flags.x != 0)
        cosine = abs(cosine);
    float selectPdf = selectProbability(lightIndex);
    if (cosine <= 0 || selectPdf <= 0)
        return 0;

    // Area pdf is selectPdf / area, so the solid-angle pdf multiplies the distance over cosine and
    // divides by the selection probability and the area.
    return distanceSquared * selectPdf / (cosine * 0.5f * twiceArea);
}