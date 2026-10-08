// Opaque geometry: fills the G-buffer (psOpaque) and the picking id target (psPick).
// Placeholder material: constant roughness/metallic until the material system lands.
#include "common/view.slangh"
#include "common/gbuffer.slangh"

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
    uint2 entity : ENTITY; // Entity::id (lo, hi), used by the picking pass
};

struct VSOut {
    float4 position : SV_Position;
    float3 worldNormal : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint2 entity : TEXCOORD1;
};

[shader("vertex")]
VSOut vsOpaque(VSIn i)
{
    float4 p = float4(i.position, 1.0);
    float3 world = float3(dot(i.row0, p), dot(i.row1, p), dot(i.row2, p));
    // Uniform scale assumed (inverse-transpose comes with the material system).
    float3 n = float3(dot(i.row0.xyz, i.normal), dot(i.row1.xyz, i.normal), dot(i.row2.xyz, i.normal));

    VSOut o;
    o.position = mul(g_view.viewProj, float4(world, 1.0));
    o.worldNormal = n;
    o.color = i.color;
    o.entity = i.entity;
    return o;
}

[shader("fragment")]
GBufferOut psOpaque(VSOut i)
{
    GBufferOut o;
    o.albedo = float4(i.color.rgb, 1.0);
    o.normal = octEncode(normalize(i.worldNormal));
    o.material = float4(0.8, 0.0, 1.0, 0.0);
    return o;
}

// Picking: writes the entity id instead of a color (only rendered on a click frame).
[shader("fragment")]
uint2 psPick(VSOut i) : SV_Target0
{
    return i.entity;
}
