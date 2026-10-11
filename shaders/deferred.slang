// Deferred lighting: reads the G-buffer + depth and lights every covered pixel (common/lighting.slangh): the sun with
// GGX specular, the cascaded shadow map, leaf transmission and screen-space contact shadows, then ambient light from the
// probe grid / the atmosphere, the sky reflection, GTAO, and the aerial perspective veil.
// Sky pixels (depth 0 in reverse-Z) are discarded: the sky pass already drew them.
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/gbuffer.slangh"

Texture2D<float4> g_albedo : register(t0);
Texture2D<float2> g_normal : register(t1);
Texture2D<float4> g_material : register(t2);
Texture2D<float> g_depth : register(t3);
Texture2D<float> g_gtao : register(t8);

#include "common/lighting.slangh"

float linearDepth(float depth)
{
    float n = g_view.params.z, f = g_view.params.w;
    return f * n / (n + depth * (f - n));
}

float interleavedGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Screen-space shadow toward the sun over the first `ssao.z` metres: what the cascades are too coarse to resolve
// (grass blades, bark ridges, the crease under a ledge). The ray is marched in the depth buffer; a hit needs the
// ray to be just behind the stored surface, so distant geometry in front does not shadow by accident.
float contactShadow(int2 px, float3 worldPos, float3 toSun, float jitter)
{
    float len = g_view.ssao.z;
    if (len <= 0.0) return 1.0;
    float4 c0 = mul(g_view.viewProj, float4(worldPos, 1.0));
    float4 c1 = mul(g_view.viewProj, float4(worldPos + toSun * len, 1.0));
    if (c0.w <= 0.0 || c1.w <= 0.0) return 1.0;
    float2 size = g_view.viewport.xy;
    float2 s0 = (c0.xy / c0.w * float2(0.5, -0.5) + 0.5) * size;
    float2 s1 = (c1.xy / c1.w * float2(0.5, -0.5) + 0.5) * size;
    float w0 = c0.w, w1 = c1.w; // view depth
    float hit = 0.0;
    const int kSteps = 12;
    [unroll] for (int i = 0; i < kSteps; ++i) {
        float t = (float(i) + 0.5 + jitter) / float(kSteps);
        float2 sp = lerp(s0, s1, t);
        if (any(sp < 0.0) || any(sp >= size)) break;
        float rayDepth = 1.0 / lerp(1.0 / w0, 1.0 / w1, t); // 1/w is affine in screen space
        float d = g_depth.Load(int3(int2(sp), 0));
        if (d <= 0.0) continue;
        float sceneDepth = linearDepth(d);
        float diff = rayDepth - sceneDepth;
        // Behind the surface by a little: occluded. A deep difference is a wall far in front of the ray: ignore.
        float thickness = 0.12 + 0.02 * sceneDepth;
        if (diff > 0.01 + 0.004 * sceneDepth && diff < thickness) hit = 1.0;
    }
    // Fade with distance: far away a pixel is metres wide and the result is just noise.
    float fade = 1.0 - saturate((w0 - 30.0) / 30.0);
    return 1.0 - hit * fade;
}

[shader("fragment")]
float4 psDeferred(FullscreenOut i) : SV_Target0
{
    int3 px = int3(int2(i.position.xy), 0);
    float depth = g_depth.Load(px);
    if (depth <= 0.0) discard;

    Surface s;
    s.albedo = g_albedo.Load(px).rgb;
    s.n = octDecode(g_normal.Load(px));
    float4 materialTex = g_material.Load(px);
    s.roughness = materialTex.r;
    s.metallic = materialTex.g;
    s.ao = materialTex.b;
    s.translucency = materialTex.a; // foliage: light wraps round the leaf and glows through it when backlit
    s.worldPos = worldPointAt(g_view.invViewProj, i.ndc, depth);
    s.cameraPos = g_view.cameraPos.xyz;
    s.occlusion = g_view.ssao.x > 0.5 ? pow(saturate(g_gtao.Load(px)), g_view.ssao.y) : 1.0;
    s.contact = 1.0;
    float3 toSun = -g_view.sunDirection.xyz;
    if (g_view.ssao.z > 0.0 && g_view.sunDirection.w > 0.5 && dot(s.n, toSun) > -0.3)
        s.contact = contactShadow(int2(px.xy), s.worldPos, toSun, interleavedGradientNoise(i.position.xy));

    return float4(shadeSurface(s, true), 1.0);
}
