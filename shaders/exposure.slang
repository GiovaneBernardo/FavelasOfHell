// Automatic exposure by a luminance histogram (the approach of most current engines): the scene's log2 luminance is
// binned into 256 buckets, the average is taken over the bins between two percentiles (so a few very bright pixels,
// like the sun disc, or black borders do not drag the exposure), and the exposure then adapts toward the value that
// maps that average to middle grey. The adapted value lives in a tiny persistent per-viewport buffer that the
// tonemap pass reads.
//
//   csHistogram   one thread per 2x2 block of the HDR image (a bilinear tap on the block corner averages it)
//   csAverage     a single thread: percentile average -> target exposure -> temporal adaptation
#include "common/post.slangh"

ConstantBuffer<PostConstants> g_post : register(b0);

// ---- histogram ----------------------------------------------------------------------------------------------

Texture2D<float4> g_hdr : register(t0);
SamplerState g_linear : register(s0);
RWStructuredBuffer<uint> g_bins : register(u0);

groupshared uint s_bins[kHistogramBins];

[shader("compute")]
[numthreads(16, 16, 1)]
void csHistogram(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    s_bins[index] = 0;
    GroupMemoryBarrierWithGroupSync();

    float2 size = g_post.size.xy;
    if (all(float2(id.xy) * 2.0 < size)) {
        // The corner shared by the four pixels of the block: one bilinear tap is their average.
        float2 uv = (float2(id.xy) * 2.0 + 1.0) / size;
        float lum = luminance(g_hdr.SampleLevel(g_linear, uv, 0).rgb);
        if (lum > 1e-4 && lum < 65000.0) {
            float t = saturate((log2(lum) - g_post.adapt.x) / (g_post.adapt.y - g_post.adapt.x));
            uint bin = 1 + (uint)(t * 254.0);
            // 0 = whole image, 1 = weight falls to zero at the corners.
            float2 ndc = uv * 2.0 - 1.0;
            float weight = lerp(1.0, 1.0 - saturate(length(ndc) * 0.70710678), g_post.meter.w);
            InterlockedAdd(s_bins[bin], (uint)(weight * kHistogramWeightScale + 0.5));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (s_bins[index] > 0) InterlockedAdd(g_bins[index], s_bins[index]);
}

// ---- average + adaptation -------------------------------------------------------------------------------------

// Registers 1 (not 0): both entry points live in this file, keep their explicit bindings from overlapping.
StructuredBuffer<uint> g_histogram : register(t1);
// x = exposure (linear), y = exposure EV, z = measured average luminance, w = target EV
RWStructuredBuffer<float4> g_state : register(u1);

static const float kMiddleGrey = 0.18;

[shader("compute")]
[numthreads(1, 1, 1)]
void csAverage()
{
    float total = 0.0;
    for (uint i = 1; i < kHistogramBins; ++i) total += (float)g_histogram[i];

    float4 state = g_state[0];
    float currentEv = state.y;
    // After a reset the buffer holds nothing meaningful (possibly NaN): start from middle grey, exposure 1.
    float logLum = g_post.exposure.w > 0.5 ? log2(kMiddleGrey) : log2(max(state.z, 1e-6));

    if (total > 0.0) {
        float lo = total * g_post.meter.y;
        float hi = total * g_post.meter.z;
        float accumulated = 0.0, sum = 0.0, count = 0.0;
        for (uint b = 1; b < kHistogramBins; ++b) {
            float c = (float)g_histogram[b];
            // The part of this bin inside the percentile window.
            float w = max(min(accumulated + c, hi) - max(accumulated, lo), 0.0);
            sum += w * min((float(b - 1) + 0.5) / 254.0, 1.0);
            count += w;
            accumulated += c;
        }
        if (count > 0.0) logLum = lerp(g_post.adapt.x, g_post.adapt.y, sum / count);
    }

    float targetEv = clamp(log2(kMiddleGrey) - logLum + g_post.exposure.z, -16.0, 16.0);
    float ev = targetEv;
    if (g_post.exposure.w < 0.5) {
        // Exponential approach in EV space (frame-rate independent). A brighter scene needs less exposure: that
        // direction is the fast one, as in the eye.
        float speed = targetEv < currentEv ? g_post.adapt.z : g_post.adapt.w;
        ev = currentEv + (targetEv - currentEv) * (1.0 - exp(-g_post.meter.x * speed));
    }
    g_state[0] = float4(exp2(ev), ev, exp2(logLum), targetEv);
}
