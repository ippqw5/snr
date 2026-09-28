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

    float3 weight;
    float  pdf;
    bool   delta;
};