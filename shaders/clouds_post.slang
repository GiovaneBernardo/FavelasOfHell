// Half-resolution cloud image -> screen.
//   psCloudResolve    temporal accumulation: the trace uses one jittered sample per pixel, so the result is
//                     blended with last frame's (reprojected, clamped to the new neighbourhood to avoid ghosts)
//   psCloudComposite  full-resolution pass over the HDR color: clouds only cover empty (sky) pixels
// Both bind two textures: resolve = (new trace, history), composite = (resolved clouds, scene depth).
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/clouds.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
ConstantBuffer<CloudConstants> g_cloud : register(b1);
Texture2D<float4> g_texA : register(t0);
Texture2D<float4> g_texB : register(t1);
SamplerState g_linear : register(s0);

[shader("fragment")]
float4 psCloudResolve(FullscreenOut i) : SV_Target0
{
    int2 px = int2(i.position.xy);
    uint w, h;
    g_texA.GetDimensions(w, h);
    int2 hi = int2(w, h) - 1;

    float4 current = g_texA.Load(int3(px, 0));
    float4 lo = current, up = current;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float4 n = g_texA.Load(int3(clamp(px + int2(x, y), int2(0, 0), hi), 0));
            lo = min(lo, n);
            up = max(up, n);
        }
    }

    // Where this pixel's cloud was last frame: the point of the middle of the slab along the same ray.
    float3 ro = g_view.cameraPos.xyz;
    float3 dir = normalize(worldPointAt(g_view.invViewProj, i.ndc, 1.0) - ro);
    float mid = 0.5 * (g_cloud.layer.x + g_cloud.layer.y);
    float t = dir.y > 1e-3 || dir.y < -1e-3 ? (mid - ro.y) / dir.y : 8000.0;
    t = t > 0.0 ? clamp(t, 100.0, 40000.0) : 8000.0;
    float4 prev = mul(g_cloud.prevViewProj, float4(ro + dir * t, 1.0));
    float2 uv = prev.xy / prev.w * float2(0.5, -0.5) + 0.5;
    bool onScreen = prev.w > 0.0 && all(uv > 0.0) && all(uv < 1.0);

    float weight = g_cloud.misc.y;
    if (!onScreen) weight = 1.0;
    float4 history = clamp(g_texB.SampleLevel(g_linear, uv, 0), lo, up);
    return lerp(history, current, weight);
}

[shader("fragment")]
float4 psCloudComposite(FullscreenOut i) : SV_Target0
{
    if (g_texB.Load(int3(int2(i.position.xy), 0)).x > 0.0) discard; // geometry in front: no clouds on it
    float4 c = g_texA.SampleLevel(g_linear, i.position.xy * g_view.viewport.zw, 0);
    return float4(c.rgb, 1.0 - c.a); // premultiplied blend: dst * (1 - coverage) + rgb
}
