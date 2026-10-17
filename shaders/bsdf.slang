import sampling;

struct SurfaceMaterial
{
    float3 baseColor;
    float  alpha;
    float3 emission;
    float  metallic;
    float  roughness;
    float  occlusion;
    bool   diffuseOnly;
    float  transmission;
    float  ior;
    float  eta; // Incident / transmitted IOR, set by the medium tracker for volume boundaries.
    bool   volume;
    float4 attenuation; // RGB attenuation color, distance (0 means infinite).
};

struct BsdfEvaluation
{
    float3 value;
    float  pdf;
};
struct BsdfSample
{
    float3 direction;
    float3 weight; // f * abs(cosine) / full mixture PDF; delta events use their discrete probability.
    float  pdf;
    bool   delta;
    bool   transmitted;
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
float dielectricFresnel(float cosine, float eta)
{
    cosine = saturate(abs(cosine));
    if (abs(eta - 1) < 1e-6)
        return 0;
    float sin2 = eta * eta * max(0.0, 1 - cosine * cosine);
    if (sin2 >= 1)
        return 1; // Total internal reflection.
    float ct = sqrt(1 - sin2);
    float rs = (eta * cosine - ct) / max(eta * cosine + ct, 1e-8);
    float rp = (cosine - eta * ct) / max(cosine + eta * ct, 1e-8);
    return saturate(0.5 * (rs * rs + rp * rp));
}
float3 specularReflectance(SurfaceMaterial m)
{
    float f0 = (m.ior - 1) / (m.ior + 1);
    return lerp(float3(f0 * f0), m.baseColor, m.metallic);
}
float3 materialFresnel(SurfaceMaterial m, float cosine)
{
    return lerp(float3(dielectricFresnel(cosine, m.eta)), fresnelSchlick(m.baseColor, cosine), m.metallic);
}
bool isSmooth(SurfaceMaterial m)
{
    return m.roughness < 0.001;
}
float3 lobeProbabilities(SurfaceMaterial m, float nv)
{
    if (m.diffuseOnly)
        return float3(0, 0, 1);
    float  f = dielectricFresnel(nv, m.eta);
    float  reflectWeight = luminance(materialFresnel(m, nv));
    float  base = max(0.0, luminance(m.baseColor)) * (1 - m.metallic) * (1 - f);
    float3 weights = float3(reflectWeight, base * m.transmission,
                            base * (1 - m.transmission) * m.occlusion);
    return weights / max(dot(weights, float3(1)), 1e-8);
}
float ggxDistribution(float nh, float alpha)
{
    float a2 = alpha * alpha;
    float denominator = (1 - nh) * (1 + nh) + a2 * nh * nh;
    return a2 / max(kPi * denominator * denominator, 1e-20);
}
float ggxG1(float nv, float alpha)
{
    return 2 * nv / max(nv + sqrt(alpha * alpha + (1 - alpha * alpha) * nv * nv), 1e-8);
}
BsdfEvaluation evaluateBsdf(SurfaceMaterial m, float3 n, float3 wo, float3 wi)
{
    BsdfEvaluation result = {};
    float          nv = dot(n, wo), nl = dot(n, wi);
    if (nv <= 0 || abs(nl) < 1e-8)
        return result;
    if (m.diffuseOnly)
    {
        if (nl > 0)
        {
            result.value = m.occlusion * m.baseColor / kPi;
            result.pdf = nl / kPi;
        }
        return result;
    }
    float3 probabilities = lobeProbabilities(m, nv);
    float  alpha = max(1e-4, m.roughness * m.roughness);
    if (nl > 0)
    {
        float3 h = normalize(wo + wi);
        float  nh = saturate(dot(n, h)), vh = saturate(dot(wo, h));
        float3 fresnel = materialFresnel(m, vh);
        result.value = (1 - fresnel) * m.baseColor * m.occlusion * (1 - m.metallic) * (1 - m.transmission) / kPi;
        result.pdf = probabilities.z * nl / kPi;
        if (!isSmooth(m))
        {
            float d = ggxDistribution(nh, alpha);
            // Separable Smith masking matches the visible-normal sampling distribution.
            float g = ggxG1(nv, alpha) * ggxG1(nl, alpha);
            result.value += fresnel * d * g / max(4 * nv * nl, 1e-8);
            result.pdf += probabilities.x * d * ggxG1(nv, alpha) / (4 * nv);
        }
    }
    else if (m.transmission > 0 && m.metallic < 1 && !isSmooth(m))
    {
        if (m.volume && abs(m.eta - 1) < 1e-5)
            return result; // Index-matched transmission is a delta.
        float3 h;
        float  jacobian;
        float  factor;
        float  absNl = -nl;
        if (m.volume)
        {
            h = normalize(m.eta * wo + wi);
            if (dot(n, h) < 0)
                h = -h;
            float vh = dot(wo, h), lh = dot(wi, h);
            if (vh <= 0 || lh >= 0)
                return result;
            float denominator = m.eta * vh + lh;
            float denom2 = max(denominator * denominator, 1e-20);
            jacobian = abs(lh) / denom2;
            // Radiance transport includes eta^2; the entering/exiting factors cancel through a solid.
            factor = abs(vh * lh) * m.eta * m.eta / (nv * absNl * denom2);
        }
        else
        {
            // KHR transmission's thin sheet: reflect the outgoing hemisphere to construct H_T.
            float3 mirrored = wi - 2 * nl * n;
            h = normalize(wo + mirrored);
            float vh = dot(wo, h);
            if (vh <= 0 || dot(wi, h) >= 0)
                return result;
            jacobian = 1 / (4 * vh);
            factor = 1 / (4 * nv * absNl);
        }
        float nh = saturate(dot(n, h)), vh = saturate(dot(wo, h));
        float d = ggxDistribution(nh, alpha);
        float g = ggxG1(nv, alpha) * ggxG1(absNl, alpha);
        float f = dielectricFresnel(vh, m.eta);
        result.value = m.baseColor * (1 - m.metallic) * m.transmission * (1 - f) * d * g * factor;
        result.pdf = probabilities.y * d * ggxG1(nv, alpha) * vh / nv * jacobian;
    }
    return result;
}

float3 sampleGgxNormal(float3 normal, float3 wo, float alpha, inout uint rng)
{
    float3 tangent = normalize(cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float3 view = normalize(float3(alpha * dot(wo, tangent), alpha * dot(wo, bitangent), dot(wo, normal)));

    float  lensq = view.x * view.x + view.y * view.y;
    float3 t1 = lensq > 0 ? float3(-view.y, view.x, 0) / sqrt(lensq) : float3(1, 0, 0);
    float3 t2 = cross(view, t1);

    float radius = sqrt(randomFloat(rng));
    float phi = 2 * kPi * randomFloat(rng);
    float x = radius * cos(phi);
    float y = radius * sin(phi);
    float blend = 0.5 * (1 + view.z);

    y = (1 - blend) * sqrt(max(0.0, 1 - x * x)) + blend * y;
    float3 nh = x * t1 + y * t2 + sqrt(max(0, 1 - x * x - y * y)) * view;
    float3 h = normalize(float3(alpha * nh.xy, max(0.0, nh.z)));
    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

BsdfSample sampleBsdf(SurfaceMaterial m, float3 n, float3 wo, inout uint rng)
{
    BsdfSample result = {};
    float      nv = dot(n, wo);
    if (nv <= 0)
        return result;
    if (m.diffuseOnly)
    {
        result.direction = cosineHemisphere(n, rng);
        result.weight = m.occlusion * m.baseColor;
        result.pdf = max(0.0, dot(n, result.direction)) / kPi;
        return result;
    }
    float3 probabilities = lobeProbabilities(m, nv);
    float  selection = randomFloat(rng);
    bool   reflectLobe = selection < probabilities.x;
    bool   transmitLobe = !reflectLobe && selection < probabilities.x + probabilities.y;
    if (reflectLobe || transmitLobe)
    {
        bool   delta = isSmooth(m) || (transmitLobe && m.volume && abs(m.eta - 1) < 1e-5);
        float3 h = delta ? n : sampleGgxNormal(n, wo, max(1e-4, m.roughness * m.roughness), rng);
        if (reflectLobe)
        {
            result.direction = reflect(-wo, h);
            if (dot(n, result.direction) <= 0)
                return result;
            if (delta)
            {
                result.weight = materialFresnel(m, nv) / max(probabilities.x, 1e-8);
                result.pdf = probabilities.x;
                result.delta = true;
                return result;
            }
        }
        else
        {
            result.transmitted = true;
            if (m.volume)
                result.direction = refract(-wo, h, m.eta);
            else
            {
                float3 reflected = reflect(-wo, h);
                result.direction = reflected - 2 * dot(reflected, n) * n;
            }
            if (dot(n, result.direction) >= 0 || dot(result.direction, result.direction) < 1e-8)
                return result;
            if (delta)
            {
                float scale = m.volume ? m.eta * m.eta : 1;
                result.weight = m.baseColor * (1 - m.metallic) * m.transmission * (1 - dielectricFresnel(nv, m.eta)) * scale / max(probabilities.y, 1e-8);
                result.pdf = probabilities.y;
                result.delta = true;
                return result;
            }
        }
    }
    else
        result.direction = cosineHemisphere(n, rng);
    BsdfEvaluation evaluated = evaluateBsdf(m, n, wo, result.direction);
    result.pdf = evaluated.pdf;
    if (result.pdf > 0)
        result.weight = evaluated.value * abs(dot(n, result.direction)) / result.pdf;
    return result;
}
