// Bloom after "Next Generation Post Processing in Call of Duty: Advanced Warfare" (Jimenez, SIGGRAPH 2014):
// a chain of progressively halved targets, each filled with a 13-tap downsample (one bilinear tap pattern that is a
// good approximation of a wide Gaussian, and does not alias the way a plain box does), then walked back up with a
// 3x3 tent filter that is ADDED onto the next larger level (the pass blends One/One onto the downsampled content).
// The result in the largest level is a very wide, smooth glow for the cost of a few small fullscreen passes.
//
// No constants: the texel size comes from the bound texture. Bilinear clamp sampler in s0.
#include "common/fullscreen.slangh"
#include "common/post.slangh"

Texture2D<float4> g_source : register(t0);
SamplerState g_linear : register(s0);

float2 texelSize()
{
    uint w, h;
    g_source.GetDimensions(w, h);
    return 1.0 / float2(w, h);
}

// Weights of a "Karis average" group: bright outliers (a single sun glint) would otherwise flicker through the
// whole chain; weighting each 2x2 group by 1 / (1 + luma) bounds their energy.
float karis(float3 c)
{
    return 1.0 / (1.0 + luminance(c));
}

// 13 taps on the source, laid out as in the slides:
//   a . b . c
//   . j . k .
//   d . e . f
//   . l . m .
//   g . h . i
float3 sampleDown(float2 uv, float2 t, bool firstLevel)
{
    float3 a = g_source.SampleLevel(g_linear, uv + t * float2(-2, 2), 0).rgb;
    float3 b = g_source.SampleLevel(g_linear, uv + t * float2(0, 2), 0).rgb;
    float3 c = g_source.SampleLevel(g_linear, uv + t * float2(2, 2), 0).rgb;
    float3 d = g_source.SampleLevel(g_linear, uv + t * float2(-2, 0), 0).rgb;
    float3 e = g_source.SampleLevel(g_linear, uv, 0).rgb;
    float3 f = g_source.SampleLevel(g_linear, uv + t * float2(2, 0), 0).rgb;
    float3 g = g_source.SampleLevel(g_linear, uv + t * float2(-2, -2), 0).rgb;
    float3 h = g_source.SampleLevel(g_linear, uv + t * float2(0, -2), 0).rgb;
    float3 i = g_source.SampleLevel(g_linear, uv + t * float2(2, -2), 0).rgb;
    float3 j = g_source.SampleLevel(g_linear, uv + t * float2(-1, 1), 0).rgb;
    float3 k = g_source.SampleLevel(g_linear, uv + t * float2(1, 1), 0).rgb;
    float3 l = g_source.SampleLevel(g_linear, uv + t * float2(-1, -1), 0).rgb;
    float3 m = g_source.SampleLevel(g_linear, uv + t * float2(1, -1), 0).rgb;

    if (firstLevel) {
        // Five overlapping groups (four outer boxes + the inner one), each weighted by its own brightness.
        float3 g0 = (a + b + d + e) * 0.25;
        float3 g1 = (b + c + e + f) * 0.25;
        float3 g2 = (d + e + g + h) * 0.25;
        float3 g3 = (e + f + h + i) * 0.25;
        float3 g4 = (j + k + l + m) * 0.25;
        float w0 = 0.125 * karis(g0), w1 = 0.125 * karis(g1), w2 = 0.125 * karis(g2), w3 = 0.125 * karis(g3);
        float w4 = 0.5 * karis(g4);
        return (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    }
    return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

float2 sourceUv(float4 position)
{
    uint w, h;
    g_source.GetDimensions(w, h);
    // The target is half the size of the source (rounded up): map the pixel center to the source's uv space.
    // Done through the target position and the source size so odd sizes stay aligned.
    return position.xy * (2.0 / float2(w, h));
}

[shader("fragment")]
float4 psBloomDownFirst(FullscreenOut i) : SV_Target0
{
    return float4(sampleDown(sourceUv(i.position), texelSize(), true), 1.0);
}

[shader("fragment")]
float4 psBloomDown(FullscreenOut i) : SV_Target0
{
    return float4(sampleDown(sourceUv(i.position), texelSize(), false), 1.0);
}

// 3x3 tent: [1 2 1; 2 4 2; 1 2 1] / 16, radius of one texel of the SMALLER level. The pass blends One/One, so this
// adds onto the downsampled content of the level it renders into.
[shader("fragment")]
float4 psBloomUp(FullscreenOut i) : SV_Target0
{
    float2 t = texelSize();
    // The target is twice the source: its pixel center in the source's uv space is position / (2 * source size).
    uint w, h;
    g_source.GetDimensions(w, h);
    float2 uv = i.position.xy * (0.5 / float2(w, h));

    float3 s = g_source.SampleLevel(g_linear, uv + t * float2(-1, 1), 0).rgb;
    s += g_source.SampleLevel(g_linear, uv + t * float2(0, 1), 0).rgb * 2.0;
    s += g_source.SampleLevel(g_linear, uv + t * float2(1, 1), 0).rgb;
    s += g_source.SampleLevel(g_linear, uv + t * float2(-1, 0), 0).rgb * 2.0;
    s += g_source.SampleLevel(g_linear, uv, 0).rgb * 4.0;
    s += g_source.SampleLevel(g_linear, uv + t * float2(1, 0), 0).rgb * 2.0;
    s += g_source.SampleLevel(g_linear, uv + t * float2(-1, -1), 0).rgb;
    s += g_source.SampleLevel(g_linear, uv + t * float2(0, -1), 0).rgb * 2.0;
    s += g_source.SampleLevel(g_linear, uv + t * float2(1, -1), 0).rgb;
    return float4(s * (1.0 / 16.0), 0.0);
}
