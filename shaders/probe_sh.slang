// Projects a probe's lit capture (probe_light.slang) onto first-order spherical harmonics and writes them into the
// probe grid: one thread group per probe refreshed this frame.
//
// A probe that sees (almost) nothing below the horizon is inside the terrain or over open water: its capture is not
// trustworthy, so it is stored empty (alpha 0) and the lighting pass falls back to the atmosphere's sky light there.
//
// Bindings (layoutProbeSh): b0 ProbeConstants, t0 lit capture (alpha = 1 on geometry), u0..u3 the four coefficient
// textures (RWTexture2DArray: x, y = grid texel, z = layer).
#include "common/probe.slangh"

ConstantBuffer<ProbeConstants> g_probes : register(b0);
Texture2D<float4> g_lit : register(t0);
RWTexture2DArray<float4> g_out0 : register(u0);
RWTexture2DArray<float4> g_out1 : register(u1);
RWTexture2DArray<float4> g_out2 : register(u2);
RWTexture2DArray<float4> g_out3 : register(u3);

static const uint kThreads = 256;
static const uint kAccum = 15; // 4 coefficients x rgb, hit weight below the horizon, total weight below the horizon, pad
groupshared float gs_accum[kThreads * kAccum];

[shader("compute")]
[numthreads(256, 1, 1)]
void csProbeSh(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint probe = group.x;
    if (probe >= g_probes.count.x) return;
    float3 origin = g_probes.origin[probe].xyz;

    float acc[kAccum];
    [unroll] for (uint k = 0; k < kAccum; ++k) acc[k] = 0.0;

    const uint texelsPerFace = kProbeFace * kProbeFace;
    for (uint t = thread; t < texelsPerFace * 6; t += kThreads) {
        uint face = t / texelsPerFace;
        uint rem = t % texelsPerFace;
        int2 local = int2(rem % kProbeFace, rem / kProbeFace);
        float2 ndc = (float2(local) + 0.5) / float(kProbeFace) * float2(2.0, -2.0) + float2(-1.0, 1.0);
        float4 w = mul(g_probes.invViewProj[probe * 6 + face], float4(ndc, 0.5, 1.0));
        float3 dir = normalize(w.xyz / w.w - origin);
        // Solid angle of a texel of a 90 degree face: 4 / (F^2 (1 + u^2 + v^2)^1.5).
        float dOmega = 4.0 / (float(texelsPerFace) * pow(1.0 + dot(ndc, ndc), 1.5));
        float4 lit = g_lit.Load(int3(int2(face * kProbeFace, probe * kProbeFace) + local, 0));
        float3 radiance = lit.rgb;
        float y0 = 0.282095, y1 = 0.488603;
        float basis[4] = {y0, y1 * dir.y, y1 * dir.z, y1 * dir.x};
        [unroll] for (uint c = 0; c < 4; ++c) {
            acc[c * 3 + 0] += radiance.r * basis[c] * dOmega;
            acc[c * 3 + 1] += radiance.g * basis[c] * dOmega;
            acc[c * 3 + 2] += radiance.b * basis[c] * dOmega;
        }
        if (dir.y < -0.25) {
            acc[12] += lit.a * dOmega;
            acc[13] += dOmega;
        }
    }
    [unroll] for (uint k2 = 0; k2 < kAccum; ++k2) gs_accum[thread * kAccum + k2] = acc[k2];
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kThreads / 2; stride > 0; stride >>= 1) {
        if (thread < stride) {
            [unroll] for (uint k3 = 0; k3 < kAccum; ++k3) gs_accum[thread * kAccum + k3] += gs_accum[(thread + stride) * kAccum + k3];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread != 0) return;

    uint3 texel = uint3(g_probes.slot[probe].x, g_probes.slot[probe].y, g_probes.slot[probe].z);
    float hit = gs_accum[12] / max(gs_accum[13], 1e-4);
    bool valid = hit > 0.4;
    float4 sh0 = float4(gs_accum[0], gs_accum[1], gs_accum[2], 1.0);
    float4 sh1 = float4(gs_accum[3], gs_accum[4], gs_accum[5], 0.0);
    float4 sh2 = float4(gs_accum[6], gs_accum[7], gs_accum[8], 0.0);
    float4 sh3 = float4(gs_accum[9], gs_accum[10], gs_accum[11], 0.0);
    if (!valid) {
        sh0 = sh1 = sh2 = sh3 = 0.0;
    } else if (g_probes.slot[probe].w != 0 && g_out0[texel].a > 0.5) {
        // A refresh of the same place: ease toward the new capture so a moving sun or a passing cloud does not pop.
        sh0 = lerp(g_out0[texel], sh0, 0.5);
        sh1 = lerp(g_out1[texel], sh1, 0.5);
        sh2 = lerp(g_out2[texel], sh2, 0.5);
        sh3 = lerp(g_out3[texel], sh3, 0.5);
    }
    g_out0[texel] = sh0;
    g_out1[texel] = sh1;
    g_out2[texel] = sh2;
    g_out3[texel] = sh3;
}
