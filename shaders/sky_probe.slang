// Sky probe for the water: the sky above the camera (atmosphere + clouds, no sun disc) in a small hemispherical
// octahedral map, rendered every frame. A reflected ray that leaves the screen without hitting anything reads this
// instead of the real sky, so the sea mirrors clouds that are behind the camera or outside the view.
//
// Layout (see octahedral helpers in water.slang): uv = (x + z, x - z) / (|x| + y + |z|) * 0.5 + 0.5, y >= 0.
// The map is a function of direction from the camera; the water shader corrects the parallax of the cloud layer.
//
// Same bindings as the cloud trace (b0 view, b1 clouds, t1..t3 atmosphere, t4..t6 noise, s0, s1); t0 is unused.
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/clouds.slangh"
#include "common/water.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
ConstantBuffer<CloudConstants> g_cloud : register(b1);
Texture2D<float> g_unusedDepth : register(t0);
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

float3 octahedralDecode(float2 uv)
{
    float2 q = uv * 2.0 - 1.0;
    float2 p = float2(q.x + q.y, q.x - q.y) * 0.5; // (x, z)
    float y = max(1.0 - abs(p.x) - abs(p.y), 0.0);
    return normalize(float3(p.x, y, p.y));
}

float3 gradientSky(float3 dir)
{
    float3 horizon = float3(0.70, 0.80, 0.92);
    float3 zenith = float3(0.12, 0.30, 0.68);
    return lerp(horizon, zenith, pow(saturate(dir.y), 0.45));
}

[shader("fragment")]
float4 psSkyProbe(FullscreenOut i) : SV_Target0
{
    float2 uv = i.position.xy / kWaterProbeSize;
    float3 dir = octahedralDecode(uv);
    // Slightly above the horizon: the sea reflects the horizon haze, not the planet's far side.
    dir = normalize(float3(dir.x, max(dir.y, 0.003), dir.z));

    float3 radiance;
    if (g_view.atmosphere1.y < 0.5) {
        radiance = gradientSky(dir);
    } else {
        AtmoRt atmo = atmoFromView(g_view);
        float3 sunDir = -g_view.sunDirection.xyz;
        float3 camera = atmoPosition(atmo, g_view.cameraPos.xyz, g_view.cameraPos.xyz);
        float3 transmittance;
        radiance = atmoSkyRadiance(atmo, camera, dir, sunDir, transmittance) * g_view.sunColor.rgb;
    }

    if (g_cloud.layer.z > 0.5) {
        // Fixed jitter: the probe has no temporal accumulation, the water's roughness blurs the pattern anyway.
        float4 c = traceClouds(i.position.xy, 0.0, g_view.cameraPos.xyz, dir);
        radiance = radiance * c.a + c.rgb;
    }
    return float4(radiance, 1.0);
}
