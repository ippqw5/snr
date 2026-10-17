import bsdf;

// Track nested homogeneous, closed dielectric volumes. Thin sheets never change the medium.
static const uint kMaxMedia = 16;
struct MediumState
{
    float4 entries[kMaxMedia]; // RGB absorption coefficient, IOR.
    uint   materials[kMaxMedia];
    uint   count;
};
float3 absorptionCoefficient(SurfaceMaterial material)
{
    if (material.attenuation.w <= 0)
        return float3(0);
    float3 sigma = -log(max(material.attenuation.rgb, float3(1e-30))) / material.attenuation.w;
    return select(material.attenuation.rgb <= 0, float3(1e30), sigma);
}
int enclosingIndex(MediumState media, uint material)
{
    for (int i = int(media.count) - 1; i >= 0; --i)
        if (media.materials[i] == material)
            return i;
    return -1;
}
void setMediumEta(inout SurfaceMaterial material, bool frontFace, MediumState media, uint materialId)
{
    if (!material.volume)
    {
        material.eta = 1 / material.ior;
        return;
    }
    float incident = media.count > 0 ? media.entries[media.count - 1].w : 1;
    float outgoing = material.ior;
    if (!frontFace)
    {
        int index = enclosingIndex(media, materialId);
        outgoing = index > 0 ? media.entries[index - 1].w : 1;
        if (media.count == 0)
            incident = material.ior; // Camera initially inside this closed volume.
    }
    material.eta = incident / outgoing;
}
float3 segmentAbsorption(MediumState media, SurfaceMaterial material, bool frontFace)
{
    if (media.count > 0)
        return media.entries[media.count - 1].rgb;
    return material.volume && !frontFace ? absorptionCoefficient(material) : float3(0);
}
bool crossMedium(inout MediumState media, SurfaceMaterial material, bool frontFace, uint materialId)
{
    if (!material.volume)
        return true;
    if (frontFace)
    {
        if (media.count == kMaxMedia)
            return false;
        media.entries[media.count] = float4(absorptionCoefficient(material), material.ior);
        media.materials[media.count++] = materialId;
    }
    else
    {
        int index = enclosingIndex(media, materialId);
        if (index >= 0)
            media.count = uint(index);
    }
    return true;
}

float3 transmittedAbsorption(MediumState media, SurfaceMaterial material, bool frontFace, uint materialId)
{
    if (!material.volume)
        return segmentAbsorption(media, material, frontFace);
    if (frontFace)
        return absorptionCoefficient(material);
    int index = enclosingIndex(media, materialId);
    return index > 0 ? media.entries[index - 1].rgb : float3(0);
}
