// Bruneton precomputed atmospheric scattering: the compute passes that build the LUTs. Run on demand (editor
// "Atmosphere" panel) or once when no cache file exists; see engine/renderer/src/atmosphere.cpp for the sequence:
//
//   csTransmittance                         -> transmittance
//   csDirectIrradiance                      -> delta irradiance
//   csSingleScattering                      -> delta rayleigh, delta mie, scattering (rayleigh rgb + mie.r in alpha)
//   for each further order n:
//     csScatteringDensity(n)                -> delta density
//     csIndirectIrradiance(n - 1)           -> delta irradiance, irradiance += delta
//     csMultipleScattering                  -> delta multiple, scattering += delta / rayleigh phase
//
// Accumulated tables ping-pong between two textures (a pass reads one and writes the other), so no pass needs
// read-modify-write access to a half-float UAV. Deltas are 32-bit. All lengths in km.
#include "common/atmosphere_core.slangh"

struct PrecomputeConstants {
    float4 radii;    // x bottom radius, y top radius, z sun angular radius, w mu_s_min
    float4 rayleigh; // rgb scattering (1/km), w scale height (km)
    float4 mieScat;  // rgb scattering, w scale height
    float4 mieExt;   // rgb extinction, w Mie phase g
    float4 ozone;    // rgb absorption extinction at the layer's peak
    float4 ground;   // rgb albedo
    float4 solar;    // rgb solar irradiance (1: the tables are per unit of sun irradiance)
    uint4 pass;      // x scattering order, y first depth slice of this dispatch
};
ConstantBuffer<PrecomputeConstants> g_pc : register(b0);

Texture2D<float4> g_transmittance : register(t0);
Texture3D<float4> g_deltaRayleigh : register(t1);
Texture3D<float4> g_deltaMie : register(t2);
Texture3D<float4> g_deltaMulti : register(t3);
Texture3D<float4> g_deltaDensity : register(t4);
Texture2D<float4> g_deltaIrradiance : register(t5);
Texture3D<float4> g_scatteringIn : register(t6);
Texture2D<float4> g_irradianceIn : register(t7);
SamplerState g_linear : register(s0);

RWTexture2D<float4> g_out2D : register(u0);
RWTexture3D<float4> g_outA : register(u1);
RWTexture3D<float4> g_outB : register(u2);
RWTexture2D<float4> g_outIrradiance : register(u3);
RWTexture3D<float4> g_outScattering : register(u4);

AtmoGeom geom()
{
    AtmoGeom a;
    a.bottom = g_pc.radii.x;
    a.top = g_pc.radii.y;
    a.sunAngularRadius = g_pc.radii.z;
    a.muSMin = g_pc.radii.w;
    return a;
}

// ---- density profiles (altitude in km) ----

float densityRayleigh(float h) { return clamp(exp(-h / g_pc.rayleigh.w), 0.0, 1.0); }
float densityMie(float h) { return clamp(exp(-h / g_pc.mieScat.w), 0.0, 1.0); }
// Tent profile of the ozone layer: rises to a peak at 25 km, falls to zero at 40 km.
float densityOzone(float h) { return clamp(h < 25.0 ? h / 15.0 - 2.0 / 3.0 : -h / 15.0 + 8.0 / 3.0, 0.0, 1.0); }

float profileDensity(int kind, float h)
{
    if (kind == 0) return densityRayleigh(h);
    if (kind == 1) return densityMie(h);
    return densityOzone(h);
}

float opticalLengthToTop(int kind, float r, float mu)
{
    const int kSamples = 500;
    AtmoGeom a = geom();
    float dx = distanceToTop(a, r, mu) / float(kSamples);
    float result = 0.0;
    for (int i = 0; i <= kSamples; ++i) {
        float d = float(i) * dx;
        float ri = sqrt(d * d + 2.0 * r * mu * d + r * r);
        float w = (i == 0 || i == kSamples) ? 0.5 : 1.0;
        result += profileDensity(kind, ri - a.bottom) * w * dx;
    }
    return result;
}

// ---- table lookups ----

float3 transmittanceToTop(float r, float mu)
{
    return g_transmittance.SampleLevel(g_linear, transmittanceUvFromRMu(geom(), r, mu), 0).rgb;
}

float3 transmittanceAlong(float r, float mu, float d, bool intersectsGround)
{
    AtmoGeom a = geom();
    float rd = clampRadius(a, sqrt(d * d + 2.0 * r * mu * d + r * r));
    float mud = clampCosine((r * mu + d) / rd);
    if (intersectsGround) return min(transmittanceToTop(rd, -mud) / transmittanceToTop(r, -mu), float3(1.0));
    return min(transmittanceToTop(r, mu) / transmittanceToTop(rd, mud), float3(1.0));
}

float3 transmittanceToSun(float r, float muS)
{
    AtmoGeom a = geom();
    float sinThetaH = a.bottom / r;
    float cosThetaH = -sqrt(max(1.0 - sinThetaH * sinThetaH, 0.0));
    return transmittanceToTop(r, muS) *
           smoothstep(-sinThetaH * a.sunAngularRadius, sinThetaH * a.sunAngularRadius, muS - cosThetaH);
}

float4 fetchScattering(Texture3D<float4> tex, float r, float mu, float muS, float nu, bool intersectsGround)
{
    float3 uvw0, uvw1;
    float lerpNu;
    scatteringLookup(geom(), r, mu, muS, nu, intersectsGround, uvw0, uvw1, lerpNu);
    return tex.SampleLevel(g_linear, uvw0, 0) * (1.0 - lerpNu) + tex.SampleLevel(g_linear, uvw1, 0) * lerpNu;
}

float3 fetchIrradiance(Texture2D<float4> tex, float r, float muS)
{
    return tex.SampleLevel(g_linear, irradianceUvFromRMuS(geom(), r, muS), 0).rgb;
}

// Radiance scattered `order` times arriving from the given direction.
float3 scatteringOfOrder(int order, float r, float mu, float muS, float nu, bool intersectsGround)
{
    if (order == 1) {
        float3 rayleigh = fetchScattering(g_deltaRayleigh, r, mu, muS, nu, intersectsGround).rgb;
        float3 mie = fetchScattering(g_deltaMie, r, mu, muS, nu, intersectsGround).rgb;
        return rayleigh * rayleighPhase(nu) + mie * miePhase(g_pc.mieExt.w, nu);
    }
    return fetchScattering(g_deltaMulti, r, mu, muS, nu, intersectsGround).rgb;
}

// ---- transmittance + irradiance ----

[shader("compute")]
[numthreads(8, 8, 1)]
void csTransmittance(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kTransmittanceW || id.y >= kTransmittanceH) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(kTransmittanceW, kTransmittanceH);
    float r, mu;
    rMuFromTransmittanceUv(geom(), uv, r, mu);
    float3 depth = g_pc.rayleigh.rgb * opticalLengthToTop(0, r, mu) + g_pc.mieExt.rgb * opticalLengthToTop(1, r, mu) +
                   g_pc.ozone.rgb * opticalLengthToTop(2, r, mu);
    g_out2D[id.xy] = float4(exp(-depth), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csDirectIrradiance(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kIrradianceW || id.y >= kIrradianceH) return;
    float r, muS;
    rMuSFromIrradianceUv(geom(), (float2(id.xy) + 0.5) / float2(kIrradianceW, kIrradianceH), r, muS);
    float alphaS = g_pc.radii.z;
    // Average of the cosine factor over the part of the sun disc above the horizon.
    float avgCos = muS < -alphaS ? 0.0 : (muS > alphaS ? muS : (muS + alphaS) * (muS + alphaS) / (4.0 * alphaS));
    g_out2D[id.xy] = float4(g_pc.solar.rgb * transmittanceToTop(r, muS) * avgCos, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csIndirectIrradiance(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kIrradianceW || id.y >= kIrradianceH) return;
    const int kSamples = 32;
    int order = int(g_pc.pass.x); // the scattering order being added to the irradiance
    float r, muS;
    rMuSFromIrradianceUv(geom(), (float2(id.xy) + 0.5) / float2(kIrradianceW, kIrradianceH), r, muS);

    float dphi = kAtmoPi / float(kSamples);
    float dtheta = kAtmoPi / float(kSamples);
    float3 omegaS = float3(sqrt(max(1.0 - muS * muS, 0.0)), 0.0, muS);
    float3 result = float3(0.0);
    for (int j = 0; j < kSamples / 2; ++j) {
        float theta = (float(j) + 0.5) * dtheta;
        for (int i = 0; i < 2 * kSamples; ++i) {
            float phi = (float(i) + 0.5) * dphi;
            float3 omega = float3(cos(phi) * sin(theta), sin(phi) * sin(theta), cos(theta));
            float domega = dtheta * dphi * sin(theta);
            float nu = dot(omega, omegaS);
            result += scatteringOfOrder(order, r, omega.z, muS, nu, false) * omega.z * domega;
        }
    }
    float4 delta = float4(result, 0.0);
    g_out2D[id.xy] = delta;
    g_outIrradiance[id.xy] = g_irradianceIn.Load(int3(int2(id.xy), 0)) + delta;
}

// ---- scattering ----

float3 integrateSingleScattering(float r, float mu, float muS, float nu, bool intersectsGround, bool mie)
{
    const int kSamples = 50;
    AtmoGeom a = geom();
    float dx = distanceToNearestBoundary(a, r, mu, intersectsGround) / float(kSamples);
    float3 sum = float3(0.0);
    for (int i = 0; i <= kSamples; ++i) {
        float d = float(i) * dx;
        float rd = clampRadius(a, sqrt(d * d + 2.0 * r * mu * d + r * r));
        float muSd = clampCosine((r * muS + d * nu) / rd);
        float3 t = transmittanceAlong(r, mu, d, intersectsGround) * transmittanceToSun(rd, muSd);
        float density = mie ? densityMie(rd - a.bottom) : densityRayleigh(rd - a.bottom);
        float w = (i == 0 || i == kSamples) ? 0.5 : 1.0;
        sum += t * density * w;
    }
    return sum * dx * g_pc.solar.rgb * (mie ? g_pc.mieScat.rgb : g_pc.rayleigh.rgb);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csSingleScattering(uint3 id : SV_DispatchThreadID)
{
    uint slice = id.z + g_pc.pass.y;
    if (id.x >= kScatteringW || id.y >= kScatteringMuSize || slice >= kScatteringRSize) return;
    uint3 texel = uint3(id.x, id.y, slice);
    float r, mu, muS, nu;
    bool ground;
    rMuMuSNuFromScatteringTexel(geom(), float3(texel) + 0.5, r, mu, muS, nu, ground);
    float3 rayleigh = integrateSingleScattering(r, mu, muS, nu, ground, false);
    float3 mie = integrateSingleScattering(r, mu, muS, nu, ground, true);
    g_outA[texel] = float4(rayleigh, 0.0);
    g_outB[texel] = float4(mie, 0.0);
    // Combined table: Rayleigh in rgb, the red channel of the Mie term in alpha (the rest is recovered at runtime).
    g_outScattering[texel] = float4(rayleigh, mie.r);
}

// Radiance scattered once more at each point of the atmosphere, from light arriving after order - 1 bounces
// (plus the light the ground reflects). g_outA receives the result.
[shader("compute")]
[numthreads(8, 8, 1)]
void csScatteringDensity(uint3 id : SV_DispatchThreadID)
{
    const int kSamples = 16;
    uint slice = id.z + g_pc.pass.y;
    if (id.x >= kScatteringW || id.y >= kScatteringMuSize || slice >= kScatteringRSize) return;
    AtmoGeom a = geom();
    int order = int(g_pc.pass.x);
    float r, mu, muS, nu;
    bool unusedGround;
    rMuMuSNuFromScatteringTexel(a, float3(id.x, id.y, slice) + 0.5, r, mu, muS, nu, unusedGround);

    float3 zenith = float3(0.0, 0.0, 1.0);
    float3 omega = float3(sqrt(max(1.0 - mu * mu, 0.0)), 0.0, mu);
    float sunX = omega.x == 0.0 ? 0.0 : (nu - mu * muS) / omega.x;
    float sunY = sqrt(max(1.0 - sunX * sunX - muS * muS, 0.0));
    float3 omegaS = float3(sunX, sunY, muS);

    float dphi = kAtmoPi / float(kSamples);
    float dtheta = kAtmoPi / float(kSamples);
    float h = r - a.bottom;
    float rayleighDensity = densityRayleigh(h);
    float mieDensity = densityMie(h);
    float3 sum = float3(0.0);
    for (int l = 0; l < kSamples; ++l) {
        float theta = (float(l) + 0.5) * dtheta;
        float cosTheta = cos(theta);
        float sinTheta = sin(theta);
        bool hitsGround = rayIntersectsGround(a, r, cosTheta);

        float distanceToGround = 0.0;
        float3 transmittanceToGround = float3(0.0);
        float3 groundAlbedo = float3(0.0);
        if (hitsGround) {
            distanceToGround = distanceToBottom(a, r, cosTheta);
            transmittanceToGround = transmittanceAlong(r, cosTheta, distanceToGround, true);
            groundAlbedo = g_pc.ground.rgb;
        }

        for (int m = 0; m < 2 * kSamples; ++m) {
            float phi = (float(m) + 0.5) * dphi;
            float3 omegaI = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
            float domegaI = dtheta * dphi * sinTheta;

            // Light arriving from omegaI after order - 1 bounces ...
            float nu1 = dot(omegaS, omegaI);
            float3 incident = scatteringOfOrder(order - 1, r, omegaI.z, muS, nu1, hitsGround);
            // ... plus what the ground reflects towards the point.
            if (hitsGround) {
                float3 groundNormal = normalize(zenith * r + omegaI * distanceToGround);
                float3 groundIrradiance = fetchIrradiance(g_deltaIrradiance, a.bottom, dot(groundNormal, omegaS));
                incident += transmittanceToGround * groundAlbedo * (1.0 / kAtmoPi) * groundIrradiance;
            }

            float nu2 = dot(omega, omegaI);
            sum += incident *
                   (g_pc.rayleigh.rgb * rayleighDensity * rayleighPhase(nu2) +
                    g_pc.mieScat.rgb * mieDensity * miePhase(g_pc.mieExt.w, nu2)) *
                   domegaI;
        }
    }
    g_outA[uint3(id.x, id.y, slice)] = float4(sum, 0.0);
}

// Integrates the scattering density along the view ray. g_outA = delta multiple scattering (radiance),
// g_outScattering = previous scattering table + delta / rayleigh phase (the table stores phase-free values).
[shader("compute")]
[numthreads(8, 8, 1)]
void csMultipleScattering(uint3 id : SV_DispatchThreadID)
{
    const int kSamples = 50;
    uint slice = id.z + g_pc.pass.y;
    if (id.x >= kScatteringW || id.y >= kScatteringMuSize || slice >= kScatteringRSize) return;
    AtmoGeom a = geom();
    float r, mu, muS, nu;
    bool ground;
    uint3 texel = uint3(id.x, id.y, slice);
    rMuMuSNuFromScatteringTexel(a, float3(texel) + 0.5, r, mu, muS, nu, ground);

    float dx = distanceToNearestBoundary(a, r, mu, ground) / float(kSamples);
    float3 sum = float3(0.0);
    for (int i = 0; i <= kSamples; ++i) {
        float d = float(i) * dx;
        float ri = clampRadius(a, sqrt(d * d + 2.0 * r * mu * d + r * r));
        float mui = clampCosine((r * mu + d) / ri);
        float muSi = clampCosine((r * muS + d * nu) / ri);
        float3 s = fetchScattering(g_deltaDensity, ri, mui, muSi, nu, ground).rgb * transmittanceAlong(r, mu, d, ground) * dx;
        float w = (i == 0 || i == kSamples) ? 0.5 : 1.0;
        sum += s * w;
    }
    g_outA[texel] = float4(sum, 0.0);
    g_outScattering[texel] = g_scatteringIn.Load(int4(int3(texel), 0)) + float4(sum / rayleighPhase(nu), 0.0);
}
