// Forward-lit instanced meshes. Placeholder lighting (sun + hemisphere ambient)
// until the PBR deferred path lands.
#include "common/view.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    // Per-instance: rows 0..2 of the world matrix (3x4) and the base color.
    float4 row0 : INSTANCE0;
    float4 row1 : INSTANCE1;
    float4 row2 : INSTANCE2;
    float4 color : INSTANCE3;
};

struct VSOut {
    float4 position : SV_Position;
    float3 worldNormal : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOut vsMesh(VSIn i)
{
    float4 p = float4(i.position, 1.0);
    float3 world = float3(dot(i.row0, p), dot(i.row1, p), dot(i.row2, p));
    // Uniform scale assumed (inverse-transpose comes with the material system).
    float3 n = float3(dot(i.row0.xyz, i.normal), dot(i.row1.xyz, i.normal), dot(i.row2.xyz, i.normal));

    VSOut o;
    o.position = mul(g_view.viewProj, float4(world, 1.0));
    o.worldNormal = n;
    o.color = i.color;
    return o;
}

[shader("fragment")]
float4 psMesh(VSOut i) : SV_Target0
{
    float3 n = normalize(i.worldNormal);
    float3 toSun = -g_view.sunDirection.xyz;
    float ndl = saturate(dot(n, toSun)) * g_view.sunDirection.w;
    float3 ambient = lerp(float3(0.10, 0.09, 0.08), float3(0.30, 0.38, 0.52), n.y * 0.5 + 0.5);
    // Lambert: radiance = albedo / pi * irradiance (sun intensity is irradiance in this unit).
    float3 albedo = i.color.rgb;
    return float4(albedo * (g_view.sunColor.rgb * ndl * 0.3183099 + ambient), 1.0);
}
