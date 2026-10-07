import sampling;

struct SurfaceMaterial
{
    float3 baseColor;
    float metallic;
    float roughness;
    bool diffuseOnly;
};

struct BsdfEvaluation
{
    float3 value;
    float  pdf;
};

struct BsdfSample
{
    float3 direction;
    // Throughput multiplier: f * cosine / PDF, or reflectance / selection
    // probability for a delta event. A zero weight represents absorption.
    float3 weight;
    float  pdf;
    bool   delta;
};


float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 fresnelSchlick(float3 f0, float cosine)
{
    float x = 1 - saturate(cosine);
    return f0 + (1 - f0) * (x * x * x * x * x);
}

float3 specularReflectance(SurfaceMaterial material)
{
    return lerp(float3(0.04), material.baseColor, material.metallic);
}

bool isSmooth(SurfaceMaterial material)
{
    // Treat sub-milliperceptual roughness as a delta lobe to avoid an unstable
    // numerical approximation to a mirror. Diffuse reflection remains continuous.
    return material.roughness < 0.001;
}

float specularProbability(SurfaceMaterial material, float cosine)
{
    if (material.diffuseOnly)
        return 0;
    if (material.metallic > 0 || luminance(material.baseColor) == 0)
        return 1;

    float3 fresnel = fresnelSchlick(specularReflectance(material), cosine);
    float specular = luminance(fresnel);
    float diffuse = luminance((1 - fresnel) * material.baseColor) * (1 - material.metallic);
    return clamp(specular / max(specular + diffuse, 1e-8), 0.05, 0.95);
}

float ggxDistribution(float nh, float alpha)
{
    float a2 = alpha * alpha;
    float denominator = (1 - nh) * (1 + nh) + a2 * nh * nh;
    return a2 / (kPi * denominator * denominator);
}

float ggxG1(float nv, float alpha)
{
    return 2 * nv / (nv + sqrt(alpha * alpha + (1 - alpha * alpha) * nv * nv));
}

BsdfEvaluation evaluateBsdf(SurfaceMaterial material, float3 normal, float3 wo, float3 wi)
{
    BsdfEvaluation result = {};
    float nv = dot(normal, wo);
    float nl = dot(normal, wi);
    if (nv <= 0 || nl <= 0) return result;

    if (material.diffuseOnly)
    {
        result.value = material.baseColor / kPi;
        result.pdf = nl / kPi;
        return result;
    }

    float3 halfVector = normalize(wo + wi);
    float nh = saturate(dot(normal, halfVector));
    float vh = saturate(dot(wo, halfVector));
    float3 fresnel = fresnelSchlick(specularReflectance(material), vh);
    float pSpecular = specularProbability(material, nv);
    result.value = (1 - fresnel) * material.baseColor * (1 - material.metallic) / kPi;
    result.pdf = (1 - pSpecular) * nl / kPi;
    if (!isSmooth(material))
    {
        float alpha = material.roughness * material.roughness;
        float a2 = alpha * alpha;
        float distribution = ggxDistribution(nh, alpha);

        // Height-correlated Smith visibility, including the 1 / (4 * N.L * N.V) factor 
        // The VNDF sampling PDF uses G1, not this two-direction term.
        float visibility = 0.5 / (nl * sqrt(a2 + (1 - a2) * nv * nv))
                         + nv * sqrt(a2 + (1 - a2) * nl * nl);
        
        result.value += fresnel * distribution * visibility;
        result.pdf   += pSpecular * distribution * ggxG1(nv, alpha) / (4 * nv);
    }
    return result;
}


float3 sampleGgxNormal(float3 normal, float3 wo, float alpha, inout uint rng)
{
    float3 tangent = normalize(cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float3 view = normalize(float3(alpha * dot(wo, tangent), alpha * dot(wo, bitangent), dot(wo, normal)));

    float lensq = view.x * view.x + view.y * view.y;
    float3 t1 = lensq > 0 ? float3(-view.y, view.x, 0) / sqrt(lensq) : float3(1, 0, 0);
    float3 t2 = cross(view, t1);

    float radius = sqrt(randomFloat(rng));
    float phi    = 2 * kPi * randomFloat(rng);
    float x      = radius * cos(phi);
    float y      = radius * sin(phi);
    float blend  = 0.5 * (1 + view.z);

    y = (1 - blend) * sqrt(max(0.0, 1 - x * x)) + blend * y;
    float3 nh = x * t1 + y * t2 + sqrt(max(0, 1- x * x - y * y)) * view;
    float3 h  = normalize(float3(alpha * nh.xy, max(0.0, nh.z)));
    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

BsdfSample sampleBsdf(SurfaceMaterial material, float3 normal, float3 wo, inout uint rng)
{
    BsdfSample result = {};
    float nv = dot(normal, wo);
    if (nv <= 0) return result;

    if (material.diffuseOnly)
    {
        result.direction = cosineHemisphere(normal, rng);
        result.weight = material.baseColor;
        result.pdf = max(0.0f, dot(normal, result.direction)) / kPi;
        return result;
    }

    float pSpecular = specularProbability(material, nv);
    if (randomFloat(rng) < pSpecular)
    {
        if (isSmooth(material))
        {
            result.direction = reflect(-wo, normal);
            result.weight = fresnelSchlick(specularReflectance(material), nv) / pSpecular;
            result.pdf = pSpecular;
            result.delta = true;
            return result;
        }
        float3 halfVector = sampleGgxNormal(normal, wo, material.roughness * material.roughness, rng);
        result.direction = reflect(-wo, halfVector);
    }
    else
    {
        result.direction = cosineHemisphere(normal, rng);
    }

    BsdfEvaluation eval = evaluateBsdf(material, normal, wo, result.direction);
    result.pdf = eval.pdf;
    if (result.pdf > 0)
        result.weight = eval.value * max(0.0f, dot(normal, result.direction)) / result.pdf;
    return result;
}