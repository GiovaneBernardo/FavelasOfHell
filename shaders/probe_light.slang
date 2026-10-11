// Lights the faces of the light-probe capture. The G-buffer atlas holds 6 faces side by side for each probe being
// refreshed this frame (face f of probe j at tile (f, j)); this pass shades it like the deferred pass does
// (common/lighting.slangh, without the screen-space terms and the specular reflection) and fills the empty pixels with
// the sky above the horizon and a ground-coloured haze below it. The result feeds the SH projection (probe_sh.slang).
//
// Bindings: layoutDeferred (b0 = the VIEWER's view constants: the shadow cascades, sun and atmosphere are shared) plus
// b1 = ProbeConstants. t0..t3 are the capture's G-buffer instead of the camera's.
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/gbuffer.slangh"

Texture2D<float4> g_albedo : register(t0);
Texture2D<float2> g_normal : register(t1);
Texture2D<float4> g_material : register(t2);
Texture2D<float> g_depth : register(t3);

#include "common/lighting.slangh"
#include "common/probe.slangh"

ConstantBuffer<ProbeConstants> g_probes : register(b1);

float3 groundRadiance(float3 dir)
{
    // Below the horizon with nothing in the way: the sea or the far ground. Lit like a grey diffuse floor.
    float3 sunDir = -g_view.sunDirection.xyz;
    float3 albedo = g_view.atmosphere3.xyz;
    if (g_view.atmosphere1.y > 0.5) {
        AtmoRt atmo = atmoFromView(g_view);
        float3 camera = float3(0.0, atmo.geom.bottom + 0.001, 0.0);
        float muS = sunDir.y;
        float3 sun = g_view.sunColor.rgb * atmoTransmittanceToSun(atmo, length(camera), muS);
        float3 sky = atmoSkyIrradiance(atmo, length(camera), muS) * g_view.sunColor.rgb;
        return albedo * (sun * max(muS, 0.0) + sky) * (1.0 / kPi);
    }
    return albedo * 0.4;
}

float3 skyRadianceFrom(float3 origin, float3 dir)
{
    if (g_view.atmosphere1.y < 0.5) return gradientSky(dir);
    AtmoRt atmo = atmoFromView(g_view);
    float3 sunDir = -g_view.sunDirection.xyz;
    float3 camera = atmoPosition(atmo, origin, origin);
    float3 transmittance;
    return atmoSkyRadiance(atmo, camera, dir, sunDir, transmittance) * g_view.sunColor.rgb;
}

[shader("fragment")]
float4 psProbeLight(FullscreenOut i) : SV_Target0
{
    int2 px = int2(i.position.xy);
    int face = px.x / kProbeFace;
    int probe = px.y / kProbeFace;
    int2 local = px - int2(face, probe) * kProbeFace;
    float2 ndc = (float2(local) + 0.5) / float(kProbeFace) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4x4 inv = g_probes.invViewProj[probe * 6 + face];
    float3 origin = g_probes.origin[probe].xyz;
    float depth = g_depth.Load(int3(px, 0));

    if (depth <= 0.0) {
        float3 far = worldPointAt(inv, ndc, 0.5);
        float3 dir = normalize(far - origin);
        float below = smoothstep(0.0, -0.08, dir.y);
        float3 sky = skyRadianceFrom(origin, normalize(float3(dir.x, max(dir.y, 0.003), dir.z)));
        return float4(lerp(sky, groundRadiance(dir), below), 0.0); // alpha 0: no geometry here (probe_sh.slang tests the lower hemisphere)
    }

    Surface s;
    s.albedo = g_albedo.Load(int3(px, 0)).rgb;
    s.n = octDecode(g_normal.Load(int3(px, 0)));
    float4 materialTex = g_material.Load(int3(px, 0));
    s.roughness = materialTex.r;
    s.metallic = materialTex.g;
    s.ao = materialTex.b;
    s.translucency = materialTex.a;
    s.worldPos = worldPointAt(inv, ndc, depth);
    s.cameraPos = origin;
    s.occlusion = 1.0;
    s.contact = 1.0;
    return float4(min(shadeSurface(s, false), 64.0), 1.0);
}
