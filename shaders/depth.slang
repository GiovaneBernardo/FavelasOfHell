// Main view depth pre-pass. vsDepth is vertex-shader only (no pixel shader); alpha-masked materials
// use vsDepthMasked + psDepthMasked, which discard the texels whose alpha is under the cutoff.
// vsDepth/vsDepthMasked must compute the position exactly like opaque.vsOpaque: the opaque pass
// depth-tests against this pass's result.
#include "common/view.slangh"
#include "common/instance.slangh"
#include "common/material.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);

[shader("vertex")]
float4 vsDepth(DepthVSIn i) : SV_Position
{
    return mul(g_view.viewProj, instanceWorldPosition(i));
}

[shader("vertex")]
MaskedVSOut vsDepthMasked(MaskedVSIn i)
{
    MaskedVSOut o;
    o.position = mul(g_view.viewProj, maskedWorldPosition(i));
    o.uv = i.uv;
    o.material = i.material;
    return o;
}

[shader("fragment")]
void psDepthMasked(MaskedVSOut i)
{
    GpuMaterial m = g_materials[i.material];
    if (materialAlpha(m, i.uv) < m.alphaCutoff) discard;
}
