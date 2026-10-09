// Weather map of the volumetric clouds, generated once at start-up: 256^2 RGBA8, tiles in x and y.
//   R  coverage noise: where clouds form (blobby, with gaps)
//   G  type noise: pushes a column toward stratus or cumulus around the global weather
//   B  height noise: how tall the cloud column grows (towers vs. flat tops)
//   A  large-scale coverage: wide clear and cloudy regions, so the sky is not uniformly patchy
#include "common/noise.slangh"

RWTexture2D<float4> g_out2D : register(u0);

[shader("compute")]
[numthreads(8, 8, 1)]
void csWeather(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    g_out2D.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    // A slice of the 3D functions: periodic in x and y, constant in z.
    float3 uvw = float3((float2(id.xy) + 0.5) / float2(w, h), 0.37);
    float coverage = perlinFbm(uvw, 3);
    float cellular = worley(uvw, 6);
    float type = perlinFbm(uvw + float3(0.0, 0.0, 0.31), 2);
    float height = perlinFbm(uvw + float3(0.17, 0.43, 0.58), 4);
    float region = perlinFbm(uvw + float3(0.61, 0.09, 0.22), 2);
    float regionCells = worley(uvw + float3(0.3, 0.2, 0.1), 3);
    g_out2D[id.xy] = float4(saturate(remap(coverage, cellular * 0.3 - 0.3, 1.0, 0.0, 1.0)), type, height,
                            saturate(remap(region, regionCells * 0.35 - 0.35, 1.0, 0.0, 1.0)));
}
