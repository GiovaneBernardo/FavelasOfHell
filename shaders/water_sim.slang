// FFT ocean simulation (Tessendorf): three cascades of tileable wave fields, run once per frame (not per viewport).
//
//   csSpectrum  (only when a wave parameter changed) initial spectrum h0(k): JONSWAP wind sea + swell, random phases
//   csEvolve    h0 -> frequency-domain fields at time t (height, choppy displacement, slopes, Jacobian terms)
//   csFftH/V    inverse 2D FFT of those fields, one row / column per thread group, in group shared memory (Stockham,
//               8 butterfly stages in one dispatch). Two real fields share one complex FFT.
//   csFinalize  displacement map + slope/foam map (foam persists between frames: it fades, new foam is added where
//               the surface folds, i.e. where the Jacobian drops below the threshold)
//   csMip       mip chains of both maps (the surface shader filters with them, the vertex shader low-passes with them)
//
// Every stage has its own register slots (they only overlap if two stages were compiled together); the C++ side
// (water.cpp) builds one binding layout per stage with the same slots.
// Frequency indexing wraps (k = 0..N/2-1, -N/2..-1), so the plain inverse DFT needs no (-1)^(x+y) correction.
static const float kPi = 3.14159265;
static const float kG = 9.81;

struct WaterSimConstants {
    float4 dims;     // x = N (texels per side), y = log2 N, z = cascade count, w = unused
    float4 time;     // x = time wrapped to the loop period (s), y = frame dt (s), z = chop, w = base angular frequency (rad/s)
    float4 lengths;  // xyz = tile length of each cascade (m)
    float4 bandLow;  // xyz = lowest wavenumber each cascade owns (rad / m)
    float4 bandHigh; // xyz = highest
    float4 sea;      // x = wind sea significant height (m), y = peak angular frequency, z = wind direction (rad), w = spread
    float4 swell;    // x = swell significant height (m), y = peak angular frequency, z = swell direction (rad), w = unused
    float4 foam;     // x = Jacobian threshold, y = gain, z = decay per frame (0..1), w = amplitude multiplier
};
ConstantBuffer<WaterSimConstants> g_sim : register(b0);

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 cconj(float2 a) { return float2(a.x, -a.y); }
float2 cexp(float phase) { return float2(cos(phase), sin(phase)); }
float2 timesI(float2 a) { return float2(-a.y, a.x); }

// ---- spectrum ----------------------------------------------------------------------------------------------------

RWTexture2DArray<float2> g_h0Out : register(u0);   // csSpectrum

uint pcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Two independent standard normal numbers (Box-Muller) from a seed.
float2 gaussian2(uint seed)
{
    uint a = pcgHash(seed);
    uint b = pcgHash(a ^ 0x9E3779B9u);
    float u1 = max(float(a >> 8) * (1.0 / 16777216.0), 1e-7);
    float u2 = float(b >> 8) * (1.0 / 16777216.0);
    return sqrt(-2.0 * log(u1)) * float2(cos(2.0 * kPi * u2), sin(2.0 * kPi * u2));
}

// JONSWAP shape (gamma 3.3). `alpha` scales the energy; see alphaFromHs.
float jonswap(float w, float wp, float alpha)
{
    float sigma = w <= wp ? 0.07 : 0.09;
    float d = (w - wp) / (sigma * wp);
    float r = exp(-0.5 * d * d);
    return alpha * kG * kG / pow(w, 5.0) * exp(-1.25 * pow(wp / w, 4.0)) * pow(3.3, r);
}

// Total energy m0 = alpha g^2 / (5 wp^4) for the Pierson-Moskowitz shape, Hs = 4 sqrt(m0); the peak enhancement adds
// about 70% (the 1/1.7).
float alphaFromHs(float hs, float wp)
{
    return (hs * hs / 16.0) * 5.0 * pow(wp, 4.0) / (kG * kG) / 1.7;
}

// Directional spreading |cos(theta / 2)|^(2s), normalised (Gamma(s+1) / Gamma(s+1/2) ~ sqrt(s + 1/4)).
float directional(float theta, float s)
{
    return sqrt(s + 0.25) * 0.28209479 * pow(abs(cos(0.5 * theta)), 2.0 * s);
}

// Hasselmann spreading: wide at the peak, narrower on both sides.
float windSpread(float w, float wp, float narrowing)
{
    float r = w / wp;
    float s = r < 1.05 ? 6.97 * pow(r, 4.06) : 9.77 * pow(r, -1.84);
    return max(s * narrowing, 0.5);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csSpectrum(uint3 id : SV_DispatchThreadID)
{
    uint n = (uint)g_sim.dims.x;
    if (id.x >= n || id.y >= n) return;
    uint c = id.z;
    int ix = id.x < n / 2 ? (int)id.x : (int)id.x - (int)n;
    int iz = id.y < n / 2 ? (int)id.y : (int)id.y - (int)n;
    float dk = 2.0 * kPi / g_sim.lengths[c];
    float2 k = float2(ix, iz) * dk;
    float kl = length(k);

    float2 h0 = float2(0.0, 0.0);
    if (kl > g_sim.bandLow[c] && kl <= g_sim.bandHigh[c]) {
        float w = sqrt(kG * kl);
        float theta = atan2(k.y, k.x);
        float s = 0.0;
        if (g_sim.sea.x > 0.0) {
            float wp = g_sim.sea.y;
            s += jonswap(w, wp, alphaFromHs(g_sim.sea.x, wp)) * directional(theta - g_sim.sea.z, windSpread(w, wp, g_sim.sea.w));
        }
        if (g_sim.swell.x > 0.0) {
            float wp = g_sim.swell.y;
            s += jonswap(w, wp, alphaFromHs(g_sim.swell.x, wp)) * directional(theta - g_sim.swell.z, 40.0);
        }
        // E|h0|^2 = S(k) dk^2 / 2 per mode, with S(k) = S(w) D (dw/dk) / k; the random pair has E|xi|^2 = 2.
        float spectrum = s * kG / (2.0 * w * kl);
        float amp = sqrt(max(spectrum, 0.0)) * dk * 0.5 * g_sim.foam.w;
        h0 = gaussian2(id.x + id.y * n + c * n * n + 0x51ED270Bu) * amp;
    }
    g_h0Out[uint3(id.xy, c)] = h0;
}

// ---- evolve ------------------------------------------------------------------------------------------------------

Texture2DArray<float2> g_h0 : register(t1);          // csEvolve
RWTexture2DArray<float2> g_specOut : register(u1);

[shader("compute")]
[numthreads(8, 8, 1)]
void csEvolve(uint3 id : SV_DispatchThreadID)
{
    uint n = (uint)g_sim.dims.x;
    if (id.x >= n || id.y >= n) return;
    uint c = id.z;
    int ix = id.x < n / 2 ? (int)id.x : (int)id.x - (int)n;
    int iz = id.y < n / 2 ? (int)id.y : (int)id.y - (int)n;
    float dk = 2.0 * kPi / g_sim.lengths[c];
    float2 k = float2(ix, iz) * dk;
    float kl = length(k);

    float2 a = g_h0.Load(int4(id.xy, c, 0));
    float2 b = g_h0.Load(int4((n - id.x) & (n - 1), (n - id.y) & (n - 1), c, 0)); // h0(-k)
    // Frequencies are snapped to multiples of the base frequency so the whole field repeats every loop period.
    float w = sqrt(kG * kl);
    float w0 = g_sim.time.w;
    float wq = floor(w / w0) * w0;
    float2 e = cexp(wq * g_sim.time.x);
    // h(k, t) = h0(k) e^{-iwt} + conj(h0(-k)) e^{+iwt}: waves travel along +k, i.e. along the wind. Hermitian by construction.
    float2 h = cmul(a, cconj(e)) + cmul(cconj(b), e);

    float2 khat = kl > 1e-6 ? k / kl : float2(0.0, 0.0);
    float kInv = kl > 1e-6 ? 1.0 / kl : 0.0;
    // Choppy displacement D = i k/|k| h pulls points toward the crests; slopes are i k h; the Jacobian terms are the
    // derivatives of D (dDx/dx = -kx^2/|k| h, ...).
    float2 dx = timesI(h) * khat.x;
    float2 dz = timesI(h) * khat.y;
    float2 dhdx = timesI(h) * k.x;
    float2 dhdz = timesI(h) * k.y;
    float2 dDxdx = -h * (k.x * k.x * kInv);
    float2 dDzdz = -h * (k.y * k.y * kInv);
    float2 dDxdz = -h * (k.x * k.y * kInv);

    // Two real fields per complex FFT: z = a + i b transforms to (field a) + i (field b).
    uint s = c * 4;
    g_specOut[uint3(id.xy, s + 0)] = h + timesI(dx);
    g_specOut[uint3(id.xy, s + 1)] = dz + timesI(dhdx);
    g_specOut[uint3(id.xy, s + 2)] = dhdz + timesI(dDxdx);
    g_specOut[uint3(id.xy, s + 3)] = dDzdz + timesI(dDxdz);
}

// ---- FFT ---------------------------------------------------------------------------------------------------------

Texture2DArray<float2> g_fftIn : register(t2);        // csFftH / csFftV
RWTexture2DArray<float2> g_fftOut : register(u2);
groupshared float2 s_fft[512]; // two ping-pong halves of up to 256 points

// Inverse FFT of one row (or column) per thread group: Stockham autosort radix 2, N/2 butterflies per stage.
void fft1D(uint lineIdx, uint slice, uint tid, bool vertical)
{
    uint n = (uint)g_sim.dims.x;
    uint logN = (uint)g_sim.dims.y;
    uint halfN = n >> 1;
    for (uint i = tid; i < n; i += 128) {
        uint3 p = vertical ? uint3(lineIdx, i, slice) : uint3(i, lineIdx, slice);
        s_fft[i] = g_fftIn.Load(int4(p, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint stage = 0; stage < logN; ++stage) {
        uint ns = 1u << stage;
        uint srcOff = (stage & 1u) * n;
        uint dstOff = ((stage + 1u) & 1u) * n;
        if (tid < halfN) {
            uint k = tid & (ns - 1u);
            float2 tw = cexp(2.0 * kPi * float(k) / float(ns * 2u));
            float2 v0 = s_fft[srcOff + tid];
            float2 v1 = cmul(s_fft[srcOff + tid + halfN], tw);
            uint d = (tid / ns) * ns * 2u + k;
            s_fft[dstOff + d] = v0 + v1;
            s_fft[dstOff + d + ns] = v0 - v1;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    uint resultOff = (logN & 1u) * n;
    for (uint j = tid; j < n; j += 128) {
        uint3 p = vertical ? uint3(lineIdx, j, slice) : uint3(j, lineIdx, slice);
        g_fftOut[p] = s_fft[resultOff + j];
    }
}

[shader("compute")]
[numthreads(128, 1, 1)]
void csFftH(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    fft1D(gid.x, gid.y, tid, false);
}

[shader("compute")]
[numthreads(128, 1, 1)]
void csFftV(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    fft1D(gid.x, gid.y, tid, true);
}

// ---- finalize ----------------------------------------------------------------------------------------------------

Texture2DArray<float2> g_spatial : register(t3);      // csFinalize
Texture2DArray<float4> g_prevSlope : register(t4);
RWTexture2DArray<float4> g_dispOut : register(u3);   // xyz = displacement (x, height, z)
RWTexture2DArray<float4> g_slopeOut : register(u4);  // xy = slope, z = foam, w = E[slope^2] (for the roughness)

[shader("compute")]
[numthreads(8, 8, 1)]
void csFinalize(uint3 id : SV_DispatchThreadID)
{
    uint n = (uint)g_sim.dims.x;
    if (id.x >= n || id.y >= n) return;
    uint c = id.z;
    uint s = c * 4;
    float2 f0 = g_spatial.Load(int4(id.xy, s + 0, 0));
    float2 f1 = g_spatial.Load(int4(id.xy, s + 1, 0));
    float2 f2 = g_spatial.Load(int4(id.xy, s + 2, 0));
    float2 f3 = g_spatial.Load(int4(id.xy, s + 3, 0));
    float h = f0.x, dx = f0.y, dz = f1.x, dhdx = f1.y, dhdz = f2.x, dDxdx = f2.y, dDzdz = f3.x, dDxdz = f3.y;

    float chop = g_sim.time.z;
    float jxx = 1.0 + chop * dDxdx;
    float jzz = 1.0 + chop * dDzdz;
    // Slopes of the displaced surface (the horizontal pull stretches / squeezes it).
    float2 slope = float2(dhdx, dhdz) / float2(max(jxx, 0.2), max(jzz, 0.2));

    // Whitecaps: a linear spectrum at a moderate sea never folds over by itself, so the foam looks at the Jacobian of a
    // displacement exaggerated three times (floor on the choppiness): where even that squeezes the surface below the
    // threshold the wave is steep enough to break.
    float lf = max(chop, 0.3) * 3.0;
    float jacFoam = (1.0 + lf * dDxdx) * (1.0 + lf * dDzdz) - lf * lf * dDxdz * dDxdz;
    float prevFoam = g_prevSlope.Load(int4(id.xy, c, 0)).z;
    float foam = max(prevFoam * g_sim.foam.z, saturate((g_sim.foam.x - jacFoam) * g_sim.foam.y));

    g_dispOut[uint3(id.xy, c)] = float4(chop * dx, h, chop * dz, 0.0);
    g_slopeOut[uint3(id.xy, c)] = float4(slope, foam, dot(slope, slope));
}

// ---- mips --------------------------------------------------------------------------------------------------------

Texture2DArray<float4> g_srcSlope : register(t5);     // csMip
Texture2DArray<float4> g_srcDisp : register(t6);
RWTexture2DArray<float4> g_dstSlope : register(u5);
RWTexture2DArray<float4> g_dstDisp : register(u6);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMip(uint3 id : SV_DispatchThreadID)
{
    uint w, h, e;
    g_dstSlope.GetDimensions(w, h, e);
    if (id.x >= w || id.y >= h) return;
    int2 p = int2(id.xy) * 2;
    float4 slope = 0.25 * (g_srcSlope.Load(int4(p, id.z, 0)) + g_srcSlope.Load(int4(p + int2(1, 0), id.z, 0)) +
                           g_srcSlope.Load(int4(p + int2(0, 1), id.z, 0)) + g_srcSlope.Load(int4(p + int2(1, 1), id.z, 0)));
    float4 disp = 0.25 * (g_srcDisp.Load(int4(p, id.z, 0)) + g_srcDisp.Load(int4(p + int2(1, 0), id.z, 0)) +
                          g_srcDisp.Load(int4(p + int2(0, 1), id.z, 0)) + g_srcDisp.Load(int4(p + int2(1, 1), id.z, 0)));
    g_dstSlope[id] = slope;
    g_dstDisp[id] = disp;
}
