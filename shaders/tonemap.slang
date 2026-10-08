// HDR scene color -> display-referred sRGB (ACES fit), written to an RGBA8 UNORM target.
#include "common/view.slangh"
#include "common/fullscreen.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
Texture2D<float4> g_hdr : register(t0);

float3 acesFit(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 linearToSrgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

[shader("fragment")]
float4 psTonemap(FullscreenOut i) : SV_Target0
{
    float3 hdr = g_hdr.Load(int3(int2(i.position.xy), 0)).rgb * g_view.params.x;
    return float4(linearToSrgb(acesFit(hdr)), 1.0);
}
