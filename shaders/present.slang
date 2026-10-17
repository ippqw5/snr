// Shared window display pass for raster and progressively accumulated path-traced films.
#include "scene_io.h.slang"
ConstantBuffer<Params> params;
ConstantBuffer<PresentParams> presentParams;
RWStructuredBuffer<float4> film;
RWStructuredBuffer<float4> display;
#include "tonemap.h.slang"

// Fullscreen triangle for the windowed present pass. Three vertices at (-1,-1), (3,-1), (-1,3) cover
// the clip-space square with one primitive, so a frame costs one draw and no index buffer.
struct PresentVertex
{
    float4 position : SV_Position;
};

[shader("vertex")]
PresentVertex presentVS(uint vertexId : SV_VertexID)
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    PresentVertex output;
    output.position = float4(corner * 2 - 1, 0, 1);
    return output;
}

// Scale the linear film to fit the window, preserving its aspect ratio with black bars.
float3 filmColor(int2 coordinate)
{
    uint2 p = uint2(clamp(coordinate, int2(0), int2(presentParams.image.xy) - 1));
    float4 sum = film[p.y * presentParams.image.x + p.x];
    return sum.rgb / max(sum.w, 1.0f);
}

[shader("fragment")]
float4 presentFS(PresentVertex input)
{
    float2 filmSize = float2(presentParams.image.xy);
    float2 targetSize = float2(presentParams.image.zw);
    float scale = min(targetSize.x / filmSize.x, targetSize.y / filmSize.y);
    float2 offset = (targetSize - filmSize * scale) * 0.5f;
    float2 position = (input.position.xy - offset) / scale;
    if (any(position < 0) || any(position >= filmSize))
        return float4(0, 0, 0, 1);
    float2 samplePosition = position - 0.5f;
    int2 base = int2(floor(samplePosition));
    float2 weight = frac(samplePosition);
    float3 radiance = lerp(lerp(filmColor(base), filmColor(base + int2(1, 0)), weight.x),
                           lerp(filmColor(base + int2(0, 1)), filmColor(base + int2(1, 1)), weight.x), weight.y);
    float3 color = tonemapCore(radiance, presentParams.config.x);
    return float4(presentParams.config.y != 0 ? color : linearToSrgb(color), 1);
}
