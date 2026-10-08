// One shadow cascade: depth-only vertex shader; b0 holds just the cascade's view-projection.
// Alpha-masked materials add a pixel shader that discards texels under the cutoff.
#include "common/instance.slangh"
#include "common/material.slangh"

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
}
