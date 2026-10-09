// Tileable noise volumes for the volumetric clouds, generated once at start-up (engine/renderer/src/clouds.cpp):
//   csShape    128^3 RGBA8   R = Perlin-Worley (the billowy base), GBA = Worley at 3 frequencies (inverted)
//   csDetail    32^3 RGBA8   RGB = Worley at 3 frequencies, erodes the edges of the shape
// (the 2D weather map is clouds_weather.slang). The target dimensions come from GetDimensions.
#include "common/noise.slangh"

RWTexture3D<float4> g_out3D : register(u0);

[shader("compute")]
[numthreads(4, 4, 4)]
void csShape(uint3 id : SV_DispatchThreadID)
{
    uint w, h, d;
    g_out3D.GetDimensions(w, h, d);
    if (any(id >= uint3(w, h, d))) return;
    float3 uvw = (float3(id) + 0.5) / float3(w, h, d);

    float w1 = worley(uvw, 4), w2 = worley(uvw, 8), w3 = worley(uvw, 16);
    float pw = remap(perlinFbm(uvw, 4), w1 * 0.625 + w2 * 0.25 + w3 * 0.125 - 1.0, 1.0, 0.0, 1.0);
    g_out3D[id] = float4(saturate(pw), w1, w2, w3);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void csDetail(uint3 id : SV_DispatchThreadID)
{
    uint w, h, d;
    g_out3D.GetDimensions(w, h, d);
    if (any(id >= uint3(w, h, d))) return;
    float3 uvw = (float3(id) + 0.5) / float3(w, h, d);
    g_out3D[id] = float4(worley(uvw, 2), worley(uvw, 4), worley(uvw, 8), 1.0);
}
