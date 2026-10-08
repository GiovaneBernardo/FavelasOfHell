// Editor ground grid: ray-plane intersection with y = 0, occluded by scene depth.
#include "common/view.slangh"
#include "common/fullscreen.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
Texture2D<float> g_depth : register(t0);

float gridLine(float2 coord, float cell)
{
    float2 c = coord / cell;
    float2 w = fwidth(c);
    float2 g = abs(frac(c - 0.5) - 0.5) / max(w, 1e-5);
    return 1.0 - min(min(g.x, g.y), 1.0);
}

[shader("fragment")]
float4 psGrid(FullscreenOut i) : SV_Target0
{
    float3 o = g_view.cameraPos.xyz;
    float3 nearP = worldPointAt(g_view.invViewProj, i.ndc, 1.0);
    float3 d = normalize(nearP - o);
    if (abs(d.y) < 1e-5) discard;
    float t = -o.y / d.y;
    if (t <= 0.0) discard;

    float sceneDepth = g_depth.Load(int3(int2(i.position.xy), 0));
    if (sceneDepth > 0.0) {
        float3 sceneP = worldPointAt(g_view.invViewProj, i.ndc, sceneDepth);
        if (length(sceneP - o) < t) discard;
    }

    float3 hit = o + d * t;
    float fade = 1.0 - saturate(t / 250.0);
    float minor = gridLine(hit.xz, 1.0) * 0.25;
    float major = gridLine(hit.xz, 10.0) * 0.55;
    float alpha = max(minor, major);

    float3 color = float3(0.75, 0.75, 0.78);
    float axisWidth = max(fwidth(hit.x), fwidth(hit.z)) * 1.5;
    if (abs(hit.x) < axisWidth) { color = float3(0.35, 0.55, 1.0); alpha = 1.0; }  // Z axis (x = 0)
    if (abs(hit.z) < axisWidth) { color = float3(1.0, 0.35, 0.35); alpha = 1.0; }  // X axis (z = 0)

    return float4(color, alpha * fade);
}
