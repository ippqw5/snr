// Forward, single-pass raster preview: a peer of pathtracer.slang over the same scene snapshot.
//
// One graphics pipeline draws every instance once. The vertex shader pulls its triangle's corners out
// of the scene's index and position arrays -- no vertex or index buffer is bound -- and projects them
// with the precomputed view-projection rows. The fragment shader then evaluates the same glTF
// material the path tracer does, because material_eval.h.slang is shared between the two, and shades
// it without tracing a ray: emission, an ambient term from the constant environment, and an
// unshadowed analytic contribution from the scene's light list.
//
// Because RASTER_PIPELINE is defined, the shared texture fetch uses implicit-LOD Sample(), which a
// fragment shader can do because it has screen-space derivatives; this is also what gives the preview
// correct mipmapping.

#define RASTER_PIPELINE 1

import sampling;
import bsdf;

#include "scene_io.h.slang"

// The scene, minus the acceleration structure and the per-triangle light list: this program never
// traces a ray, so it does not declare either.
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
StructuredBuffer<float> lightCdf;
uniform Texture2D<float4>.Handle images[SNR_MAX_TEXTURES];
uniform SamplerState.Handle samplers[SNR_MAX_SAMPLERS];

// Per-frame constants for the raster entries, plus the block and buffers the shared tonemap entry
// point reads (see tonemap.h.slang). presentParams belongs to the windowed present pass below.
ConstantBuffer<RasterParams>  rasterParams;
ConstantBuffer<Params>        params;
ConstantBuffer<PresentParams> presentParams;
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

// Emission, then the environment arriving from every direction, then the scene's lights. Nothing here
// is occluded and no ray is traced, which is the documented difference from the path tracer: the
// preview has no shadows and only a constant environment instead of a real one.
float3 shadeRaster(SurfaceInteraction surface, float3 view)
{
    SurfaceMaterial material = surface.material;
    float3 radiance = material.emission;

    // A constant environment integrated over the hemisphere: the diffuse lobe keeps the base color
    // attenuated by Fresnel, the specular lobe returns the Fresnel-tinted environment, and occlusion
    // scales how much of it arrives. Reflecting a constant environment does not blur it, so the
    // roughness only has to enter through the Fresnel term.
    float3 fresnel = fresnelSchlick(specularReflectance(material), saturate(dot(surface.normal, view)));
    float3 ambient =
        material.baseColor * (1 - material.metallic) * (1 - fresnel) + fresnel;
    radiance += rasterParams.environment.rgb * material.occlusion * ambient;

    // Like the path tracer's next-event estimation, but sampled at each light's centroid and added
    // without a visibility test. The strata are fixed rather than random so the preview is stable
    // from frame to frame. lightPdf() returns the solid-angle pdf the same way NEE computes it.
    const uint kMaxLightSamples = 8;
    uint       lightCount = rasterParams.counts.x;
    uint       sampleCount = min(kMaxLightSamples, lightCount);
    float      total = lightCdf[lightCount];
    if (sampleCount > 0 && total > 0)
    {
        for (uint i = 0; i < sampleCount; ++i)
        {
            float target = total * (float(i) + 0.5) / float(sampleCount);
            uint  lightIndex = lightIndexFromCdf(target, lightCount);
            Light light = lights[lightIndex];

            float3 centroid = (light.p0.xyz + light.p1.xyz + light.p2.xyz) / 3;
            float3 delta = centroid - surface.position;
            float  distance = length(delta);
            if (distance <= 0)
                continue;
            float3 wi = delta / distance;

            float pdf = lightPdf(lightIndex, light, surface.position, centroid, lightCount);
            float cosine = dot(surface.normal, wi);
            if (pdf <= 0 || cosine <= 0)
                continue;

            // The centroid is the barycentric (1/3, 1/3), the same point lightPdf() measured.
            float3 emission = lightEmission(images, samplers, light, float2(1.0 / 3, 1.0 / 3));
            BsdfEvaluation bsdf = evaluateBsdf(material, surface.normal, view, wi);
            radiance += bsdf.value * emission * cosine / (pdf * float(sampleCount));
        }
    }
    return radiance;
}

struct RasterPixel
{
    float3 worldPosition : POSITION;
    nointerpolation uint instanceIndex : INSTANCE;
};

[shader("fragment")]
float4 rasterFS(
    RasterPixel input,
    uint        primitiveIndex : SV_PrimitiveID,
    float3      barycentric : SV_Barycentrics
)
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

    return float4(shadeRaster(surface, view), 1);
}

// Fullscreen triangle for the windowed present pass. Three vertices at (-1,-1), (3,-1), (-1,3) cover
// the clip-space square with one primitive, so a frame costs one draw and no index buffer.
struct PresentVertex
{
    float4 position : SV_Position;
};

[shader("vertex")]
PresentVertex presentVS(uint vertexId : SV_VertexID)
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    PresentVertex output;
    output.position = float4(corner * 2 - 1, 0, 1);
    return output;
}

// Tone maps the film into a swapchain image. SV_Position names the pixel directly, so the film is
// read at the same texel the fragment covers and no bounds check is needed: the triangle covers
// exactly the target. The film holds the same linear radiance headless readback tone maps, so the
// windowed image matches the offscreen one for the same camera and exposure.
[shader("fragment")]
float4 presentFS(PresentVertex input)
{
    uint   pixel = uint(input.position.y) * presentParams.image.x + uint(input.position.x);
    float4 sum = film[pixel];
    float3 color = tonemapCore(sum.rgb / sum.w, presentParams.config.x);
    // An sRGB target format encodes on write, so it receives linear values; otherwise encode here.
    return float4(presentParams.config.y != 0 ? color : linearToSrgb(color), 1);
}
