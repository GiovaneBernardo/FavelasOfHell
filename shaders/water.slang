// Water surface. A camera-centred clipmap of rings (each twice as coarse as the one inside), generated from
// SV_VertexID: there is no vertex buffer. The pixel shader runs only where the water is visible (it tests the scene
// depth first), reads the opaque scene from a copy (refraction, screen-space reflections) and writes the final colour
// over the HDR target.
//
// Waves: FFT cascades (water_sim.slang) or, on the low quality tier, six analytic Gerstner waves.
//   vertex stage   displacement from the displacement maps at the mip that matches the ring's vertex spacing, so a
//                  coarse ring only gets the waves it can represent (no aliasing, no morphing needed);
//                  amplitude is damped toward the shore (shoaling) using the sea-floor height field
//   pixel stage    slopes + foam from the slope maps (trilinear anisotropic), roughness from the slope variance
//                  that the mips lose, so distant water turns into a glossy sheen instead of shimmering
// Shading: Schlick Fresnel; reflections = screen-space hit or else the sky probe (atmosphere + clouds, even those
// off-screen) with a parallax correction for the cloud layer; GGX sun glint; Beer-Lambert absorption along the view
// ray through the water (thickness from the scene depth, or from the height field where nothing is drawn);
// body scattering lit by the atmosphere tables; foam (whitecaps, breaker zone, swash line); aerial perspective.
#include "common/view.slangh"
#include "common/water.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
ConstantBuffer<WaterConstants> g_water : register(b1);
Texture2D<float> g_depth : register(t0);
Texture2D<float4> g_scene : register(t1);
Texture2D<float4> g_sky : register(t2);
Texture2DArray<float4> g_disp : register(t3);
Texture2DArray<float4> g_slope : register(t4);
Texture2D<float> g_bed : register(t5);
Texture2D<float4> g_atmoTransmittance : register(t6);
Texture3D<float4> g_atmoScattering : register(t7);
Texture2D<float4> g_atmoIrradiance : register(t8);
SamplerState g_wave : register(s0);       // anisotropic, wrap
SamplerState g_atmoSampler : register(s1); // bilinear, clamp (atmosphere tables, scene, probe, height field)

#include "common/atmosphere.slangh"

static const float kPi = 3.14159265;

// ---- sea floor ---------------------------------------------------------------------------------------------------

// Vertical depth of the water (m) at a position, from the sea-floor height field.
float bedDepth(float2 xz)
{
    if (g_water.bedParams.x < 0.5) return g_water.bedParams.z;
    float h = g_bed.SampleLevel(g_atmoSampler, (xz - g_water.bed.xy) * g_water.bed.zw, 0);
    return g_water.level.x - h;
}

// Waves lose height in shallow water (and break, which the foam shows).
float shoalFactor(float depth)
{
    return lerp(0.12, 1.0, smoothstep(0.0, max(g_water.bedParams.y, 0.01), depth));
}

// ---- vertex stage ------------------------------------------------------------------------------------------------

struct WaterVsOut {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float2 gridXZ : TEXCOORD1; // where the vertex is before the waves moved it: wave lookups use this
    float shoal : TEXCOORD2;
};

float3 gerstnerDisplace(float2 p, float cell)
{
    float kaTotal = 0.0;
    [unroll] for (int i = 0; i < kWaterGerstnerWaves; ++i) kaTotal += g_water.gerstner[i].w * g_water.gerstner[i].z;
    float q = g_water.level.z / max(kaTotal, 1e-3); // chop 1 = the steepest surface that does not fold over
    float3 d = float3(0.0, 0.0, 0.0);
    [unroll] for (int i = 0; i < kWaterGerstnerWaves; ++i) {
        float4 g = g_water.gerstner[i];
        float phase = g.w * dot(g.xy, p) - sqrt(kWaterGravity * g.w) * g_water.level.y;
        float fade = smoothstep(2.0, 4.0, (2.0 * kPi / g.w) / cell); // waves this ring's vertices cannot resolve
        d.xz += g.xy * (q * g.z * cos(phase) * fade);
        d.y += g.z * sin(phase) * fade;
    }
    return d;
}

float3 fftDisplace(float2 p, float cell)
{
    float3 d = float3(0.0, 0.0, 0.0);
    [unroll] for (int c = 0; c < 3; ++c) {
        float len = g_water.cascade[c];
        float texel = g_water.grid.w * (len / g_water.cascade.x);
        float mip = clamp(log2(max(cell, 1e-3) / texel), 0.0, g_water.misc.y);
        d += g_disp.SampleLevel(g_wave, float3(p / len, (float)c), mip).xyz;
    }
    return d;
}

WaterVsOut buildVertex(uint vid, uint level)
{
    uint cells = (uint)g_water.grid.y;
    uint stride = cells + 1u;
    float2 ij = float2(vid % stride, vid / stride);
    float cell = g_water.grid.x * exp2((float)level);
    float2 center = round(g_view.cameraPos.xz / cell) * cell; // snapped: the lattice stays fixed in the world
    float2 gp = (ij - 0.5 * (float)cells) * cell + center;

    float shoal = g_water.bedParams.x > 0.5 ? shoalFactor(bedDepth(gp)) : 1.0;
    float3 d = g_water.cascade.w > 0.5 ? fftDisplace(gp, cell) : gerstnerDisplace(gp, cell);
    d *= shoal;

    WaterVsOut o;
    o.world = float3(gp.x + d.x, g_water.level.x + d.y, gp.y + d.z);
    o.position = mul(g_view.viewProj, float4(o.world, 1.0));
    o.gridXZ = gp;
    o.shoal = shoal;
    return o;
}

// Rings 1.. are drawn coarsest first (instance 0), so the finest ring ends up on top where they overlap.
[shader("vertex")]
WaterVsOut vsWaterRing(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    return buildVertex(vid, (uint)g_water.grid.z - 1u - inst);
}

// The innermost level: a full grid, no hole.
[shader("vertex")]
WaterVsOut vsWaterCenter(uint vid : SV_VertexID)
{
    return buildVertex(vid, 0u);
}

// ---- pixel stage: waves ------------------------------------------------------------------------------------------

struct Surface {
    float2 slope;   // dh/dx, dh/dz
    float foam;     // 0..1, persistent
    float variance; // sub-pixel slope variance (what the filtering averaged away)
    float height;   // wave height above the sea level (m), per pixel: the clipmap rings filter the vertex heights differently
};

Surface surfaceFFT(float2 gp)
{
    const float foamWeight[3] = {1.0, 0.8, 0.5};
    Surface s;
    s.slope = float2(0.0, 0.0);
    s.foam = 0.0;
    s.variance = 0.0;
    s.height = 0.0;
    [unroll] for (int c = 0; c < 3; ++c) {
        float3 uvw = float3(gp / g_water.cascade[c], (float)c);
        float4 t = g_slope.Sample(g_wave, uvw);
        s.height += g_disp.Sample(g_wave, uvw).y;
        s.slope += t.xy;
        s.foam = max(s.foam, t.z * foamWeight[c]);
        s.variance += max(t.w - dot(t.xy, t.xy), 0.0);
    }
    return s;
}

Surface surfaceGerstner(float2 gp, float footprint)
{
    float kaTotal = 0.0;
    [unroll] for (int i = 0; i < kWaterGerstnerWaves; ++i) kaTotal += g_water.gerstner[i].w * g_water.gerstner[i].z;
    float q = g_water.level.z / max(kaTotal, 1e-3);

    float2 slope = float2(0.0, 0.0);
    float dxx = 0.0, dzz = 0.0, dxz = 0.0, variance = 0.0, height = 0.0;
    [unroll] for (int i = 0; i < kWaterGerstnerWaves; ++i) {
        float4 g = g_water.gerstner[i];
        float k = g.w;
        float phase = k * dot(g.xy, gp) - sqrt(kWaterGravity * k) * g_water.level.y;
        // Waves narrower than ~2 pixels would alias: they become roughness instead.
        float resolved = saturate((2.0 * kPi / k) / (footprint * 3.0) - 1.0);
        float c = cos(phase), s = sin(phase);
        slope += g.xy * (k * g.z * c * resolved);
        height += g.z * s * resolved;
        float h = q * g.z;
        dxx -= h * k * g.x * g.x * s * resolved;
        dzz -= h * k * g.y * g.y * s * resolved;
        dxz -= h * k * g.x * g.y * s * resolved;
        variance += 0.5 * (k * g.z) * (k * g.z) * (1.0 - resolved);
    }
    float jac = (1.0 + dxx) * (1.0 + dzz) - dxz * dxz;
    Surface sf;
    sf.slope = slope / float2(max(1.0 + dxx, 0.2), max(1.0 + dzz, 0.2));
    sf.foam = saturate((g_water.misc.w - jac) * 2.5);
    sf.variance = variance;
    sf.height = height;
    return sf;
}

// ---- pixel stage: helpers ----------------------------------------------------------------------------------------

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float valueNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(i), hash21(i + float2(1.0, 0.0)), f.x), lerp(hash21(i + float2(0.0, 1.0)), hash21(i + float2(1.0, 1.0)), f.x), f.y);
}

float foamNoise(float2 p, float footprintScale)
{
    float n = valueNoise(p) * 0.55 + valueNoise(p * 2.31 + 7.7) * 0.3 + valueNoise(p * 5.17 + 3.1) * 0.15;
    return lerp(n, 0.55, saturate(footprintScale)); // far away the pattern is smaller than a pixel: show its average
}

float interleavedGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float2 octahedralEncode(float3 d)
{
    float2 p = d.xz / (abs(d.x) + abs(d.y) + abs(d.z));
    return float2(p.x + p.y, p.x - p.y) * 0.5 + 0.5;
}

// Roughness blurs the probe a little (five taps): the sea does not mirror cloud edges sharply.
float3 probeColor(float3 dir, float rough)
{
    float2 uv = octahedralEncode(dir);
    float r = 0.004 + rough * 0.06;
    float3 c = g_sky.SampleLevel(g_atmoSampler, uv, 0).rgb * 0.4;
    c += g_sky.SampleLevel(g_atmoSampler, uv + float2(r, 0.0), 0).rgb * 0.15;
    c += g_sky.SampleLevel(g_atmoSampler, uv - float2(r, 0.0), 0).rgb * 0.15;
    c += g_sky.SampleLevel(g_atmoSampler, uv + float2(0.0, r), 0).rgb * 0.15;
    c += g_sky.SampleLevel(g_atmoSampler, uv - float2(0.0, r), 0).rgb * 0.15;
    return c;
}

// The probe is a function of direction from the camera, but the clouds are a layer at finite height: look up
// the direction toward where the reflected ray crosses that layer.
float3 probeDirection(float3 p, float3 r)
{
    float t = clamp((g_water.reflection.w - p.y) / max(r.y, 0.05), 0.0, 40000.0);
    float3 d = p + r * t - g_view.cameraPos.xyz;
    return normalize(float3(d.x, max(d.y, 1e-3), d.z));
}

// View-space distance for a reverse-Z depth value (the projection is the standard one, see MakePerspectiveReverseZ).
float linearViewDepth(float d)
{
    float n = g_view.params.z, f = g_view.params.w;
    return (f * n / (f - n)) / (d + n / (f - n));
}

// Screen-space reflection: a linear march in pixel space along the reflected ray with perspective-correct depth,
// jittered per pixel, refined by four bisection steps. Returns the colour of the scene at the hit (rgb) and how far
// to trust it (a). Rays that leave the screen or pass behind geometry return 0 and the caller falls back to the probe.
float4 traceScreen(float3 p, float3 r, float jitter, float rough)
{
    float2 size = 1.0 / g_view.viewport.zw;
    float4 c0 = mul(g_view.viewProj, float4(p, 1.0));
    float4 c1 = mul(g_view.viewProj, float4(p + r * g_water.reflection.y, 1.0));
    float wMin = max(g_view.params.z, 0.05);
    if (c0.w < wMin) return float4(0.0, 0.0, 0.0, 0.0);
    if (c1.w < wMin) c1 = lerp(c0, c1, (c0.w - wMin) / (c0.w - c1.w));

    float2 s0 = (c0.xy / c0.w * float2(0.5, -0.5) + 0.5) * size;
    float2 s1 = (c1.xy / c1.w * float2(0.5, -0.5) + 0.5) * size;
    float k0 = 1.0 / c0.w, k1 = 1.0 / c1.w;
    float2 delta = s1 - s0;

    // Clip the segment to the screen.
    float tEnd = 1.0;
    if (delta.x > 1e-4) tEnd = min(tEnd, (size.x - 1.0 - s0.x) / delta.x);
    else if (delta.x < -1e-4) tEnd = min(tEnd, -s0.x / delta.x);
    if (delta.y > 1e-4) tEnd = min(tEnd, (size.y - 1.0 - s0.y) / delta.y);
    else if (delta.y < -1e-4) tEnd = min(tEnd, -s0.y / delta.y);
    float len = length(delta) * tEnd;
    if (len < 2.0) return float4(0.0, 0.0, 0.0, 0.0);

    float steps = min(g_water.reflection.x, len * 0.5);
    float stride = tEnd / steps;
    float tPrev = 0.0;
    for (float n = 0.0; n < steps; n += 1.0) {
        float t = (n + 0.2 + 0.8 * jitter) * stride;
        float d = g_depth.Load(int3(int2(s0 + delta * t), 0));
        if (d > 0.0) {
            float rayZ = 1.0 / lerp(k0, k1, t);
            float diff = rayZ - linearViewDepth(d);
            if (diff > 0.0 && diff < g_water.reflection.z * (1.0 + rayZ * 0.02)) {
                float lo = tPrev, hi = t;
                [unroll] for (int b = 0; b < 4; ++b) {
                    float mid = 0.5 * (lo + hi);
                    float dm = g_depth.Load(int3(int2(s0 + delta * mid), 0));
                    if (dm > 0.0 && 1.0 / lerp(k0, k1, mid) > linearViewDepth(dm)) hi = mid; else lo = mid;
                }
                float2 uv = (s0 + delta * hi) * g_view.viewport.zw;
                float2 edge = min(uv, 1.0 - uv);
                float conf = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - smoothstep(0.6, 1.0, hi)) * saturate(1.0 - rough * 2.0);
                return float4(g_scene.SampleLevel(g_atmoSampler, uv, 0).rgb, conf);
            }
        }
        tPrev = t;
    }
    return float4(0.0, 0.0, 0.0, 0.0);
}

float distributionGGX(float nh, float a)
{
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float visibilitySmith(float nl, float nv, float a)
{
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// ---- pixel stage -------------------------------------------------------------------------------------------------

[shader("fragment")]
float4 psWater(WaterVsOut i) : SV_Target0
{
    int2 px = int2(i.position.xy);
    float sceneDepth = g_depth.Load(int3(px, 0));
    if (sceneDepth > i.position.z) discard; // geometry in front of the water

    float3 cam = g_view.cameraPos.xyz;
    float seaLevel = g_water.level.x;
    float time = g_water.level.y;
    float2 uv = (float2(px) + 0.5) * g_view.viewport.zw;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float2 fw = fwidth(i.gridXZ);
    float footprint = max(fw.x, fw.y);

    // ---- surface
    Surface sf = g_water.cascade.w > 0.5 ? surfaceFFT(i.gridXZ) : surfaceGerstner(i.gridXZ, footprint);
    sf.slope *= i.shoal;
    // The shading position uses the per-pixel (filtered) wave height, not the interpolated vertex: the clipmap rings
    // filter the vertex heights differently, and at grazing angles the view vector (so the Fresnel term) is that
    // sensitive to a metre of height, which showed as a brightness step at every ring boundary.
    float3 P = float3(i.world.x, seaLevel + sf.height * i.shoal, i.world.z);
    float3 toCam = cam - P;
    float dist = length(toCam);
    float3 V = toCam / dist;
    float3 N = normalize(float3(-sf.slope.x, 1.0, -sf.slope.y));
    float rough = saturate(sqrt(sf.variance));
    if (g_water.cascade.w > 0.5 && g_water.misc.w > 1.5 && g_water.misc.w < 4.5) { // QA-TEMP debug views
        if (g_water.misc.w < 2.5) return float4(sf.foam.xxx, 1.0);
        if (g_water.misc.w < 3.5) return float4(0.5 + sf.height.xxx * 0.5, 1.0);
        if (g_water.misc.w < 4.5) return float4(0.5 + sf.slope.x * 2.0, 0.5 + sf.slope.y * 2.0, sf.variance * 20.0, 1.0);
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    // ---- light
    AtmoRt atmo = atmoFromView(g_view);
    bool atmosphereOn = g_view.atmosphere1.y > 0.5;
    float3 toSun = -g_view.sunDirection.xyz;
    float3 pointKm = atmoPosition(atmo, P, cam);
    float pointR = length(pointKm);
    float muS = dot(pointKm, toSun) / pointR;
    float3 sunE = g_view.sunColor.rgb;
    float3 skyE = float3(0.30, 0.38, 0.52);
    if (atmosphereOn) {
        sunE *= atmoTransmittanceToSun(atmo, pointR, muS);
        skyE = atmoSkyIrradiance(atmo, pointR, muS) * g_view.sunColor.rgb;
    }
    skyE += float3(0.006, 0.008, 0.014); // moonless nights are dark, not black
    float3 lightIn = sunE * saturate(toSun.y) + skyE;

    // ---- the water column behind this pixel
    bool hasScene = sceneDepth > 0.0;
    float thickness = 200.0;
    float verticalDepth = 200.0;
    if (hasScene) {
        float3 Ps = worldPointAt(g_view.invViewProj, ndc, sceneDepth);
        thickness = length(Ps - P);
        verticalDepth = seaLevel - Ps.y;
    }

    if (g_water.misc.w > 5.5 && g_water.misc.w < 6.5) { // QA-TEMP: world position of the scene point
        float3 q = worldPointAt(g_view.invViewProj, ndc, sceneDepth);
        return float4(frac(q.x / 100.0), frac(q.z / 100.0), saturate(-q.y / 80.0), 1.0);
    }
    if (g_water.misc.w > 4.5 && g_water.misc.w < 5.5) return float4(hasScene ? 1.0 : 0.0, saturate(thickness / 200.0), saturate(verticalDepth / 60.0), 1.0); // QA-TEMP
    if (cam.y < seaLevel) {
        // Under the surface: the view through the water is all fog.
        float3 scene = g_scene.SampleLevel(g_atmoSampler, uv, 0).rgb;
        float travel = hasScene ? thickness : 200.0;
        float3 fog = g_water.body.rgb * lightIn * (1.0 / kPi);
        float3 c = scene * exp(-g_water.absorb.rgb * travel) + fog * (1.0 - exp(-travel * g_water.body.w));
        return float4(c, 1.0);
    }

    // ---- refraction: the scene seen through the surface
    float2 uvR = uv;
    if (hasScene) {
        float2 offset = N.xz * g_water.absorb.w * saturate(thickness / 3.0) / (1.0 + dist * 0.1);
        float2 uvTry = uv + offset;
        float dTry = g_depth.Load(int3(int2(uvTry / g_view.viewport.zw), 0));
        if (dTry > 0.0 && dTry <= i.position.z) { // something behind the surface: safe to sample
            uvR = uvTry;
            float2 ndcR = float2(uvR.x * 2.0 - 1.0, 1.0 - uvR.y * 2.0);
            thickness = length(worldPointAt(g_view.invViewProj, ndcR, dTry) - P);
        }
    }
    float3 beneath;
    if (hasScene) {
        beneath = g_scene.SampleLevel(g_atmoSampler, uvR, 0).rgb;
    } else {
        // Nothing drawn below the water (no terrain yet, open sea): a flat sandy floor at the height field's depth,
        // deep water where there is none.
        float floorDepth = bedDepth(i.gridXZ);
        thickness = clamp(floorDepth / max(V.y, 0.05), 0.0, 200.0);
        verticalDepth = floorDepth;
        beneath = g_water.bedParams.w * lightIn * (1.0 / kPi);
    }
    float3 transmitted = beneath * exp(-g_water.absorb.rgb * thickness);
    float scatterAmount = 1.0 - exp(-thickness * g_water.body.w);
    float3 bodyColor = g_water.body.rgb * lightIn * (1.0 / kPi);
    transmitted += bodyColor * scatterAmount;

    // Light shining through the crests: only in the tall waves, toward the sun.
    float crest = saturate(sf.height * i.shoal * 0.8);
    float sss = crest * pow(saturate(dot(-V, toSun)), 3.0) * saturate(1.0 - V.y * 1.5);
    transmitted += g_water.body.rgb * sunE * sss * 0.5;

    // ---- reflection
    float3 R = reflect(-V, N);
    R = normalize(float3(R.x, abs(R.y) + 0.004, R.z)); // a ray into the water is a ray off the horizon
    float jitter = frac(interleavedGradientNoise(i.position.xy) + g_water.misc.x * 0.61803398875);
    float4 ssr = float4(0.0, 0.0, 0.0, 0.0);
    if (g_water.reflection.x > 0.5) ssr = traceScreen(P, R, jitter, rough);
    float3 reflection = lerp(probeColor(probeDirection(P, R), rough), ssr.rgb, ssr.a);

    float nv = saturate(dot(N, V));
    float fresnel = 0.02 + (max(1.0 - rough, 0.02) - 0.02) * pow(1.0 - nv, 5.0);
    float3 color = lerp(transmitted, reflection, fresnel);

    // ---- sun glint
    if (g_view.sunDirection.w > 0.5 && toSun.y > 0.0) {
        float3 H = normalize(V + toSun);
        float nh = saturate(dot(N, H)), nl = saturate(dot(N, toSun)), vh = saturate(dot(V, H));
        float a = clamp(rough * 0.8, 0.03, 0.8); // the sun is a disc: a floor on the roughness keeps the glint finite
        float f = 0.02 + 0.98 * pow(1.0 - vh, 5.0);
        color += distributionGGX(nh, a) * visibilitySmith(nl, max(dot(N, V), 1e-3), a) * f * sunE * nl * (1.0 - ssr.a);
    }

    // ---- foam: whitecaps from the simulation, the breaker zone and the swash line from the shore
    float foamAmount = sf.foam * g_water.foam.x;
    if (g_water.foam.y > 0.0) {
        float floorDepth = bedDepth(i.gridXZ);
        float2 windDir = float2(cos(g_water.misc.z), sin(g_water.misc.z));
        // Breakers: waves arrive parallel to the shore, so their crests follow the depth contours. They travel
        // toward the beach (the phase falls with the depth and rises with time) and the line wanders along it.
        float warp = valueNoise(i.gridXZ * 0.035) * 2.0 - 1.0;
        float phase = floorDepth * 1.4 + time * 0.12 + warp * 0.6;
        float line = smoothstep(0.55, 1.0, 0.5 + 0.5 * sin(phase * 6.2831853));
        float zone = g_water.foam.w;
        float breakers = smoothstep(zone, zone * 0.15, floorDepth) * line + smoothstep(zone * 1.6, 0.0, floorDepth) * 0.3;
        // Swash: a thin line where the water ends, surging back and forth.
        float surge = 0.25 + 0.15 * sin(time * 0.9 + dot(i.gridXZ, windDir) * 0.07);
        float swash = 1.0 - smoothstep(0.0, surge, verticalDepth);
        foamAmount = max(foamAmount, saturate(breakers + swash) * g_water.foam.y);
    }
    if (foamAmount > 0.01) {
        float noise = foamNoise(i.gridXZ * g_water.foam.z + float2(time * 0.05, 0.0), footprint * g_water.foam.z * 0.5);
        float mask = saturate(foamAmount * 1.7 - (1.0 - noise) * 0.9);
        float3 foamLit = 0.85 * (sunE * saturate(dot(float3(0.0, 1.0, 0.0), toSun) * 0.8 + 0.2) + skyE) * (1.0 / kPi);
        color = lerp(color, foamLit, mask * 0.92);
    }

    // ---- blend into the shore, then the air between here and the eye
    if (hasScene) {
        float edge = smoothstep(0.0, 0.12, verticalDepth);
        color = lerp(g_scene.SampleLevel(g_atmoSampler, uv, 0).rgb, color, edge);
    }
    if (atmosphereOn && g_view.render.y > 0.5) {
        float3 cameraKm = atmoPosition(atmo, cam, cam);
        float3 transmittance;
        float3 inscatter = atmoSkyRadianceToPoint(atmo, cameraKm, pointKm, toSun, transmittance);
        color = color * transmittance + inscatter * g_view.sunColor.rgb;
    }
    return float4(color, 1.0);
}
