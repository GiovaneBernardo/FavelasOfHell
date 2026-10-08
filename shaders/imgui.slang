// Dear ImGui draw lists. Colors are blended in display (sRGB) space, as ImGui expects.

struct ImGuiConstants {
    float2 scale;
    float2 translate;
};

ConstantBuffer<ImGuiConstants> g_constants : register(b0);
Texture2D<float4> g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct VSIn {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOut vsImGui(VSIn i)
{
    VSOut o;
    o.position = float4(i.position * g_constants.scale + g_constants.translate, 0.0, 1.0);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

[shader("fragment")]
float4 psImGui(VSOut i) : SV_Target0
{
    return i.color * g_texture.Sample(g_sampler, i.uv);
}
