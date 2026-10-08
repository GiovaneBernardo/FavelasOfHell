// Procedural sky gradient + sun disc. Placeholder for the Bruneton atmosphere.
#include "common/view.slangh"
#include "common/fullscreen.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);

[shader("fragment")]
float4 psSky(FullscreenOut i) : SV_Target0
{
    float3 dir = normalize(worldPointAt(g_view.invViewProj, i.ndc, 1.0) - g_view.cameraPos.xyz);
    float3 horizon = float3(0.70, 0.80, 0.92);
    float3 zenith = float3(0.12, 0.30, 0.68);
    float3 ground = float3(0.16, 0.16, 0.17);
    float up = dir.y;
    float3 sky = lerp(horizon, zenith, pow(saturate(up), 0.45));
    sky = lerp(sky, ground, smoothstep(0.0, -0.25, up));
    float sunAmount = saturate(dot(dir, -g_view.sunDirection.xyz)) * g_view.sunDirection.w;
    sky += g_view.sunColor.rgb * (pow(sunAmount, 900.0) * 3.0 + pow(sunAmount, 12.0) * 0.08);
    return float4(sky, 1.0);
}
