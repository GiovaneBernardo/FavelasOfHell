// Ground-truth ambient occlusion (Jimenez et al., "Practical Realtime Strategies for Accurate Indirect Occlusion",
// SIGGRAPH 2016; the slice setup follows Intel's XeGTAO): per pixel a few screen-space slices through the surface point,
// each searched for its two horizons, and the visible arc between them integrated analytically against the cosine
// lobe around the normal. psGtao writes the noisy visibility, psGtaoBlur a depth-aware 5x5 filter of it.
//
// Bindings (GtaoLayout): b0 view, t0 depth (R32F), t1 G-buffer normal (octahedral), t2 the noisy visibility (blur only).
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/gbuffer.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_normal : register(t1);
Texture2D<float> g_noisy : register(t2);

static const float kPi = 3.14159265;
static const float kHalfPi = 1.57079633;

float linearDepth(float depth)
{
    float n = g_view.params.z, f = g_view.params.w;
    return f * n / (n + depth * (f - n));
}

// Right-handed view space: x right, y up, the camera looks down -z.
float3 viewPosition(int2 px, float depth)
{
    float d = linearDepth(depth);
    float2 ndc = (float2(px) + 0.5) * g_view.viewport.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
    return float3(ndc.x * d / g_view.proj[0][0], ndc.y * d / g_view.proj[1][1], -d);
}

float interleavedGradientNoise(float2 p, float seed)
{
    return frac(52.9829189 * frac(dot(p + seed * 5.588238, float2(0.06711056, 0.00583715))));
}

float fastAcos(float x)
{
    float a = abs(x);
    float r = sqrt(max(1.0 - a, 0.0)) * (1.5707288 + a * (-0.2121144 + a * (0.0742610 - a * 0.0187293)));
    return x >= 0.0 ? r : kPi - r;
}

[shader("fragment")]
float4 psGtao(FullscreenOut i) : SV_Target0
{
    int2 px = int2(i.position.xy);
    float depth = g_depth.Load(int3(px, 0));
    if (depth <= 0.0) return float4(1.0, 0.0, 0.0, 0.0);

    float3 p = viewPosition(px, depth);
    float3 nWorld = octDecode(g_normal.Load(int3(px, 0)));
    float3 n = normalize(mul(g_view.view, float4(nWorld, 0.0)).xyz);
    float3 v = normalize(-p);

    float radius = g_view.ssao.w;
    // World radius -> pixels at this depth, kept to a range that stays cheap and still resolves contacts.
    float radiusPx = clamp(radius * 0.5 * g_view.viewport.y * g_view.proj[1][1] / -p.z, 6.0, 160.0);
    const float kFalloff = 0.6 * radius; // occluders in the outer 60% of the radius count less
    const int kSlices = 3;
    const int kSteps = 6;
    float noiseSlice = interleavedGradientNoise(i.position.xy, 0.0);
    float noiseStep = interleavedGradientNoise(i.position.xy, 1.0);

    float visibility = 0.0;
    [unroll] for (int slice = 0; slice < kSlices; ++slice) {
        float phi = (float(slice) + noiseSlice) * (kPi / float(kSlices));
        float2 omega = float2(cos(phi), sin(phi)); // in view space (y up); the pixel direction flips y
        float3 directionVec = float3(omega, 0.0);
        float3 orthoDirectionVec = directionVec - dot(directionVec, v) * v;
        float3 axis = normalize(cross(directionVec, v));
        float3 projectedNormal = n - axis * dot(n, axis);
        float projectedLength = length(projectedNormal);
        float signNorm = sign(dot(orthoDirectionVec, projectedNormal));
        float cosNorm = saturate(dot(projectedNormal, v) / max(projectedLength, 1e-4));
        float nAngle = signNorm * fastAcos(cosNorm);

        // The tangent plane is the lowest horizon a side can have.
        float horizonCos0 = cos(nAngle + kHalfPi);
        float horizonCos1 = cos(nAngle - kHalfPi);
        [unroll] for (int step = 0; step < kSteps; ++step) {
            float t = (float(step) + noiseStep) / float(kSteps);
            float offset = max(t * t * radiusPx, 1.5 + float(step));
            float2 stepPx = float2(omega.x, -omega.y) * offset;
            int2 s0 = int2(float2(px) + stepPx);
            int2 s1 = int2(float2(px) - stepPx);
            if (all(s0 >= 0) && all(s0 < int2(g_view.viewport.xy))) {
                float d0 = g_depth.Load(int3(s0, 0));
                if (d0 > 0.0) {
                    float3 delta = viewPosition(s0, d0) - p;
                    float dist = length(delta);
                    float weight = saturate((radius - dist) / kFalloff);
                    horizonCos0 = max(horizonCos0, lerp(cos(nAngle + kHalfPi), dot(delta, v) / max(dist, 1e-4), weight));
                }
            }
            if (all(s1 >= 0) && all(s1 < int2(g_view.viewport.xy))) {
                float d1 = g_depth.Load(int3(s1, 0));
                if (d1 > 0.0) {
                    float3 delta = viewPosition(s1, d1) - p;
                    float dist = length(delta);
                    float weight = saturate((radius - dist) / kFalloff);
                    horizonCos1 = max(horizonCos1, lerp(cos(nAngle - kHalfPi), dot(delta, v) / max(dist, 1e-4), weight));
                }
            }
        }
        float h0 = -fastAcos(horizonCos1);
        float h1 = fastAcos(horizonCos0);
        h0 = nAngle + clamp(h0 - nAngle, -kHalfPi, kHalfPi);
        h1 = nAngle + clamp(h1 - nAngle, -kHalfPi, kHalfPi);
        float arc0 = (cosNorm + 2.0 * h0 * sin(nAngle) - cos(2.0 * h0 - nAngle)) * 0.25;
        float arc1 = (cosNorm + 2.0 * h1 * sin(nAngle) - cos(2.0 * h1 - nAngle)) * 0.25;
        visibility += projectedLength * (arc0 + arc1);
    }
    return float4(saturate(visibility / float(kSlices)), 0.0, 0.0, 0.0);
}

// Joint-bilateral filter: neighbours across a depth discontinuity do not leak into each other.
[shader("fragment")]
float4 psGtaoBlur(FullscreenOut i) : SV_Target0
{
    int2 px = int2(i.position.xy);
    int2 size = int2(g_view.viewport.xy);
    float depth = g_depth.Load(int3(px, 0));
    if (depth <= 0.0) return float4(1.0, 0.0, 0.0, 0.0);
    float centre = linearDepth(depth);
    float sum = 0.0, weightSum = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            int2 q = clamp(px + int2(x, y), int2(0), size - 1);
            float dq = g_depth.Load(int3(q, 0));
            if (dq <= 0.0) continue;
            float diff = abs(linearDepth(dq) - centre) / max(centre, 0.1);
            float w = exp(-diff * 80.0) * exp(-0.25 * float(x * x + y * y));
            sum += g_noisy.Load(int3(q, 0)) * w;
            weightSum += w;
        }
    }
    return float4(sum / max(weightSum, 1e-4), 0.0, 0.0, 0.0);
}
