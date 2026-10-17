// Forward, single-pass raster preview: a peer of pathtracer.slang over the same scene snapshot.
//
// The vertex shader pulls each triangle's corners out of the scene's index and position arrays -- no
// vertex or index buffer is bound -- and projects them with the precomputed view-projection rows. The
// OPAQUE entry forces early depth tests; MASK retains late depth writes for alpha discard. BLEND
// instances arrive sorted back to front after them. The shared fragment shading then
// evaluates the same glTF material the path tracer does, because material_eval.h.slang is shared
// between the two, and shades it with emission, an ambient term from the constant environment, and a
// contribution from the scene's light list that an inline ray query shadows: the fragment shader
// traces one occlusion ray per light sample against the same acceleration structure the path tracer
// uses.
//
// Because RASTER_PIPELINE is defined, the shared texture fetch uses implicit-LOD Sample(), which a
// fragment shader can do because it has screen-space derivatives; this is also what gives the preview
// correct mipmapping.

#define RASTER_PIPELINE 1

import sampling;
import bsdf;
import medium;

#include "scene_io.h.slang"

// The scene arrays plus the acceleration structure the shadow rays test against. The raster program
// traces only occlusion rays, so it declares the BVH but not the per-triangle light list the path
// tracer's next-event estimation needs.
uniform RaytracingAccelerationStructure sceneBVH;
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
StructuredBuffer<float> lightCdf;
uniform Texture2D<float4>.Handle images[SNR_MAX_TEXTURES];
uniform SamplerState.Handle samplers[SNR_MAX_SAMPLERS];

// Per-frame constants for the raster entries, plus the block and buffers the shared tonemap entry
// point reads (see tonemap.h.slang).
ConstantBuffer<RasterParams>  rasterParams;
ConstantBuffer<Params>        params;
RWStructuredBuffer<float4> film;
RWStructuredBuffer<float4> display;

// Geometry, material and light evaluation, shared with the path tracer.
#include "material_eval.h.slang"
// Exposure, tone mapping and sRGB encoding, shared with the path tracer.
#include "tonemap.h.slang"

struct RasterVertex
{
    float4 clipPosition : SV_Position;
    float3 worldPosition : POSITION;
    nointerpolation uint instanceIndex : INSTANCE;
};

// One draw covers a whole primitive as 3 * triangleCount vertices, so the vertex id names the triangle
// and one of its corners directly. The instance index arrives through SV_StartInstanceLocation, which
// the backend maps to the draw's first instance.
[shader("vertex")]
RasterVertex rasterVS(uint vertexId : SV_VertexID, uint instanceIndex : SV_StartInstanceLocation)
{
    Instance  instance = instances[instanceIndex];
    Primitive primitive = primitives[instance.primitive];

    uint triangleIndex = min(vertexId / 3, primitive.triangleCount - 1);
    uint corner = vertexId % 3;

    uint3 ids = indices[primitive.firstTriangle + triangleIndex] + primitive.vertexBase;
    uint  vertex = corner == 0 ? ids.x : (corner == 1 ? ids.y : ids.z);

    float3 worldPosition = transformPoint(instance, positions[vertex]);

    RasterVertex output;
    output.clipPosition = float4(
        dot(rasterParams.viewProjRows[0].xyz, worldPosition) + rasterParams.viewProjRows[0].w,
        dot(rasterParams.viewProjRows[1].xyz, worldPosition) + rasterParams.viewProjRows[1].w,
        dot(rasterParams.viewProjRows[2].xyz, worldPosition) + rasterParams.viewProjRows[2].w,
        dot(rasterParams.viewProjRows[3].xyz, worldPosition) + rasterParams.viewProjRows[3].w
    );
    output.worldPosition = worldPosition;
    output.instanceIndex = instanceIndex;
    return output;
}

// Emission, then the environment, then sampled area lights and analytic point lights with shadow
// rays. The environment stays unshadowed because a constant environment has no direction to trace 
// toward.
float3 shadeRaster(SurfaceInteraction surface, float3 view, uint2 pixel)
{
    SurfaceMaterial material = surface.material;
    float3 radiance = material.emission;

    // A constant environment integrated over the hemisphere: the diffuse lobe keeps the base color
    // attenuated by Fresnel, the specular lobe returns the Fresnel-tinted environment, and occlusion
    // scales how much of it arrives. Reflecting a constant environment does not blur it, so the
    // roughness only has to enter through the Fresnel term.
    float3 fresnel = materialFresnel(material, saturate(dot(surface.normal, view)));
    float3 ambient =
        material.baseColor * (1 - material.metallic) * (1 - material.transmission) * (1 - fresnel) + fresnel;
    radiance += rasterParams.environment.rgb * material.occlusion * ambient;

    const uint kLightSamples = 32;
    uint       lightCount = rasterParams.counts.z; // Only sample the triangle-light CDF prefix.
    float      total = lightCdf[lightCount];
    if (kLightSamples > 0 && total > 0)
    {
        uint rng = hash32(pixel.x ^ hash32(pixel.y));
        float shift = randomFloat(rng);
        for (uint i = 0; i < kLightSamples; ++i)
        {
            float target = total * (float(i) + randomFloat(rng)) / float(kLightSamples);
            // Rounding must not put the last sample at the CDF's exclusive upper bound
            target = min(target, asfloat(asuint(total) - 1u));
            uint  lightIndex = lightIndexFromCdf(target, lightCount);
            Light light = lights[lightIndex];

            float u = (target - lightCdf[lightIndex]) / (lightCdf[lightIndex + 1] - lightCdf[lightIndex]);
            float r = sqrt(saturate(u));
            float v = frac(float(reversebits(i)) * (1.0 / 4294967296.0) + shift);
            float2 bary = float2(r * (1 - v), r * v);
            float3 lightPoint = light.p0.xyz * (1 - r) + light.p1.xyz * bary.x + light.p2.xyz * bary.y;
            float3 delta = lightPoint - surface.position;
            float distance = length(delta);
            if (distance <= 0)
                continue;
            float3 wi = delta / distance;

            float pdf = lightPdf(lightIndex, light, surface.position, lightPoint, lightCount);
            float cosine = dot(surface.normal, wi);
            if (pdf <= 0 || cosine <= 0)
                continue;

            // Shadow ray, biased off both surfaces the way the path tracer biases its NEE ray: one
            // endpoint sits on the shaded surface, the other on the emitter, and both would otherwise
            // re-intersect themselves at t = 0.
            float3 lightNormal = normalize(cross(light.p1.xyz - light.p0.xyz, light.p2.xyz - light.p0.xyz));
            if (dot(lightNormal, -wi) < 0)
                lightNormal = -lightNormal;
            float3 shadowOrigin = offsetRay(surface.position, surface.geometricNormal);
            float3 shadowTarget = offsetRay(lightPoint, lightNormal);
            float3 shadowDelta = shadowTarget - shadowOrigin;
            float  shadowDistance = length(shadowDelta);
            if (shadowDistance <= 0 ||
                occluded(sceneBVH, shadowOrigin, shadowDelta / shadowDistance, shadowDistance - 1e-4))
                continue;

            float3 emission = lightEmission(images, samplers, light, bary);
            BsdfEvaluation bsdf = evaluateBsdf(material, surface.normal, view, wi);
            radiance += bsdf.value * emission * cosine / (pdf * float(kLightSamples));
        }
    }
    // Each point light is evaluated exactly once, independently of area-light sampling. There is
    // no emitter-position integral: intensity / distance^2 is the incident radiance weight.
    for (uint index = lightCount; index < rasterParams.counts.x; ++index)
    {
        Light light = lights[index];
        float3 delta = light.p0.xyz - surface.position;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared <= 1e-12) continue;
        float3 wi = delta * rsqrt(distanceSquared);
        float cosine = dot(surface.normal, wi);
        if (cosine <= 0) continue;
        float3 origin = offsetRay(surface.position, surface.geometricNormal);
        float3 shadowDelta = light.p0.xyz - origin;
        float distance = length(shadowDelta);
        if (distance <= 1e-4 || occluded(sceneBVH, origin, shadowDelta / distance, distance - 1e-4)) continue;
        BsdfEvaluation bsdf = evaluateBsdf(material, surface.normal, view, wi);
        radiance += bsdf.value * light.p1.rgb * cosine / distanceSquared;
    }
    return radiance;
}

struct RasterPixel
{
    float4 pixelPosition : SV_Position;
    float3 worldPosition : POSITION;
    nointerpolation uint instanceIndex : INSTANCE;
};

// A stable, evenly spread colour per object id. The golden-ratio hue step keeps successive ids far
// apart on the colour wheel, so neighbouring instances are never confusable, and an id keeps its
// colour across runs.
float3 instanceColor(uint id)
{
    float  hue = frac(float(id) * 0.6180339887);
    float3 rgb = abs(frac(hue + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1;
    return saturate(rgb);
}

// The debug views: one channel of the pass drawn as a colour instead of the shaded image. They are
// display values, not radiance, so they are read through the shared tone map -- the HDR output holds
// them as written, while the PNG shows them tone mapped like any other frame.
bool tracePreviewSurface(float3 origin, float3 direction, out SurfaceInteraction surface, out uint materialId, out float distance)
{
    surface = {}; materialId = 0; distance = 0;
    RayDesc ray;
    ray.Origin = origin; ray.Direction = direction; ray.TMin = 0; ray.TMax = 1e30;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneBVH, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        Instance instance = instances[query.CandidateInstanceID()];
        Triangle triangle = triangleAt(instance, query.CandidatePrimitiveIndex());
        GltfMaterial material = materials[instance.material];
        if (acceptSurfaceHit(instance, triangle, material, query.CandidateTriangleBarycentrics(), direction))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    Instance instance = instances[query.CommittedInstanceID()];
    Triangle triangle = triangleAt(instance, query.CommittedPrimitiveIndex());
    materialId = instance.material;
    distance = query.CommittedRayT();
    surface = surfaceAt(images, samplers, instance, triangle, materials[materialId],
                        query.CommittedTriangleBarycentrics(), -direction, -1);
    return true;
}

float3 shadeTransmissivePreview(SurfaceInteraction surface, uint materialId, float3 view, uint2 pixel)
{
    MediumState media = {};
    float3 radiance = 0;
    float3 throughput = exp(-segmentAbsorption(media,surface.material,surface.frontFace)*
                            length(surface.position-rasterParams.eye.xyz));
    uint rng = hash32(pixel.x ^ hash32(pixel.y) ^ 0x7193u);
    for (uint boundary = 0; boundary < 12; ++boundary)
    {
        setMediumEta(surface.material, surface.frontFace, media, materialId);
        radiance += throughput * shadeRaster(surface, view, pixel);
        SurfaceMaterial m = surface.material;
        float transmission = m.transmission * (1-m.metallic);
        if (transmission <= 0) break;
        float3 h = isSmooth(m) ? surface.normal : sampleGgxNormal(surface.normal,view,max(1e-4,m.roughness*m.roughness),rng);
        float3 direction;
        if (m.volume) direction = refract(-view,h,m.eta);
        else
        {
            float3 reflected = reflect(-view,h);
            direction = reflected - 2*dot(reflected,surface.normal)*surface.normal;
        }
        if (dot(direction,surface.geometricNormal)>=0 || dot(direction,direction)<1e-8) break;
        float f = dielectricFresnel(abs(dot(view,h)),m.eta);
        throughput *= m.baseColor * transmission * (1-f) * (m.volume ? m.eta*m.eta : 1);
        if (!crossMedium(media,m,surface.frontFace,materialId)) break;
        float3 origin = offsetRay(surface.position,-surface.geometricNormal);
        SurfaceInteraction next; uint nextMaterial; float distance;
        if (!tracePreviewSurface(origin,direction,next,nextMaterial,distance))
        {
            radiance += throughput * rasterParams.environment.rgb;
            break;
        }
        throughput *= exp(-segmentAbsorption(media,next.material,next.frontFace)*distance);
        surface = next; materialId = nextMaterial; view = -direction;
    }
    return radiance;
}

float3 debugColor(uint view, uint objectId, SurfaceInteraction surface, float3 worldPosition)
{
    if (view == rasterDebugDepth)
    {
        // Distance along the view axis, normalised by the eye-to-target distance, so the scale follows
        // the model rather than a fixed near/far pair, the orbit target sits mid grey, and farther
        // geometry is brighter.
        float viewZ = dot(rasterParams.viewProjRows[3].xyz, worldPosition - rasterParams.eye.xyz);
        return float3(saturate(viewZ / max(rasterParams.environment.w, 1e-4)));
    }
    if (view == rasterDebugObjectId)
        return instanceColor(objectId);
    if (view == rasterDebugBaseColor)
        return surface.material.baseColor;
    // rasterDebugNormal: the world-space shading normal, mapped from [-1, 1] to [0, 1].
    return surface.normal * 0.5 + 0.5;
}

// The fragment outputs, spelled out with explicit target semantics: a return value plus an `out`
// parameter get their locations assigned in the wrong order, which silently puts the id in the colour
// target.
struct RasterOutput
{
    float4 color : SV_Target0;
    uint   objectId : SV_Target1;
};

RasterOutput shadeRasterPixel(RasterPixel input, uint primitiveIndex, float3 barycentric)
{
    Instance      instance = instances[input.instanceIndex];
    Primitive     primitive = primitives[instance.primitive];
    GltfMaterial  material = materials[instance.material];
    uint          triangleIndex = min(primitiveIndex, primitive.triangleCount - 1);
    Triangle      triangle = triangleAt(instance, triangleIndex);

    // SV_Barycentrics is ordered against the triangle's vertices 0, 1, 2 -- the order triangleAt()
    // returns and the order the vertex shader submitted them in. The shared convention names vertex 1
    // with bary.x and vertex 2 with bary.y (vertex 0's weight is the remainder), so the two weights
    // are shifted across.
    float2 bary = float2(barycentric.y, barycentric.z);

    float3 view = normalize(rasterParams.eye.xyz - input.worldPosition);
    // The footprint only builds an explicit mip gradient for SampleGrad; under RASTER_PIPELINE the
    // fetch uses Sample() and the hardware derives the mip from the pixel quad, so it is unused.
    SurfaceInteraction surface =
        surfaceAt(images, samplers, instance, triangle, material, bary, view, 0.0);

    // glTF alpha: MASK drops fragments whose coverage is below the cutoff, and its survivors are
    // opaque. BLEND keeps the coverage as the fragment's opacity, which the transparent pipeline
    // blends with. OPAQUE writes neither. The colour target's alpha doubles as the film's sample
    // counter, so only the blended pass may carry coverage into it, and that is the one pipeline that
    // leaves the alpha it finds alone.
    uint  alphaMode = material.flags.z;
    float alpha = 1;
    if (alphaMode == alphaModeMask)
    {
        if (surface.material.alpha < material.pbrMetallicRoughnessFactor.z)
            discard;
    }
    else if (alphaMode == alphaModeBlend)
    {
        alpha = surface.material.alpha;
    }

    // The object id names the instance the fragment came from, one-based so the zero the clear leaves
    // in unwritten pixels stays outside the instance range. It is the raster pass's answer to "what is
    // under this pixel", for picking and segmentation; the object-id debug view colours the same index.
    uint   objectId = input.instanceIndex + 1;
    uint   debugView = rasterParams.counts.y;
    float3 color = debugView == rasterDebugShaded
                       ? (surface.material.transmission > 0
                              ? shadeTransmissivePreview(surface, instance.material, view, uint2(input.pixelPosition.xy))
                              : shadeRaster(surface, view, uint2(input.pixelPosition.xy)))
                       : debugColor(debugView, objectId, surface, input.worldPosition);

    RasterOutput output;
    output.color = float4(color, alpha);
    output.objectId = objectId;
    return output;
}

// OPAQUE fragments cannot discard. Test and write depth before texture evaluation, transmission
// traversal and the area-light loop, so hidden surfaces (including the studio floor) do no shading.
[shader("fragment")]
[earlydepthstencil]
RasterOutput rasterOpaqueFS(RasterPixel input, uint primitiveIndex : SV_PrimitiveID,
                            float3 barycentric : SV_Barycentrics)
{
    return shadeRasterPixel(input, primitiveIndex, barycentric);
}

// MASK retains late depth writes, and BLEND retains normal depth testing without depth writes.
[shader("fragment")]
RasterOutput rasterFS(RasterPixel input, uint primitiveIndex : SV_PrimitiveID,
                      float3 barycentric : SV_Barycentrics)
{
    return shadeRasterPixel(input, primitiveIndex, barycentric);
}
