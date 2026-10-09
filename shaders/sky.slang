// Sky background from the precomputed atmosphere (Bruneton): one lookup per pixel, no ray marching. Only
// pixels without geometry are shaded (the depth buffer is read first), so the cost scales with the visible
// sky, not with the screen. Without atmosphere tables (not generated yet) or without a sun: a plain gradient.
#include "common/view.slangh"
#include "common/fullscreen.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
Texture2D<float> g_depth : register(t0);
Texture2D<float4> g_atmoTransmittance : register(t1);
Texture3D<float4> g_atmoScattering : register(t2);
Texture2D<float4> g_atmoIrradiance : register(t3);
SamplerState g_atmoSampler : register(s0);

#include "common/atmosphere.slangh"

float3 fallbackSky(float3 dir)
{
    float3 horizon = float3(0.70, 0.80, 0.92);
    float3 zenith = float3(0.12, 0.30, 0.68);
    float3 ground = float3(0.16, 0.16, 0.17);
    float up = dir.y;
    float3 sky = lerp(horizon, zenith, pow(saturate(up), 0.45));
    sky = lerp(sky, ground, smoothstep(0.0, -0.25, up));
    float sunAmount = saturate(dot(dir, -g_view.sunDirection.xyz)) * g_view.sunDirection.w;
    sky += g_view.sunColor.rgb * (pow(sunAmount, 900.0) * 3.0 + pow(sunAmount, 12.0) * 0.08);
    return sky;
}

[shader("fragment")]
float4 psSky(FullscreenOut i) : SV_Target0
{
    if (g_depth.Load(int3(int2(i.position.xy), 0)) > 0.0) discard; // geometry: the lighting pass shades it

    float3 dir = normalize(worldPointAt(g_view.invViewProj, i.ndc, 1.0) - g_view.cameraPos.xyz);
    if (g_view.atmosphere1.y < 0.5) return float4(fallbackSky(dir), 1.0);

    AtmoRt atmo = atmoFromView(g_view);
    float3 sunDir = -g_view.sunDirection.xyz;
    float3 sunIrradiance = g_view.sunColor.rgb;
    float3 camera = atmoPosition(atmo, g_view.cameraPos.xyz, g_view.cameraPos.xyz);
    float r = length(camera);
    float mu = dot(camera, dir) / r;

    float3 transmittance;
    float3 radiance = atmoSkyRadiance(atmo, camera, dir, sunDir, transmittance);

    if (rayIntersectsGround(atmo.geom, r, mu)) {
        // Below the horizon with no terrain drawn: the planet surface, lit by sun and sky and veiled by haze.
        float d = distanceToBottom(atmo.geom, r, mu);
        float3 p = camera + dir * d;
        float3 up = normalize(p);
        float3 transmittanceToGround;
        float3 inscatter = atmoSkyRadianceToPoint(atmo, camera, p, sunDir, transmittanceToGround);
        float muS = dot(up, sunDir);
        float3 irradiance = atmoTransmittanceToSun(atmo, atmo.geom.bottom, muS) * max(muS, 0.0) +
                            atmoSkyIrradiance(atmo, atmo.geom.bottom, muS);
        float3 ground = max(atmo.groundAlbedo, float3(0.25)) * (1.0 / 3.14159265) * irradiance * sunIrradiance * transmittanceToGround +
                        inscatter * sunIrradiance;
        // Far ground disappears into the haze: towards the horizon fade to the horizon sky instead of showing the
        // planet's dark surface, so the seam where a finite or far-clipped ground plane ends is not visible.
        float3 horizonDir = normalize(float3(dir.x, 0.0, dir.z));
        float3 horizonTransmittance;
        float3 horizon = atmoSkyRadiance(atmo, camera, horizonDir, sunDir, horizonTransmittance) * sunIrradiance;
        radiance = lerp(horizon, ground, smoothstep(0.0, 0.5, -dir.y));
    } else {
        radiance *= sunIrradiance;
        // The sun disc, dimmed by the air it shines through (reddens towards the horizon).
        float cosSun = dot(dir, sunDir);
        float cosRadius = cos(atmo.geom.sunAngularRadius);
        float edge = smoothstep(cosRadius - (1.0 - cosRadius) * 0.3, cosRadius, cosSun);
        float3 sunRadiance = sunIrradiance / (3.14159265 * atmo.geom.sunAngularRadius * atmo.geom.sunAngularRadius);
        radiance += transmittance * min(sunRadiance, float3(60000.0)) * edge * g_view.sunDirection.w;
    }
    return float4(radiance, 1.0);
}
