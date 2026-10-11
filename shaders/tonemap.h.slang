// Exposure, tone mapping and sRGB encoding, shared by the path tracer and the raster preview.
//
// Both renderers fill the same Film buffers with linear radiance, so both end at this one entry
// point and the two pipelines cannot drift in how exposure, the display transform or the sRGB
// encode is applied. The file declares no bindings of its own: the including module must declare
// `film`, `display` and `params` (see scene_io.h.slang for Params).

#ifndef SNR_TONEMAP_H
#define SNR_TONEMAP_H

float3 linearToSrgb(float3 color)
{
    return select(color <= 0.0031308, color * 12.92, pow(color, 1 / 2.4) * 1.055 - 0.055);
}

// Exposure, then Reinhard, on an already-averaged linear color. Shared so the buffer write and the
// windowed present pass cannot drift in how a filled film is turned into a displayable value.
float3 tonemapCore(float3 linearColor, float exposure)
{
    float3 color = max(linearColor * exp2(exposure), 0);
    return color / (color + 1);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void tonemapMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.image.x || tid.y >= params.image.y)
        return;

    uint pixel = tid.y * params.image.x + tid.x;
    float4 sum = film[pixel];
    display[pixel] = float4(tonemapCore(sum.rgb / sum.w, params.environment.w), 1);
}

#endif // SNR_TONEMAP_H
