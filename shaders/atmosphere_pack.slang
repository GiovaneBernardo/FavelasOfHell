// Dumps a finished LUT as RGBA16F texels (x fastest) into a raw buffer, for the cache file
// (engine/renderer/src/atmosphere.cpp). Separate file from atmosphere_precompute.slang: different bindings.

Texture2D<float4> g_pack2D : register(t0);
Texture3D<float4> g_pack3D : register(t1);
RWByteAddressBuffer g_packOut : register(u0);

uint2 packHalf4(float4 v)
{
    return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPack2D(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    g_pack2D.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    g_packOut.Store2((id.y * w + id.x) * 8, packHalf4(g_pack2D.Load(int3(int2(id.xy), 0))));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPack3D(uint3 id : SV_DispatchThreadID)
{
    uint w, h, d;
    g_pack3D.GetDimensions(w, h, d);
    if (id.x >= w || id.y >= h || id.z >= d) return;
    g_packOut.Store2(((id.z * h + id.y) * w + id.x) * 8, packHalf4(g_pack3D.Load(int4(int3(id), 0))));
}
