import sampling;
import scene;

RWStructuredBuffer<float4> film;
RWStructuredBuffer<float4> display;

float3 tracePath(float3 origin, float3 direction, inout uint rng)
{
    float3 radiance = 0;
    float3 throughput = 1;
    float3 previousPosition = origin;
    float previousPdf = 0;
    bool previousUseNee = false;

    for (uint bounce = 0; bounce < params.integrator.x; ++bounce)
    {
        Hit hit;
        if (!traceScene(origin, direction, 1e30, false, hit))
        {
            radiance += throughput * params.environment.rgb;
            break;
        }

        Triangle triangle = triangles[hit.primitive];
        Material material = materials[triangle.info.x];
        float3 position = triangle.p0.xyz * (1 - hit.bary.x - hit.bary.y) + triangle.p1.xyz * hit.bary.x + triangle.p2.xyz * hit.bary.y;

        float3 ng = geometricNormal(triangle);
        float3 normal = normalize(triangle.n0.xyz * (1 - hit.bary.x - hit.bary.y) + triangle.n1.xyz * hit.bary.x + triangle.n2.xyz * hit.bary.y);
        
        if (dot(normal, ng) < 0)
            normal = -normal;
        if (dot(ng, direction) > 0)
        {
            ng = -ng;
            normal = -normal;
        }
        if (dot(normal, direction) > 0)
            normal = ng;

        float3 albedo = baseColorAt(triangle, material, hit.bary);

        float emissionWeight = 1;
        if (previousUseNee && triangle.info.y != 0xffffffff)
        {
            emissionWeight = powerHeuristic(previousPdf, lightPdf(triangle, previousPosition, position));
        }

        radiance += throughput * material.emission.rgb * emissionWeight;

        if (bounce == params.integrator.x)
            break;
        bool useNee = triangle.info.w != 0 && params.integrator.z != 0;
        if (useNee)
        {
            uint lightIndex = min(uint(randomFloat(rng) * params.integrator.z), params.integrator.z - 1);
            Triangle light = triangles[lights[lightIndex]];
            float r = sqrt(randomFloat(rng));
            float v = randomFloat(rng);
            float3 lightPoint = light.p0.xyz * (1 - r) + light.p1.xyz * r * (1 - v) + light.p2.xyz * r * v;
            float3 wi = normalize(lightPoint - position);
            float cosine = max(0.0, dot(normal, wi));
            float pdf = lightPdf(light, position, lightPoint);
            if (pdf > 0 && cosine > 0 && dot(ng, wi) > 0)
            {
                float3 lightNormal = geometricNormal(light);
                if (dot(lightNormal, -wi) < 0)
                    lightNormal = -lightNormal;
            
                float3 shadowOrigin = offsetRay(position, ng);
                float3 shadowTarget = offsetRay(lightPoint, lightNormal);
                float3 shadowDelta = shadowTarget - shadowOrigin;
                float distance = length(shadowDelta);
                Hit shadowHit;
                if (distance > 0 && !traceScene(shadowOrigin, shadowDelta / distance, distance - 1e-4, true, shadowHit))
                {
                    float weight = powerHeuristic(pdf, cosine / kPi);
                    radiance += throughput * albedo / kPi * materials[light.info.x].emission.rgb * weight * cosine / pdf;
                }
            }
        }

        if (max(albedo.r, max(albedo.g, albedo.b)) <= 0)
            break;

        direction = cosineHemisphere(normal, rng);
        if (dot(direction, ng) < 0)
            break;

        throughput *= albedo;
        previousPdf = max(0.0, dot(normal, direction)) / kPi;
        previousPosition = position;
        previousUseNee = useNee;
        origin = offsetRay(position, ng);

        if (bounce >= 3)
        {
            float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (randomFloat(rng) > survival)
                break;
            throughput /= survival;
        }
    }
    return radiance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void traceMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.image.x || tid.y >= params.image.y)
        return;

    uint pixel = tid.y * params.image.x + tid.x;
    float4 sum = params.image.z == 0 ? float4(0) : film[pixel];
    for (uint i = 0; i < params.image.z; ++i)
    {
        uint sampleIndex = params.image.z + i;
        uint rng = hash32(pixel ^ hash32(sampleIndex ^ hash32(params.integrator.y)));
        float2 jitter = float2(randomFloat(rng), randomFloat(rng));
        float2 xy = (float2(tid.xy) + jitter) / float2(params.image.xy) * 2 - 1;
        float3 direction = normalize(params.forward.xyz + 
                                    params.right.xyz * (xy.x * params.forward.w * params.eye.w) -
                                    params.up.xyz * (xy.y * params.eye.w)); 

        sum += float4(tracePath(params.eye.xyz, direction, rng), 1);
    }
    film[pixel] = sum;
}

float3 linearToSrgb(float3 color)
{
    return select(color <= 0.0031308, color * 12.92, pow(color, 1 / 2.4) * 1.055 - 0.055);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void tonemapMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.image.x || tid.y >= params.image.y)
        return;

    uint pixel = tid.y * params.image.x + tid.x;
    float4 sum = film[pixel];
    float3 color = max(sum.rgb / sum.w * exp2(params.environment.w), 0);
    color = color / (color + 1);
    display[pixel] = float4(linearToSrgb(color), 1);
}