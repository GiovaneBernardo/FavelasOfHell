// Weather map of the volumetric clouds, generated once at start-up: 256^2 RGBA8, R = coverage noise,
// G = cloud type / height noise. Tiles in x and y.
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
    g_out2D[id.xy] = float4(saturate(remap(coverage, cellular * 0.3 - 0.3, 1.0, 0.0, 1.0)), type, 0.0, 1.0);
}
