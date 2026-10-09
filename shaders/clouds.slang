// Volumetric clouds: a ray march through a horizontal slab, at HALF resolution and one jittered sample per
// pixel (clouds_post.slang accumulates the jitter over time and composites). The result is premultiplied:
// rgb = radiance the clouds add, a = transmittance (1 = clear sky).
//
// What keeps it cheap:
//  - pixels whose 2x2 full-resolution block is all geometry exit at once, so only the visible sky is marched;
//  - the ray only covers the slab (a handful of metres looking up, capped at the max distance when grazing);
//  - the weather map is read first: where it says "no cloud" the 3D noise is never fetched;
//  - the detail noise and the light march (5 cheap taps toward the sun) only run inside actual cloud.
//
// Lighting: the sun is attenuated by the Bruneton transmittance table at the cloud layer, scattered with a
// dual-lobe Henyey-Greenstein phase and three octaves of "multiple scattering" (Hillaire), and shadowed by the
// light march. The ambient comes from the sky irradiance table (and the ground bounce for the dark underside).
// The result is veiled by the aerial perspective table at the cloud's mean distance.
// The march itself lives in common/clouds_march.slangh (the water's sky probe uses it too).
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/clouds.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
ConstantBuffer<CloudConstants> g_cloud : register(b1);
Texture2D<float> g_depth : register(t0);
Texture2D<float4> g_atmoTransmittance : register(t1);
Texture3D<float4> g_atmoScattering : register(t2);
Texture2D<float4> g_atmoIrradiance : register(t3);
Texture3D<float4> g_shape : register(t4);
Texture3D<float4> g_detail : register(t5);
Texture2D<float4> g_weather : register(t6);
SamplerState g_atmoSampler : register(s0);
SamplerState g_wrap : register(s1);

#include "common/atmosphere.slangh"
#include "common/clouds_march.slangh"

[shader("fragment")]
float4 psClouds(FullscreenOut i) : SV_Target0
{
    int2 px = int2(i.position.xy);

    // Geometry covers the whole block: nothing to draw here.
    int2 full = px * 2;
    if (g_depth.Load(int3(full, 0)) > 0.0 && g_depth.Load(int3(full + int2(1, 0), 0)) > 0.0 &&
        g_depth.Load(int3(full + int2(0, 1), 0)) > 0.0 && g_depth.Load(int3(full + int2(1, 1), 0)) > 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 ro = g_view.cameraPos.xyz;
    float3 dir = normalize(worldPointAt(g_view.invViewProj, i.ndc, 1.0) - ro);
    return traceClouds(float2(px), g_cloud.misc.x, ro, dir);
}
