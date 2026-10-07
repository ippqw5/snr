import bsdf;

struct Triangle
{
    float4 p0, p1, p2;
    float4 n0, n1, n2;
    float4 uv01, uv2;
    float4 mrUv01, mrUv02;
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

struct Hit
{
    float t;
    uint primitive;
    float2 bary;
};

RaytracingAccelerationStructure sceneBVH;
StructuredBuffer<Triangle> triangles;
StructuredBuffer<Material> materials;
StructuredBuffer<uint> lights;
StructuredBuffer<float4> texels;
ConstantBuffer<Params> params;

float3 geometricNormal(Triangle t)
{
    return normalize(cross(t.p1.xyz - t.p0.xyz, t.p2.xyz - t.p0.xyz));
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
            Triangle triangle = triangles[query.CandidatePrimitiveIndex()];
            Material material = materials[triangle.info.x];

            if (material.settings.z != 0 || dot(geometricNormal(triangle), direction) < 0)
                query.CommitNonOpaqueTriangleHit();
        }
    }

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;

    hit.t = query.CommittedRayT();
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
    int x = wrapTexel(p.x, textureInfo.y, textureInfo.w);
    int y = wrapTexel(p.y, textureInfo.z, textureInfo.w);
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

SurfaceInteraction surfaceAt(Triangle triangle, Material material, float2 bary, float3 wo)
{
    SurfaceInteraction surface = {};
    surface.position = triangle.p0.xyz * (1 - bary.x - bary.y) + triangle.p1.xyz * bary.x + triangle.p2.xyz * bary.y;

    float3 ng = geometricNormal(triangle);
    float3 normal = triangle.n0.xyz * (1 - bary.x - bary.y) + triangle.n1.xyz * bary.x + triangle.n2.xyz * bary.y;
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

float lightPdf(Triangle light, float3 from, float3 to)
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

