static const float kPi = 3.14159265358979323846;

uint hash32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float randomFloat(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28) + 4)) ^ state) * 277803737u;
    return float((word ^ (word >> 22u)) >> 8u) * (1.0 / 16777216.0);
}

float3 cosineHemisphere(float3 normal, inout uint rng)
{
    float u = randomFloat(rng);
    float phi = 2.0 * kPi * randomFloat(rng);
    float3 tangent = normalize(cross(normal.z < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(sqrt(u) * cos(phi) * tangent + sqrt(u) * sin(phi) * bitangent + sqrt(1 - u) * normal);
}

float powerHeuristic(float a, float b)
{
    float scale = max(a, b);
    if (scale <= 0) return 0;

    a /= scale;
    b /= scale;
    return (a * a) / (a * a + b * b);
}

float3 offsetRay(float3 p, float3 n)
{
    int3 offset = int3(256.0 * n);
    float3 shifted = asfloat(asint(p) + select(p < 0, -offset, offset));
    return select(abs(p) < (1.0 / 32.0), p + n * (1.0 / 65536.0), shifted);
}