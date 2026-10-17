// Geometry, material and light-sample evaluation shared by the path tracer and the raster preview.
//
// The file is textually included by both entry-point modules, and it declares no bindings of its own.
// The vertex arrays, primitives, instances, materials and lights it reads are globals the including
// module owns, while the bindless image and sampler heaps travel as parameters. That split is what
// lets one file serve both pipelines: sharing means sharing arithmetic, not sharing a binding set, so
// neither program is compiled against the other's globals. The one resource both renderers need, the
// acceleration structure, travels as a parameter of the occlusion helper below instead.
//
// The including module must have imported bsdf and included scene_io.h.slang and sampling.slang
// before this file, and must declare the globals the functions below read.
//
// Defines RASTER_PIPELINE (as rasterizer.slang does) to fetch textures with implicit-LOD Sample(),
// which a fragment shader can do because it has screen-space derivatives; the ray tracer has none and
// takes the explicit-gradient path instead.

#ifndef SNR_MATERIAL_EVAL_H
#define SNR_MATERIAL_EVAL_H

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
    float2 transmissionUv0, transmissionUv1, transmissionUv2;
};

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
    triangle.transmissionUv0 = transmissionUvs[ids.x];
    triangle.transmissionUv1 = transmissionUvs[ids.y];
    triangle.transmissionUv2 = transmissionUvs[ids.z];
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

// A ray tracer has no screen-space derivatives, so Sample()'s implicit LOD cannot pick a mip. The
// footprint of the ray cone at the hit is converted into texture space with the triangle's texel
// density (UV area per world area) and passed to SampleGrad as an isotropic gradient.
float2 textureGradient(float footprint, float3 edge1, float3 edge2, float2 uv0, float2 uv1, float2 uv2)
{
    // Secondary preview rays cannot use derivatives from unrelated fragment quads.
    if (footprint < 0) return float2(-1);
    float worldArea = length(cross(edge1, edge2));
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float uvArea = abs(duv1.x * duv2.y - duv1.y * duv2.x);
    float density = sqrt(max(uvArea, 1e-20) / max(worldArea, 1e-20));
    float g = footprint * density;
    return float2(g, g);
}

// Fetches one texture, or a constant 1 when the slot is empty. The full texel is returned because the
// base color's alpha drives the raster preview's alpha test and blending; the other slots read .rgb.
float4 sampleTexture(
    Texture2D<float4>.Handle images[],
    SamplerState.Handle samplers[],
    int textureIndex,
    int samplerIndex,
    float2 uv0,
    float2 uv1,
    float2 uv2,
    float2 bary,
    float2 gradient)
{
    if (textureIndex < 0)
        return float4(1);
    float2 uv = uv0 * (1 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
    Texture2D<float4> image = images[uint(textureIndex)];
    SamplerState sampler = samplers[uint(samplerIndex)];
#ifdef RASTER_PIPELINE
    // A fragment shader runs per pixel quad, so the hardware can pick the mip from the derivatives.
    return gradient.x < 0 ? image.SampleLevel(sampler, uv, 0) : image.Sample(sampler, uv);
#else
    return image.SampleGrad(sampler, uv, gradient, gradient);
#endif
}

SurfaceMaterial materialAt(
    Texture2D<float4>.Handle images[],
    SamplerState.Handle samplers[],
    Instance instance,
    Triangle triangle,
    GltfMaterial material,
    float2 bary,
    float footprint)
{
    float3 a = transformPoint(instance, triangle.p0);
    float3 b = transformPoint(instance, triangle.p1);
    float3 c = transformPoint(instance, triangle.p2);
    float3 edge1 = b - a;
    float3 edge2 = c - a;

    SurfaceMaterial result = {};
    float2 baseColorGradient = textureGradient(footprint, edge1, edge2,
        triangle.baseColorUv0, triangle.baseColorUv1, triangle.baseColorUv2);
    float4 baseColorSample = sampleTexture(images, samplers,
        material.pbrBaseColorTexture, material.pbrBaseColorSampler, triangle.baseColorUv0,
        triangle.baseColorUv1, triangle.baseColorUv2, bary, baseColorGradient);
    result.baseColor = material.pbrBaseColorFactor.rgb * baseColorSample.rgb;
    result.alpha = material.pbrBaseColorFactor.a * baseColorSample.a;

    float2 metallicRoughnessGradient = textureGradient(footprint, edge1, edge2,
        triangle.metallicRoughnessUv0, triangle.metallicRoughnessUv1, triangle.metallicRoughnessUv2);
    float3 mr = sampleTexture(images, samplers, material.pbrMetallicRoughnessTexture,
        material.pbrMetallicRoughnessSampler, triangle.metallicRoughnessUv0,
        triangle.metallicRoughnessUv1, triangle.metallicRoughnessUv2, bary, metallicRoughnessGradient).rgb;
    result.metallic = saturate(material.pbrMetallicRoughnessFactor.x * mr.b);
    result.roughness = saturate(material.pbrMetallicRoughnessFactor.y * mr.g);

    // Occlusion is the red channel of its own map; the strength lerps between no effect and full.
    float2 occlusionGradient = textureGradient(footprint, edge1, edge2,
        triangle.occlusionUv0, triangle.occlusionUv1, triangle.occlusionUv2);
    float occlusion = sampleTexture(images, samplers, material.occlusionTexture, material.occlusionSampler,
        triangle.occlusionUv0, triangle.occlusionUv1, triangle.occlusionUv2, bary, occlusionGradient).r;
    result.occlusion = lerp(1.0, occlusion, material.occlusionStrength);

    // Emission is the emissive factor times its texture; light sampling computes the same value, so a
    // directly hit emitter and a sampled one agree for multiple importance sampling.
    float2 emissiveGradient = textureGradient(footprint, edge1, edge2,
        triangle.emissiveUv0, triangle.emissiveUv1, triangle.emissiveUv2);
    result.emission = material.emissiveFactor.rgb * sampleTexture(images, samplers, material.emissiveTexture,
        material.emissiveSampler, triangle.emissiveUv0, triangle.emissiveUv1, triangle.emissiveUv2,
        bary, emissiveGradient).rgb;

    float2 transmissionGradient = textureGradient(footprint, edge1, edge2,
        triangle.transmissionUv0, triangle.transmissionUv1, triangle.transmissionUv2);
    float transmitted = sampleTexture(images, samplers, material.transmissionTextures.x,
        material.transmissionTextures.y, triangle.transmissionUv0, triangle.transmissionUv1,
        triangle.transmissionUv2, bary, transmissionGradient).r;
    result.transmission = saturate(material.transmissionVolume.x * transmitted);
    result.ior = material.transmissionVolume.y == 0 ? 1e6 : material.transmissionVolume.y;
    result.eta = 1 / result.ior;
    // Ray-based rendering uses actual geometry distances, not the baked thickness map.
    result.volume = material.transmissionVolume.z > 0;
    result.attenuation = material.attenuation;
    result.diffuseOnly = material.flags.y == 0;
    return result;
}

// Triangle emitted radiance at barycentric (bary.x, bary.y), or point radiant intensity for NEE.
// The same expression materialAt() uses, so the two estimators agree; the mip is level 0 because a
// light sample is a single point with no footprint to blur by.
float3 lightEmission(
    Texture2D<float4>.Handle images[],
    SamplerState.Handle samplers[],
    Light light,
    float2 bary)
{
    if (light.info.y == lightTypePoint)
        return light.p1.rgb;
    GltfMaterial material = materials[light.info.x];
    if (material.emissiveTexture < 0)
        return material.emissiveFactor.rgb;

    float2 uv = light.uv01.xy * (1 - bary.x - bary.y) + light.uv01.zw * bary.x + light.uv2.xy * bary.y;
    Texture2D<float4> image = images[uint(material.emissiveTexture)];
    SamplerState sampler = samplers[uint(material.emissiveSampler)];

    // Randomized light samples have no screen-space footprint. Implicit derivatives would blur the 
    // emitter according to unrelated samples in neighboring pixels of the raster preview.
    return material.emissiveFactor.rgb * image.SampleLevel(sampler, uv, 0).rgb;
}

// True when any triangle blocks the segment from `origin` to `origin + direction * distance`. The
// acceleration structure is a parameter, like the bindless heaps, so this header declares no binding
// of its own and the raster preview is the only caller. The raster pipeline never invokes it for the
// path tracer, which has its own traversal in scene.slang.
//
// The preview uses binary triangle visibility, including for transparent materials. Force opaque
// traversal so hardware can accept the first blocker without returning candidates to the shader.
bool occluded(RaytracingAccelerationStructure bvh, float3 origin, float3 direction, float distance)
{
    if (distance <= 0)
        return false;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0;
    ray.TMax = distance;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(bvh, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Mikkelsen's derivative-free tangent frame. A ray tracer has no ddx/ddy, so the frame comes from the
// triangle's own world-space edges and its normal-map UVs: dP/du and dP/dv give the tangents, which
// are orthogonalised against the interpolated shading normal. The sampled tangent-space normal then
// perturbs that normal. The raster preview shares the frame rather than using hardware derivatives so
// that a normal-mapped surface resolves to the same shading normal in both renderers.
float3 applyNormalMap(
    Texture2D<float4>.Handle images[],
    SamplerState.Handle samplers[],
    float3 normal,
    GltfMaterial material,
    Triangle triangle,
    Instance instance,
    float2 bary,
    float footprint)
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
    float3 sampled = sampleTexture(images, samplers, material.normalTexture, material.normalSampler,
        uv0, uv1, uv2, bary, gradient).rgb;
    float3 tangentNormal = float3((sampled.xy * 2 - 1) * material.normalScale, sampled.z * 2 - 1);
    return normalize(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal);
}

struct SurfaceInteraction
{
    float3 position;
    float3 geometricNormal;
    float3 normal;
    SurfaceMaterial material;
    bool frontFace;
};

SurfaceInteraction surfaceAt(
    Texture2D<float4>.Handle images[],
    SamplerState.Handle samplers[],
    Instance instance,
    Triangle triangle,
    GltfMaterial material,
    float2 bary,
    float3 wo,
    float footprint)
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
    normal = applyNormalMap(images, samplers, normal, material, triangle, instance, bary, footprint);
    // A perturbed normal can tilt past the surface plane; keep it on the side geometry faces.
    if (dot(normal, ng) < 0)
        normal = -normal;
    surface.frontFace = dot(ng, wo) >= 0;
    if (!surface.frontFace)
    {
        ng = -ng;
        normal = -normal;
    }

    surface.geometricNormal = ng;
    surface.normal = dot(normal, wo) > 0 ? normal : ng;
    surface.material = materialAt(images, samplers, instance, triangle, material, bary, footprint);
    if (surface.material.volume && !surface.frontFace) surface.material.eta = surface.material.ior;
    return surface;
}

bool acceptSurfaceHit(Instance instance, Triangle triangle, GltfMaterial material, float2 bary, float3 direction)
{
    if (material.flags.x == 0 && material.transmissionVolume.z == 0 &&
        dot(geometricNormal(instance, triangle), direction) >= 0) return false;
    if (material.flags.z == alphaModeMask)
    {
        SurfaceMaterial evaluated = materialAt(images, samplers, instance, triangle, material, bary, -1);
        if (evaluated.alpha < material.pbrMetallicRoughnessFactor.z) return false;
    }
    return true;
}

// Picks a light from the cumulative power distribution. `target` is uniform in [0, total); the light
// is the interval it lands in. Lights with no power own an empty interval and are never chosen.
uint lightIndexFromCdf(float target, uint lightCount)
{
    uint low = 0;
    uint high = lightCount;
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

// Probability that lightIndexFromCdf() returns `index`, the width of its interval over the total.
float selectProbability(uint index, uint lightCount)
{
    float total = lightCdf[lightCount];
    return total > 0 ? (lightCdf[index + 1] - lightCdf[index]) / total : 0;
}

float lightPdf(uint lightIndex, Light light, float3 from, float3 to, uint lightCount)
{
    // Point-light sampling has discrete probability mass, not a solid-angle density.
    if (light.info.y == lightTypePoint)
        return selectProbability(lightIndex, lightCount);
    float3 delta = to - from;
    float distanceSquared = dot(delta, delta);
    float3 crossEdges = cross(light.p1.xyz - light.p0.xyz, light.p2.xyz - light.p0.xyz);
    float twiceArea = length(crossEdges);
    float cosine = dot(crossEdges / twiceArea, -normalize(delta));
    if (materials[light.info.x].flags.x != 0)
        cosine = abs(cosine);
    float selectPdf = selectProbability(lightIndex, lightCount);
    if (cosine <= 0 || selectPdf <= 0)
        return 0;

    // Area pdf is selectPdf / area, so the solid-angle pdf multiplies the distance over cosine and
    // divides by the selection probability and the area.
    return distanceSquared * selectPdf / (cosine * 0.5f * twiceArea);
}

#endif // SNR_MATERIAL_EVAL_H
