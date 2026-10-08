// 1:1 copy of a texture onto the target (players without UI present with this).
#include "common/fullscreen.slangh"

Texture2D<float4> g_source : register(t0);

[shader("fragment")]
float4 psBlit(FullscreenOut i) : SV_Target0
{
    return g_source.Load(int3(int2(i.position.xy), 0));
}
