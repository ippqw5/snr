import bsdf;

struct Triangle
{
    float4 p0, p1, p2;
    float4 n0, n1, n2;
    float4 uv01, uv2;
    float4 mrUv01, mrUv02;
};

struct Primitive
{
    uint firstTriangle;
    uint triangleCount;
    uint material;
    uint reserved;
};

// Places a primitive in the world. The rows are explicit so the host and shader agree on
// layout without depending on either side's matrix convention.
struct Instance
{
    float4 transformRows[3];
    float4 normalRows[3];
    uint primitive;
    uint material;
    uint lightBase;
    uint flags;
};

// A world-space emissive triangle used for next-event estimation.
struct Light
{
    float4 p0, p1, p2;
    uint4 info;
};

struct Material
{
    float4 baseColor;
    float4 emission;
    int4 textureInfo;
    uint4 settings;
    float4 pbr;
    int4 mrTextureInfo;
    uint4 mrSettings;
};

struct Params
{
    uint4 image;
    uint4 integrator;
    float4 eye;
    float4 forward;
    float4 right;
    float4 up;
    float4 environment;
};

// A committed hit, resolved to the instance that produced it.
struct Hit
{
    float t;
    uint instance;
    uint primitive;
    float2 bary;
};

RaytracingAccelerationStructure sceneBVH;
StructuredBuffer<Triangle> triangles;
StructuredBuffer<Primitive> primitives;
StructuredBuffer<Instance> instances;
StructuredBuffer<Material> materials;
StructuredBuffer<Light> lights;
StructuredBuffer<float4> texels;
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
    return triangles[primitives[instance.primitive].firstTriangle + localTriangle];
}

// World-space normal taken from the transformed edges, so mirrored instances still face correctly.
float3 geometricNormal(Instance instance, Triangle triangle)
{
    float3 a = transformPoint(instance, triangle.p0.xyz);
    float3 b = transformPoint(instance, triangle.p1.xyz);
    float3 c = transformPoint(instance, triangle.p2.xyz);
    return normalize(cross(b - a, c - a));
}

// Index of the world-space light a hit emitted from, or -1 when it is not an emitter.
int lightIndexOf(Instance instance, uint localTriangle)
{
    return instance.lightBase == 0xffffffffu ? -1 : int(instance.lightBase + localTriangle);
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
            Material material = materials[instance.material];
            Triangle triangle = triangleAt(instance, query.CandidatePrimitiveIndex());

            if (material.settings.z != 0 || dot(geometricNormal(instance, triangle), direction) < 0)
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

float3 sampleTexture(int4 textureInfo, uint2 wrap, float4 uv01, float4 uv2, float2 bary)
{
    if (textureInfo.x < 0)
        return 1;
    float2 uv = uv01.xy * (1 - bary.x - bary.y) + uv01.zw * bary.x + uv2.xy * bary.y;
    uv = float2(wrapCoordinate(uv.x, wrap.x), wrapCoordinate(uv.y, wrap.y));
    float2 p = uv * float2(textureInfo.yz);
    if (textureInfo.w == 1)
        return fetchTexel(textureInfo, wrap, int2(floor(p)));

    p -= 0.5;
    int2 lo = int2(floor(p));
    float2 f = frac(p);
    return lerp(lerp(fetchTexel(textureInfo, wrap, lo),
                    fetchTexel(textureInfo, wrap, lo + int2(1, 0)), f.x),
                lerp(fetchTexel(textureInfo, wrap, lo + int2(0, 1)),
                    fetchTexel(textureInfo, wrap, lo + int2(1, 1)), f.x), f.y);
}

SurfaceMaterial materialAt(Triangle triangle, Material material, float2 bary)
{
    SurfaceMaterial result = {};
    result.baseColor = material.baseColor.rgb * sampleTexture(material.textureInfo,
        material.settings.xy, triangle.uv01, triangle.uv2, bary);

    float3 mr = sampleTexture(material.mrTextureInfo, material.mrSettings.xy, triangle.mrUv01, triangle.mrUv02, bary);
    result.metallic = saturate(material.pbr.x * mr.b);
    result.roughness = saturate(material.pbr.y * mr.g);
    result.diffuseOnly = material.settings.w == 0;
    return result;
}

struct SurfaceInteraction
{
    float3 position;
    float3 geometricNormal;
    float3 normal;
    SurfaceMaterial material;
};

SurfaceInteraction surfaceAt(Instance instance, Triangle triangle, Material material, float2 bary, float3 wo)
{
    SurfaceInteraction surface = {};

    // Interpolating in object space and transforming once is equivalent to transforming the
    // vertices, because the placement is affine.
    float3 objectPosition = triangle.p0.xyz * (1 - bary.x - bary.y) + triangle.p1.xyz * bary.x + triangle.p2.xyz * bary.y;
    surface.position = transformPoint(instance, objectPosition);

    float3 ng = geometricNormal(instance, triangle);
    float3 normal = transformNormal(instance,
        triangle.n0.xyz * (1 - bary.x - bary.y) + triangle.n1.xyz * bary.x + triangle.n2.xyz * bary.y);
    normal = dot(normal, normal) > 1e-12 ? normalize(normal) : ng;
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
    if (materials[light.info.x].settings.z != 0)
        cosine = abs(cosine);
    if (cosine <= 0 || params.integrator.z == 0)
        return 0;

    return distanceSquared / (cosine * 0.5f * twiceArea * params.integrator.z);
}