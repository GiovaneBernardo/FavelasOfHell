// Deferred lighting: reads the G-buffer + depth, lights every covered pixel with the
// sun (GGX specular, Lambert diffuse, cascaded shadow map) plus ambient, then veils it with aerial perspective.
// With the precomputed atmosphere (common/atmosphere.slangh) the sun is attenuated by the air it crosses, the
// ambient comes from the sky irradiance table and distant surfaces pick up in-scattered light; without it
// (tables not generated, or no sun) the old hemisphere ambient is used.
// Sky pixels (depth 0 in reverse-Z) are discarded: the sky pass already drew them.
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/gbuffer.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
Texture2D<float4> g_albedo : register(t0);
Texture2D<float2> g_normal : register(t1);
Texture2D<float4> g_material : register(t2);
Texture2D<float> g_depth : register(t3);
Texture2D<float> g_shadowAtlas : register(t4);
SamplerComparisonState g_shadowSampler : register(s0);
Texture2D<float4> g_atmoTransmittance : register(t5);
Texture3D<float4> g_atmoScattering : register(t6);
Texture2D<float4> g_atmoIrradiance : register(t7);
SamplerState g_atmoSampler : register(s1);

#include "common/atmosphere.slangh"

static const float kPi = 3.14159265;

float distributionGGX(float nh, float a)
{
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility (includes the 1 / (4 n.l n.v) term).
float visibilitySmith(float nl, float nv, float a)
{
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// 1 = fully lit. The cascade is picked by view depth; the receiver is moved toward the
// light and along its normal by a few shadow texels (the usual acne / peter-panning trade).
float sunShadow(float3 worldPos, float3 n, float3 toSun, float nl)
{
    if (g_view.shadowParams.w < 0.5) return 1.0;
    float viewDepth = -mul(g_view.view, float4(worldPos, 1.0)).z;
    int cascade = 0;
    [unroll] for (int c = 0; c < 3; ++c) { if (viewDepth > g_view.cascadeSplits[c]) cascade = c + 1; }
    float lastSplit = g_view.cascadeSplits.w;
    if (viewDepth >= lastSplit) return 1.0;

    float texel = g_view.cascadeTexel[cascade];
    float3 p = worldPos + n * (texel * (1.0 + 2.0 * (1.0 - nl))) + toSun * (texel * 2.0);
    float4 clip = mul(g_view.cascadeViewProj[cascade], float4(p, 1.0));

    float tile = g_view.shadowParams.y;
    float2 origin = float2(cascade & 1, cascade >> 1) * tile;
    float2 uv = clip.xy * float2(0.5, -0.5) + 0.5;
    // Keep the whole 3x3 kernel (plus the bilinear footprint) inside the cascade's tile.
    float margin = 1.5 * g_view.shadowParams.x;
    uv = clamp(uv * tile, margin, tile - margin) + origin;

    float occluded = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            occluded += g_shadowAtlas.SampleCmpLevelZero(g_shadowSampler, uv + float2(x, y) * g_view.shadowParams.x, clip.z);
        }
    }
    float lit = 1.0 - occluded / 9.0;
    // Fade the last 10% of the range out instead of a hard edge.
    return lerp(1.0, lit, saturate((lastSplit - viewDepth) / (0.1 * lastSplit)));
}

[shader("fragment")]
float4 psDeferred(FullscreenOut i) : SV_Target0
{
    int3 px = int3(int2(i.position.xy), 0);
    float depth = g_depth.Load(px);
    if (depth <= 0.0) discard;

    float3 albedo = g_albedo.Load(px).rgb;
    float3 n = octDecode(g_normal.Load(px));
    float3 material = g_material.Load(px).rgb;
    float roughness = max(material.r, 0.045);
    float metallic = material.g;
    float ao = material.b;

    float3 worldPos = worldPointAt(g_view.invViewProj, i.ndc, depth);
    float3 v = normalize(g_view.cameraPos.xyz - worldPos);
    float3 toSun = -g_view.sunDirection.xyz;
    float nl = saturate(dot(n, toSun)) * g_view.sunDirection.w;

    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 diffuseColor = albedo * (1.0 - metallic);

    bool atmosphereOn = g_view.atmosphere1.y > 0.5;
    AtmoRt atmo = atmoFromView(g_view);
    float3 sunDir = -g_view.sunDirection.xyz;
    float3 sunRadiance = g_view.sunColor.rgb; // irradiance on a surface facing the sun
    float3 pointKm = atmoPosition(atmo, worldPos, g_view.cameraPos.xyz);
    float pointR = length(pointKm);
    float muS = dot(pointKm, sunDir) / pointR;
    float3 skyIrradiance = float3(0.0);
    if (atmosphereOn) {
        sunRadiance *= atmoTransmittanceToSun(atmo, pointR, muS);
        // Sky dome light, scaled by how much of the dome the normal sees; the ground bounce fills the lower half.
        float3 sky = atmoSkyIrradiance(atmo, pointR, muS) * g_view.sunColor.rgb;
        float3 bounce = atmo.groundAlbedo * (sunRadiance * max(muS, 0.0) + sky) * (1.0 / kPi);
        skyIrradiance = sky * (n.y * 0.5 + 0.5) + bounce * (0.5 - n.y * 0.5) * kPi;
    }

    float3 color = 0.0;
    if (nl > 0.0) {
        float3 h = normalize(v + toSun);
        float nv = max(dot(n, v), 1e-4);
        float nh = saturate(dot(n, h));
        float vh = saturate(dot(v, h));
        float a = roughness * roughness;
        float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
        float3 specular = distributionGGX(nh, a) * visibilitySmith(nl, nv, a) * fresnel;
        float3 diffuse = diffuseColor * (1.0 - fresnel) / kPi;
        color += (diffuse + specular) * sunRadiance * nl * sunShadow(worldPos, n, toSun, nl);
    }

    if (atmosphereOn) {
        color += diffuseColor * skyIrradiance * (1.0 / kPi) * ao;
        // A faint floor so moonless nights are dark but not pitch black.
        color += diffuseColor * float3(0.002, 0.0025, 0.0045) * ao;

        float3 cameraKm = atmoPosition(atmo, g_view.cameraPos.xyz, g_view.cameraPos.xyz);
        float3 transmittance;
        float3 inscatter = atmoSkyRadianceToPoint(atmo, cameraKm, pointKm, sunDir, transmittance);
        color = color * transmittance + inscatter * g_view.sunColor.rgb;
    } else {
        float3 ambient = lerp(float3(0.10, 0.09, 0.08), float3(0.30, 0.38, 0.52), n.y * 0.5 + 0.5);
        color += diffuseColor * ambient * ao;
    }
    return float4(color, 1.0);
}
