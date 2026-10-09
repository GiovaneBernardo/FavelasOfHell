// HDR scene color -> display-referred sRGB, written to an RGBA8 UNORM target. Applies, in order: the bloom composite
// (an energy-conserving blend with the blurred chain from bloom.slang), the exposure (manual, or the value the
// auto-exposure passes adapted), the selected tone mapping operator, the sRGB encode and a dither against banding.
#include "common/view.slangh"
#include "common/fullscreen.slangh"
#include "common/post.slangh"

ConstantBuffer<ViewConstants> g_view : register(b0);
ConstantBuffer<PostConstants> g_post : register(b1);
Texture2D<float4> g_hdr : register(t0);
Texture2D<float4> g_bloom : register(t1); // the scene itself when bloom is off (strength 0)
StructuredBuffer<float4> g_exposure : register(t2);
SamplerState g_linear : register(s0);

// Keep in sync with foh::Tonemapper (renderer.h).
static const int kLinear = 0;
static const int kReinhard = 1;
static const int kReinhardExtended = 2;
static const int kUncharted2 = 3;
static const int kAces = 4;
static const int kPbrNeutral = 5;

// Narkowicz's fit of the ACES filmic curve.
float3 acesFit(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// Reinhard on the luminance with a white point: colors keep their hue, `white` maps to 1.
float3 reinhardExtended(float3 c, float white)
{
    float l = luminance(c);
    float scaled = l * (1.0 + l / (white * white)) / (1.0 + l);
    return saturate(c * (scaled / max(l, 1e-6)));
}

// John Hable's filmic curve (Uncharted 2), with the usual exposure bias and a white point.
float3 hable(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float3 uncharted2(float3 c, float white)
{
    const float bias = 2.0;
    return saturate(hable(c * bias) / hable(float3(white, white, white)));
}

// Khronos PBR Neutral: leaves colors below ~0.8 untouched (what you author is what you see) and only compresses
// the highlights, desaturating them slightly.
float3 pbrNeutral(float3 c)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return saturate(lerp(c, newPeak * float3(1, 1, 1), g));
}

float3 tonemap(float3 c, int op, float white)
{
    if (op == kReinhard) return c / (1.0 + c);
    if (op == kReinhardExtended) return reinhardExtended(c, white);
    if (op == kUncharted2) return uncharted2(c, white);
    if (op == kAces) return acesFit(c);
    if (op == kPbrNeutral) return pbrNeutral(c);
    return saturate(c);
}

float3 linearToSrgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// Interleaved gradient noise (Jimenez 2014): a cheap blue-ish dither that hides the 8-bit banding of smooth
// gradients such as the sky.
float ditherNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 psTonemap(FullscreenOut i) : SV_Target0
{
    float3 hdr = g_hdr.Load(int3(int2(i.position.xy), 0)).rgb;
    if (g_post.bloom.x > 0.0) {
        float3 bloom = g_bloom.SampleLevel(g_linear, i.position.xy * g_view.viewport.zw, 0).rgb;
        hdr = lerp(hdr, bloom, g_post.bloom.x);
    }
    float exposure = g_post.exposure.y > 0.5 ? g_exposure[0].x : g_post.exposure.x;
    float3 srgb = linearToSrgb(tonemap(hdr * exposure, (int)g_post.tonemap.x, g_post.tonemap.y));
    return float4(srgb + (ditherNoise(i.position.xy) - 0.5) * (g_view.render.x / 255.0), 1.0);
}
