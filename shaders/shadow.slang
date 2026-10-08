// One shadow cascade: depth-only vertex shader; b0 holds just the cascade's view-projection.
#include "common/instance.slangh"

struct ShadowConstants {
    float4x4 viewProj;
};
ConstantBuffer<ShadowConstants> g_shadow : register(b0);

[shader("vertex")]
float4 vsShadow(DepthVSIn i) : SV_Position
{
    return mul(g_shadow.viewProj, instanceWorldPosition(i));
}
