// Opaque geometry: fills the G-buffer (psOpaque / psOpaqueMasked) and the picking id target (psPick / psPickMasked).
//
// Material model: glTF metallic-roughness. Base color = texture * material factor * instance tint; the metal/roughness
// texture packs roughness in G and metallic in B (and, by convention, ambient occlusion in R of the occlusion texture,
// which is usually the same texture); normal maps are BC5 (xy stored, z reconstructed).
#include "common/view.slangh"
#include "common/gbuffer.slangh"
#include "common/material.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT; // xyz + bitangent sign (snorm bytes in the vertex buffer)
    // Per-instance: rows 0..2 of the world matrix (3x4), the tint color, the entity id and the material index.
    float4 row0 : INSTANCE0;
    float4 row1 : INSTANCE1;
    float4 row2 : INSTANCE2;
    float4 color : INSTANCE3;
    uint2 entity : ENTITY; // Entity::id (lo, hi), used by the picking pass
    uint material : MATERIAL;
};

struct VSOut {
    float4 position : SV_Position;
    float3 worldNormal : TEXCOORD0;
    float4 worldTangent : TEXCOORD1; // xyz + sign
    float2 uv : TEXCOORD2;
    float4 color : COLOR0;
    nointerpolation uint material : TEXCOORD3;
    nointerpolation uint2 entity : TEXCOORD4;
};

float3 rotateByRows(VSIn i, float3 v)
{
    return float3(dot(i.row0.xyz, v), dot(i.row1.xyz, v), dot(i.row2.xyz, v));
}

[shader("vertex")]
VSOut vsOpaque(VSIn i)
{
    float4 p = float4(i.position, 1.0);
    float3 world = float3(dot(i.row0, p), dot(i.row1, p), dot(i.row2, p));

    VSOut o;
    o.position = mul(g_view.viewProj, float4(world, 1.0));
    // Normals and tangents use the 3x3 part as is: exact for uniform scale (the inverse-transpose for
    // non-uniform scale is not applied; keep instance scale uniform or accept slightly skewed lighting).
    o.worldNormal = rotateByRows(i, i.normal);
    o.worldTangent = float4(rotateByRows(i, i.tangent.xyz), i.tangent.w);
    o.uv = i.uv;
    o.color = i.color;
    o.material = i.material;
    o.entity = i.entity;
    return o;
}

GBufferOut shade(VSOut i, bool frontFace, bool masked)
{
    GpuMaterial m = g_materials[i.material];

    float4 baseTexel = sampleTexture(m.baseColorTex, i.uv);
    float alpha = m.baseColor.a * baseTexel.a;
    if (masked && alpha < m.alphaCutoff) discard;

    float3 n = normalize(i.worldNormal);
    if (!frontFace) n = -n; // double-sided materials: shade the back face as a front face
    if ((m.flags & kMaterialNormalMap) != 0) {
        float2 xy = (sampleTexture(m.normalTex, i.uv).xy * 2.0 - 1.0) * m.normalScale;
        float3 t = normalize(i.worldTangent.xyz - n * dot(n, i.worldTangent.xyz)); // re-orthogonalize after interpolation
        float3 b = cross(n, t) * i.worldTangent.w * (frontFace ? 1.0 : -1.0);
        float z = sqrt(saturate(1.0 - dot(xy, xy)));
        n = normalize(t * xy.x + b * xy.y + n * z);
    }

    // glTF packs roughness in G and metallic in B; occlusion is the R channel of its own texture (often the same one).
    float3 mr = sampleTexture(m.metalRoughTex, i.uv).rgb;
    float ao = m.occlusionTex == m.metalRoughTex ? mr.r : sampleTexture(m.occlusionTex, i.uv).r;
    ao = lerp(1.0, ao, m.occlusionStrength);

    GBufferOut o;
    o.albedo = float4(baseTexel.rgb * m.baseColor.rgb * i.color.rgb, 1.0);
    o.normal = octEncode(n);
    o.material = float4(mr.g * m.roughness, mr.b * m.metallic, ao, 0.0);
    return o;
}

[shader("fragment")]
GBufferOut psOpaque(VSOut i, bool frontFace : SV_IsFrontFace)
{
    return shade(i, frontFace, false);
}

[shader("fragment")]
GBufferOut psOpaqueMasked(VSOut i, bool frontFace : SV_IsFrontFace)
{
    return shade(i, frontFace, true);
}

// Picking: writes the entity id instead of a color (only rendered on a click frame).
[shader("fragment")]
uint2 psPick(VSOut i) : SV_Target0
{
    return i.entity;
}

[shader("fragment")]
uint2 psPickMasked(VSOut i) : SV_Target0
{
    GpuMaterial m = g_materials[i.material];
    if (materialAlpha(m, i.uv) < m.alphaCutoff) discard;
    return i.entity;
}
