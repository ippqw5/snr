struct UiVertex
{
    float4 position;
    float4 color;
    float4 uv;
};
StructuredBuffer<UiVertex> vertices;
Texture2D<float4> fontTexture;
SamplerState fontSampler;
struct UiVarying
{
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};
[shader("vertex")]
UiVarying uiVS(uint id : SV_VertexID, uint first : SV_StartVertexLocation)
{
    id += first;
    UiVarying output;
    output.position = vertices[id].position;
    output.color = vertices[id].color;
    output.uv = vertices[id].uv.xy;
    return output;
}
[shader("fragment")]
float4 uiFS(UiVarying input) : SV_Target
{
    return input.color * fontTexture.Sample(fontSampler, input.uv);
}
