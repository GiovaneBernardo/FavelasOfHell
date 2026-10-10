// Opaque geometry: fills the G-buffer (psOpaque / psOpaqueMasked) and the picking id target (psPick / psPickMasked).
//
// Material model: glTF metallic-roughness. Base color = texture * material factor * instance tint; the metal/roughness
// texture packs roughness in G and metallic in B (and, by convention, ambient occlusion in R of the occlusion texture,
// which is usually the same texture); normal maps are BC5 (xy stored, z reconstructed).
#include "common/view.slangh"
#include "common/gbuffer.slangh"
#include "common/material.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT; // xyz + bitangent sign (snorm bytes in the vertex buffer)
    // Per-instance: rows 0..2 of the world matrix (3x4), the tint color, the entity id and the material index.
    float4 row0 : INSTANCE0;
    float4 row1 : INSTANCE1;
    float4 row2 : INSTANCE2;
    float4 color : INSTANCE3;
    uint2 entity : ENTITY; // Entity::id (lo, hi), used by the picking pass
    uint material : MATERIAL;
};

struct VSOut {
    float4 position : SV_Position;
    float3 worldNormal : TEXCOORD0;
    float4 worldTangent : TEXCOORD1; // xyz + sign
    float2 uv : TEXCOORD2;
    float4 color : COLOR0;
    nointerpolation uint material : TEXCOORD3;
    nointerpolation uint2 entity : TEXCOORD4;
    float3 worldPos : TEXCOORD5; // terrain materials blend in world space
};

float3 rotateByRows(VSIn i, float3 v)
{
    return float3(dot(i.row0.xyz, v), dot(i.row1.xyz, v), dot(i.row2.xyz, v));
}

[shader("vertex")]
VSOut vsOpaque(VSIn i)
{
    float4 p = float4(i.position, 1.0);
    float3 world = float3(dot(i.row0, p), dot(i.row1, p), dot(i.row2, p));

    VSOut o;
    o.position = mul(g_view.viewProj, float4(world, 1.0));
    // Normals and tangents use the 3x3 part as is: exact for uniform scale (the inverse-transpose for
    // non-uniform scale is not applied; keep instance scale uniform or accept slightly skewed lighting).
    o.worldNormal = rotateByRows(i, i.normal);
    o.worldTangent = float4(rotateByRows(i, i.tangent.xyz), i.tangent.w);
    o.uv = i.uv;
    o.color = i.color;
    o.material = i.material;
    o.entity = i.entity;
    o.worldPos = world;
    return o;
}

// ---- terrain material ---------------------------------------------------------------------------------------------
// Four ground layers (grass, soil, rock, sand) blended by slope, height and noise from the world position; no UVs, so
// tiles and LOD levels agree. Albedo and normals come from the layer textures (rock is projected from three axes so
// cliffs do not stretch); a layer without a resident texture falls back to a plain colour.

float hash21(float2 p)
{
    uint2 q = asuint(int2(floor(p)));
    uint h = q.x * 374761393u + q.y * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return float(h ^ (h >> 16u)) * (1.0 / 4294967295.0);
}

float valueNoise(float2 p)
{
    float2 c = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(c), hash21(c + float2(1, 0)), f.x), lerp(hash21(c + float2(0, 1)), hash21(c + float2(1, 1)), f.x), f.y);
}

static const float3 kLayerFallback[4] = {float3(0.16, 0.25, 0.07), float3(0.28, 0.21, 0.15), float3(0.34, 0.33, 0.31), float3(0.55, 0.50, 0.38)};
static const float kLayerRoughness[4] = {0.92, 0.95, 0.86, 0.92};

uint layerAlbedoSlot(GpuMaterial m, uint l)
{
    return l == 0 ? m.baseColorTex : (l == 1 ? m.metalRoughTex : (l == 2 ? m.occlusionTex : m.emissiveTex));
}

uint layerNormalSlot(GpuMaterial m, uint l)
{
    return l == 0 ? m.normalTex : (l == 1 ? m.extra0 : (l == 2 ? m.extra1 : m.extra2));
}

// Albedo of one layer at a world position: a near and a far sample, so the repeat is not visible.
float3 layerAlbedo(GpuMaterial m, uint l, float3 wp, float3 tri, float tile)
{
    if ((m.flags & (kMaterialLayerAlbedo0 << l)) == 0) return kLayerFallback[l] * (0.8 + 0.4 * valueNoise(wp.xz * 0.9));
    uint slot = layerAlbedoSlot(m, l);
    float2 nearXZ = wp.xz / tile, farXZ = wp.xz / (tile * 7.31) + 0.37;
    float3 a = sampleTexture(slot, nearXZ).rgb;
    if (l == 2) {
        // Rock: three planar projections weighted by the normal, so vertical faces are not smeared.
        a = a * tri.y + sampleTexture(slot, wp.zy / tile).rgb * tri.x + sampleTexture(slot, wp.xy / tile).rgb * tri.z;
    }
    float3 b = sampleTexture(slot, farXZ).rgb;
    return lerp(a, b, 0.3);
}

// Offset (world space) that a layer's normal map adds to the surface normal.
float3 layerNormalOffset(GpuMaterial m, uint l, float3 wp, float3 tri, float tile)
{
    if ((m.flags & (kMaterialLayerNormal0 << l)) == 0) return 0.0;
    uint slot = layerNormalSlot(m, l);
    float2 xz = (sampleTexture(slot, wp.xz / tile).xy * 2.0 - 1.0) + 0.5 * (sampleTexture(slot, wp.xz / (tile * 7.31) + 0.37).xy * 2.0 - 1.0);
    float3 offset = float3(xz.x, 0.0, xz.y) * (l == 2 ? tri.y : 1.0);
    if (l == 2) {
        float2 zy = sampleTexture(slot, wp.zy / tile).xy * 2.0 - 1.0;
        float2 xy = sampleTexture(slot, wp.xy / tile).xy * 2.0 - 1.0;
        offset += float3(0.0, zy.y, zy.x) * tri.x + float3(xy.x, xy.y, 0.0) * tri.z;
    }
    return offset;
}

GBufferOut shadeTerrain(VSOut i, GpuMaterial m)
{
    float3 n = normalize(i.worldNormal);
    float3 wp = i.worldPos;
    float tile = max(m.terrainTile, 0.5);
    float slope = 1.0 - n.y; // 0 flat, ~0.3 at 45 degrees
    float h = wp.y;
    float n1 = valueNoise(wp.xz / 23.0), n2 = valueNoise(wp.xz / 7.0 + 11.0), n3 = valueNoise(wp.xz / 90.0 - 5.0);

    // Layer weights: rock on steep or very high ground, sand at the shore, soil on bare patches and moderate slopes.
    float rock = smoothstep(0.20, 0.38, slope + (n1 - 0.5) * 0.14 + smoothstep(300.0, 520.0, h) * 0.12);
    float sand = (1.0 - smoothstep(0.8, 5.0 + n1 * 3.0, h)) * (1.0 - rock);
    float soil = saturate(smoothstep(0.09, 0.22, slope + (n2 - 0.5) * 0.10) + smoothstep(0.72, 0.84, valueNoise(wp.xz / 37.0 + 8.0)) * 0.8) * (1.0 - rock) * (1.0 - sand);
    float grass = saturate(1.0 - rock - sand - soil);
    float4 w = float4(grass, soil, rock, sand);
    w /= max(dot(w, 1.0), 1e-4);

    float3 tri = pow(abs(n), 4.0);
    tri /= max(tri.x + tri.y + tri.z, 1e-4);

    float3 albedo = 0.0;
    float3 offset = 0.0;
    float roughness = 0.0;
    [unroll] for (uint l = 0; l < 4; ++l) {
        float wl = w[l];
        if (wl > 0.01) {
            albedo += layerAlbedo(m, l, wp, tri, tile) * wl;
            offset += layerNormalOffset(m, l, wp, tri, tile) * wl;
        }
        roughness += kLayerRoughness[l] * wl;
    }
    // Large-scale variation, wet sand at the waterline and the darker green of forest on the lower slopes.
    albedo *= 0.82 + 0.36 * n3;
    albedo *= lerp(1.0, 0.55, w.w * (1.0 - smoothstep(0.0, 1.6, h)));
    albedo *= lerp(1.0, float3(0.72, 0.84, 0.66), w.x * smoothstep(8.0, 40.0, h) * (1.0 - smoothstep(380.0, 520.0, h)));

    // Far forest: real trees exist only near the camera, so beyond them the grass slopes get a canopy look (mottled dark
    // greens at the scale of tree crowns) instead of bare lawn.
    float farFactor = smoothstep(380.0, 620.0, length(wp - g_view.cameraPos.xyz));
    float forest = w.x * smoothstep(8.0, 30.0, h) * (1.0 - smoothstep(400.0, 480.0, h)) * smoothstep(0.28, 0.55, valueNoise(wp.xz / 55.0 + 3.1)) * farFactor;
    float crowns = valueNoise(wp.xz / 7.5) * 0.6 + valueNoise(wp.xz / 3.1 + 5.0) * 0.4;
    albedo = lerp(albedo, lerp(float3(0.045, 0.10, 0.035), float3(0.10, 0.19, 0.055), crowns), forest);

    n = normalize(n + offset * 0.8);

    GBufferOut o;
    o.albedo = float4(albedo * i.color.rgb, 1.0);
    o.normal = octEncode(n);
    o.material = float4(roughness, 0.0, 1.0, 0.0);
    return o;
}

GBufferOut shade(VSOut i, bool frontFace, bool masked)
{
    GpuMaterial m = g_materials[i.material];
    if ((m.flags & kMaterialTerrain) != 0) return shadeTerrain(i, m);

    float4 baseTexel = sampleTexture(m.baseColorTex, i.uv);
    float alpha = m.baseColor.a * baseTexel.a;
    if (masked && alpha < m.alphaCutoff) discard;

    float3 n = normalize(i.worldNormal);
    // Double-sided materials: shade the back face as a front face. Foliage keeps its smooth crown normal on both sides.
    const bool foliage = (m.flags & kMaterialFoliage) != 0;
    if (!frontFace && !foliage) n = -n;
    if ((m.flags & kMaterialNormalMap) != 0) {
        float2 xy = (sampleTexture(m.normalTex, i.uv).xy * 2.0 - 1.0) * m.normalScale;
        float3 t = normalize(i.worldTangent.xyz - n * dot(n, i.worldTangent.xyz)); // re-orthogonalize after interpolation
        float3 b = cross(n, t) * i.worldTangent.w * (frontFace ? 1.0 : -1.0);
        float z = sqrt(saturate(1.0 - dot(xy, xy)));
        n = normalize(t * xy.x + b * xy.y + n * z);
    }

    // glTF packs roughness in G and metallic in B; occlusion is the R channel of its own texture (often the same one).
    float3 mr = sampleTexture(m.metalRoughTex, i.uv).rgb;
    float ao = m.occlusionTex == m.metalRoughTex ? mr.r : sampleTexture(m.occlusionTex, i.uv).r;
    ao = lerp(1.0, ao, m.occlusionStrength);

    GBufferOut o;
    o.albedo = float4(baseTexel.rgb * m.baseColor.rgb * i.color.rgb, 1.0);
    o.normal = octEncode(n);
    o.material = float4(mr.g * m.roughness, mr.b * m.metallic, ao, foliage ? m.translucency : 0.0);
    return o;
}

[shader("fragment")]
GBufferOut psOpaque(VSOut i, bool frontFace : SV_IsFrontFace)
{
    return shade(i, frontFace, false);
}

[shader("fragment")]
GBufferOut psOpaqueMasked(VSOut i, bool frontFace : SV_IsFrontFace)
{
    return shade(i, frontFace, true);
}

// Picking: writes the entity id instead of a color (only rendered on a click frame).
[shader("fragment")]
uint2 psPick(VSOut i) : SV_Target0
{
    return i.entity;
}

[shader("fragment")]
uint2 psPickMasked(VSOut i) : SV_Target0
{
    GpuMaterial m = g_materials[i.material];
    if (materialAlpha(m, i.uv) < m.alphaCutoff) discard;
    return i.entity;
}
