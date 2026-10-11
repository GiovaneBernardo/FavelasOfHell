// One shadow cascade: depth-only vertex shader; b0 holds just the cascade's view-projection.
// Alpha-masked materials add a pixel shader that discards texels under the cutoff.
#include "common/instance.slangh"
#include "common/material.slangh"
#include "common/foliage.slangh"

struct ShadowConstants {
    float4x4 viewProj;
};
ConstantBuffer<ShadowConstants> g_shadow : register(b0);

[shader("vertex")]
float4 vsShadow(DepthVSIn i) : SV_Position
{
    return mul(g_shadow.viewProj, instanceWorldPosition(i));
}

[shader("vertex")]
MaskedVSOut vsShadowMasked(MaskedVSIn i)
{
    MaskedVSOut o;
    o.position = mul(g_shadow.viewProj, maskedWorldPosition(i));
    o.uv = i.uv;
    o.material = i.material;
    return o;
}

[shader("fragment")]
void psShadowMasked(MaskedVSOut i)
{
    GpuMaterial m = g_materials[i.material];
    if (materialAlpha(m, i.uv) < m.alphaCutoff) discard;
    if ((m.flags & kMaterialFoliage) != 0 && foliageGap(i.uv)) discard;
}

// Foliage transmission atlas (the FoliageTransmit pass): the same casters as the shadow map, drawn only where a foliage
// leaf IS the first thing the sun hits (depth-tested against the shadow atlas, nudged toward the light so a leaf passes
// against its own depth). The pixel holds the colour of the light that leaks through that leaf; the lighting pass reads
// it where the shadow map says "shadowed" and tells a leaf shadow from a wall's.
[shader("vertex")]
MaskedVSOut vsShadowTransmit(MaskedVSIn i)
{
    MaskedVSOut o;
    o.position = mul(g_shadow.viewProj, maskedWorldPosition(i));
    o.position.z += 1e-4 * o.position.w; // reverse-Z: nearer to the light = larger
    o.uv = i.uv;
    o.material = i.material;
    return o;
}

[shader("fragment")]
float4 psShadowTransmit(MaskedVSOut i) : SV_Target0
{
    GpuMaterial m = g_materials[i.material];
    if ((m.flags & kMaterialFoliage) == 0) discard;
    float4 texel = sampleTexture(m.baseColorTex, i.uv);
    if (m.baseColor.a * texel.a < m.alphaCutoff) discard;
    if (foliageGap(i.uv)) discard; // the same texels as the shadow pass dropped
    float3 albedo = texel.rgb * m.baseColor.rgb;
    return float4(leafTransmitColor(albedo, m.translucency), 1.0);
}
