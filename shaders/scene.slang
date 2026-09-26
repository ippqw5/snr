struct Triangle
{
    float4 p0, p1, p2;
    float4 n0, n1, n2;
    float4 uv01, uv2;
    uint4 info;
};

struct Material
{
    float4 baseColor;
    float4 emission;
    int4 textureInfo;
    uint4 settings;
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

float3 fetchTexel(Material m, int2 p)
{
    int x = wrapTexel(p.x, m.textureInfo.y, m.settings.x);
    int y = wrapTexel(p.y, m.textureInfo.z, m.settings.y);
    return texels[m.textureInfo.x + y * m.textureInfo.y + x].rgb;
}

float3 baseColorAt(Triangle triangle, Material material, float2 bary)
{
    float3 color = material.baseColor.rgb;
    if (material.textureInfo.x < 0)
        return color;
    float2 uv = triangle.uv01.xy * (1 - bary.x - bary.y) + triangle.uv01.zw * bary.x
                + triangle.uv2.xy * bary.y;

    float2 p = uv * float2(material.textureInfo.yz);
    if (material.textureInfo.w == 1) return color * fetchTexel(material, int2(p));
    p -= 0.5;
    int2 lo = int2(floor(p));
    float2 f = frac(p);
    return color * lerp(
        lerp(fetchTexel(material, lo), fetchTexel(material, lo + int2(1, 0)), f.x),
        lerp(fetchTexel(material, lo + int2(0, 1)), fetchTexel(material, lo + int2(1, 1)), f.x),
        f.y);
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

