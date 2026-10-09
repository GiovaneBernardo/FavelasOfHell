// Pre-skinning (ADR 0009): deforms every skinned mesh block of the scene into one vertex buffer, in the MeshVertex
// layout, so the depth, shadow, G-buffer and picking passes draw characters like any other geometry. Skinning once
// here instead of in each pass's vertex shader pays for the work once instead of 1 (depth) + 4 (cascades) + 1
// (G-buffer) (+ 1 picking) times, and the result is what hardware ray tracing needs for a BLAS refit later.
//
// One thread per vertex, 64 per group. A job is one mesh block with one pose; the groups of all jobs are laid end to
// end and each group finds its job by binary search over `firstGroup`.
//
// Linear blend skinning with four influences. Normals and tangents use the rotation part of the blended matrix (exact
// for rigid bones, which is what animated characters use; scale would need the inverse transpose).

struct SkinJob {
    uint srcVertex;   // first vertex of the mesh block in the shared vertex buffer
    uint vertexCount;
    uint dstVertex;   // first output vertex
    uint palette;     // index of the character's first skinning matrix
    uint firstGroup;
    uint jobCount;    // repeated in every job
    uint pad0;
    uint pad1;
};

// Mirrors MeshVertex (36 bytes): the tangent is four snorm bytes.
struct Vertex {
    float3 position;
    float3 normal;
    float2 uv;
    uint tangent;
};

StructuredBuffer<SkinJob> g_jobs : register(t0);
StructuredBuffer<Vertex> g_source : register(t1);
StructuredBuffer<uint2> g_influences : register(t2); // x: four joint indices, y: four weights (unorm8)
StructuredBuffer<float4> g_palette : register(t3);   // three rows per matrix
RWStructuredBuffer<Vertex> g_skinned : register(u0);

float4 unpackSnorm4(uint packed)
{
    int4 b = int4(int(packed << 24) >> 24, int(packed << 16) >> 24, int(packed << 8) >> 24, int(packed) >> 24);
    return max(float4(b) / 127.0, -1.0);
}

uint packSnorm4(float4 v)
{
    int4 b = int4(round(clamp(v, -1.0, 1.0) * 127.0));
    return uint(b.x & 255) | (uint(b.y & 255) << 8) | (uint(b.z & 255) << 16) | (uint(b.w & 255) << 24);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csSkin(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint jobCount = g_jobs[0].jobCount;
    // Last job whose first group is <= this group.
    uint lo = 0, hi = jobCount;
    while (hi - lo > 1) {
        uint mid = (lo + hi) >> 1;
        if (g_jobs[mid].firstGroup <= group.x) lo = mid;
        else hi = mid;
    }
    SkinJob job = g_jobs[lo];
    uint local = (group.x - job.firstGroup) * 64 + thread.x;
    if (local >= job.vertexCount) return;

    Vertex v = g_source[job.srcVertex + local];
    uint2 influence = g_influences[job.srcVertex + local];

    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll]
    for (uint k = 0; k < 4; ++k) {
        float w = float((influence.y >> (8 * k)) & 255) * (1.0 / 255.0);
        if (w <= 0.0) continue;
        uint joint = (influence.x >> (8 * k)) & 255;
        uint m = (job.palette + joint) * 3;
        r0 += g_palette[m] * w;
        r1 += g_palette[m + 1] * w;
        r2 += g_palette[m + 2] * w;
    }

    float4 p = float4(v.position, 1.0);
    float4 t = unpackSnorm4(v.tangent);
    Vertex o;
    o.position = float3(dot(r0, p), dot(r1, p), dot(r2, p));
    o.normal = normalize(float3(dot(r0.xyz, v.normal), dot(r1.xyz, v.normal), dot(r2.xyz, v.normal)));
    o.uv = v.uv;
    float3 tangent = normalize(float3(dot(r0.xyz, t.xyz), dot(r1.xyz, t.xyz), dot(r2.xyz, t.xyz)));
    o.tangent = packSnorm4(float4(tangent, t.w));
    g_skinned[job.dstVertex + local] = o;
}
