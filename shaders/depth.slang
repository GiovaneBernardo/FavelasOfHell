// Main view depth pre-pass: vertex shader only, no pixel shader.
// vsDepth must compute the position exactly like opaque.vsOpaque: the opaque pass
// depth-tests against this pass's result.
#include "common/view.slangh"
#include "common/instance.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);

[shader("vertex")]
float4 vsDepth(DepthVSIn i) : SV_Position
{
    return mul(g_view.viewProj, instanceWorldPosition(i));
}
