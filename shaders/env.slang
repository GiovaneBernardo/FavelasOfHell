// Blur chain of the sky environment image (see psSkyEnv in sky_probe.slang): each level is half the size of the
// previous one and is filled with a 3x3 binomial filter plus the bilinear footprint of the 2x downsample, so a level
// is about twice as blurry as the one before: the levels stand in for GGX roughness 0, 0.25, 0.5, 0.75, 1.
//
// t0 = the previous level, s0 = bilinear clamp. The octahedral image wraps across its edge only on the side facing
// down, where nothing ever reflects the sky, so clamping is fine.
#include "common/fullscreen.slangh"

Texture2D<float4> g_source : register(t0);
SamplerState g_linear : register(s0);

[shader("fragment")]
float4 psEnvDown(FullscreenOut i) : SV_Target0
{
    uint w, h;
    g_source.GetDimensions(w, h);
    float2 srcTexel = 1.0 / float2(w, h);
    float2 uv = i.position.xy / (float2(w, h) * 0.5);
    float3 sum = 0.0;
    float weights = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float wgt = (2 - abs(x)) * (2 - abs(y));
            sum += g_source.SampleLevel(g_linear, uv + float2(x, y) * srcTexel * 1.5, 0).rgb * wgt;
            weights += wgt;
        }
    }
    return float4(sum / weights, 1.0);
}
