// Texture cooking: size cap, mip chain, block compression.
//
// Levels are produced one at a time (decode -> encode level -> downsample -> ...), so peak memory is
// the source image plus two levels instead of the whole chain, and every stage that touches pixels
// runs through ParallelFor. BC7 comes from bc7enc_rdo, BC5 from rgbcx (third_party/).

#include <algorithm>
#include <cmath>
#include <mutex>

#include <bc7enc.h>
#include <rgbcx.h>

#include "foh/asset_import.h"

namespace foh {

namespace {

// ---- color space tables ----------------------------------------------------------------------

struct ColorTables {
    f32 srgbToLinear[256];
    u8 linearToSrgb[65536]; // indexed by round(linear * 65535)
    ColorTables() {
        for (int i = 0; i < 256; ++i) {
            const f32 c = static_cast<f32>(i) / 255.0f;
            srgbToLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int i = 0; i < 65536; ++i) {
            const f32 v = static_cast<f32>(i) / 65535.0f;
            const f32 c = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            linearToSrgb[i] = static_cast<u8>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
};
const ColorTables& tables() {
    static const ColorTables t; // thread-safe initialization
    return t;
}

u8 toSrgb8(f32 linear) { return tables().linearToSrgb[static_cast<u32>(std::clamp(linear, 0.0f, 1.0f) * 65535.0f + 0.5f)]; }

enum class Filter { Linear, Srgb, Normal };

// 2x2 box filter. Rows are independent, so they are split across the pool.
void downsample(const u8* src, u32 sw, u32 sh, std::vector<u8>& dst, u32& dw, u32& dh, Filter filter, JobSystem* jobs) {
    dw = std::max(sw / 2, 1u);
    dh = std::max(sh / 2, 1u);
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    const f32* lut = tables().srgbToLinear;
    auto rows = [&](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            const u32 ya = std::min(2 * y, sh - 1), yb = std::min(2 * y + 1, sh - 1);
            for (u32 x = 0; x < dw; ++x) {
                const u32 xa = std::min(2 * x, sw - 1), xb = std::min(2 * x + 1, sw - 1);
                const u8* t[4] = {src + (static_cast<size_t>(ya) * sw + xa) * 4, src + (static_cast<size_t>(ya) * sw + xb) * 4,
                                  src + (static_cast<size_t>(yb) * sw + xa) * 4, src + (static_cast<size_t>(yb) * sw + xb) * 4};
                u8* o = &dst[(static_cast<size_t>(y) * dw + x) * 4];
                if (filter == Filter::Srgb) {
                    for (int c = 0; c < 3; ++c) o[c] = toSrgb8((lut[t[0][c]] + lut[t[1][c]] + lut[t[2][c]] + lut[t[3][c]]) * 0.25f);
                } else if (filter == Filter::Normal) {
                    // Average the vectors and renormalize: plain averaging shortens them, which makes
                    // distant normal-mapped surfaces look smoother (and BC5 reconstructs z from unit xy).
                    f32 v[3] = {0, 0, 0};
                    for (int k = 0; k < 4; ++k)
                        for (int c = 0; c < 3; ++c) v[c] += static_cast<f32>(t[k][c]) * (2.0f / 255.0f) - 1.0f;
                    const f32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    for (int c = 0; c < 3; ++c) {
                        const f32 n = len > 1e-6f ? v[c] / len : (c == 2 ? 1.0f : 0.0f);
                        o[c] = static_cast<u8>(std::clamp((n * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    }
                } else {
                    for (int c = 0; c < 3; ++c) o[c] = static_cast<u8>((t[0][c] + t[1][c] + t[2][c] + t[3][c] + 2) / 4);
                }
                o[3] = static_cast<u8>((t[0][3] + t[1][3] + t[2][3] + t[3][3] + 2) / 4); // alpha is always linear
            }
        }
    };
    // ~16K output texels per chunk keeps small levels inline.
    ParallelFor(jobs, dh, std::max(1u, 16384u / std::max(dw, 1u)), rows);
}

// ---- block compression -------------------------------------------------------------------------

void initEncoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

// Gathers the 4x4 block at (bx, by); pixels past the edge repeat the edge (only mips under 4 texels).
void gatherBlock(const u8* rgba, u32 w, u32 h, u32 bx, u32 by, u8 block[64]) {
    for (u32 y = 0; y < 4; ++y) {
        const u32 sy = std::min(by * 4 + y, h - 1);
        for (u32 x = 0; x < 4; ++x) {
            const u32 sx = std::min(bx * 4 + x, w - 1);
            std::memcpy(block + (y * 4 + x) * 4, rgba + (static_cast<size_t>(sy) * w + sx) * 4, 4);
        }
    }
}

void encodeLevel(const u8* rgba, u32 w, u32 h, TexelFormat format, JobSystem* jobs, std::vector<u8>& out) {
    initEncoders();
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    const u32 blockBytes = format == TexelFormat::Bc4Unorm ? 8u : 16u;
    out.resize(static_cast<size_t>(bw) * bh * blockBytes);

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    // Speed over the last fraction of a dB: the cook runs on every texture of every import. Modes 0-7
    // stay enabled (alpha and smooth gradients need them); partitions and uber level are what cost time.
    params.m_max_partitions = 16;
    params.m_uber_level = 0;
    if (format == TexelFormat::Bc7Srgb) bc7enc_compress_block_params_init_perceptual_weights(&params);
    else bc7enc_compress_block_params_init_linear_weights(&params);

    auto rows = [&](u32 y0, u32 y1) {
        u8 block[64];
        for (u32 by = y0; by < y1; ++by) {
            for (u32 bx = 0; bx < bw; ++bx) {
                gatherBlock(rgba, w, h, bx, by, block);
                u8* dst = &out[(static_cast<size_t>(by) * bw + bx) * blockBytes];
                switch (format) {
                    case TexelFormat::Bc7Unorm:
                    case TexelFormat::Bc7Srgb: bc7enc_compress_block(dst, block, &params); break;
                    case TexelFormat::Bc5Unorm: rgbcx::encode_bc5(dst, block, 0, 1, 4); break;
                    case TexelFormat::Bc4Unorm: rgbcx::encode_bc4(dst, block, 4); break;
                    default: break;
                }
            }
        }
    };
    ParallelFor(jobs, bh, std::max(1u, 32u / std::max(bw, 1u)), rows);
}

} // namespace

std::vector<TextureMip> BuildMipChain(const u8* rgba, u32 width, u32 height, bool srgb, JobSystem* jobs) {
    std::vector<TextureMip> chain;
    TextureMip base;
    base.width = width;
    base.height = height;
    base.data.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    chain.push_back(std::move(base));
    while (chain.back().width > 1 || chain.back().height > 1) {
        TextureMip next;
        downsample(chain.back().data.data(), chain.back().width, chain.back().height, next.data, next.width, next.height,
                   srgb ? Filter::Srgb : Filter::Linear, jobs);
        chain.push_back(std::move(next));
    }
    return chain;
}

bool CookTexture(const u8* rgba, u32 width, u32 height, const std::string& name, TextureRole role, const ImportSettings& settings,
                 JobSystem* jobs, TextureAsset& out, std::string* warning) {
    if (width == 0 || height == 0 || width > (1u << 15) || height > (1u << 15)) return false;

    const bool blockOk = width % 4 == 0 && height % 4 == 0;
    const bool compress = settings.compressTextures && blockOk;
    if (settings.compressTextures && !blockOk && warning)
        *warning = "texture '" + name + "' is " + std::to_string(width) + "x" + std::to_string(height) +
                   " (not a multiple of 4): stored uncompressed";

    TexelFormat format;
    Filter filter;
    switch (role) {
        case TextureRole::Color:
            format = compress ? TexelFormat::Bc7Srgb : TexelFormat::Rgba8Srgb;
            filter = Filter::Srgb;
            break;
        case TextureRole::Normal:
            format = compress ? TexelFormat::Bc5Unorm : TexelFormat::Rgba8Unorm;
            filter = Filter::Normal;
            break;
        default:
            format = compress ? TexelFormat::Bc7Unorm : TexelFormat::Rgba8Unorm;
            filter = Filter::Linear;
            break;
    }
    const bool blockFormat = format != TexelFormat::Rgba8Srgb && format != TexelFormat::Rgba8Unorm;

    TextureAsset tex;
    tex.name = name;
    tex.role = role;
    tex.format = format;

    // One level at a time: `cur` points at the level being emitted, `bufA/bufB` ping-pong as downsample targets.
    std::vector<u8> bufA, bufB;
    const u8* cur = rgba;
    u32 w = width, h = height;
    bool emitted = false;
    for (;;) {
        const bool capped = settings.maxTextureSize != 0 && std::max(w, h) > settings.maxTextureSize && (w > 1 || h > 1);
        if (!capped) {
            TextureMip mip;
            mip.width = w;
            mip.height = h;
            if (blockFormat) encodeLevel(cur, w, h, format, jobs, mip.data);
            else mip.data.assign(cur, cur + static_cast<size_t>(w) * h * 4);
            if (!emitted) {
                tex.width = w;
                tex.height = h;
            }
            tex.mips.push_back(std::move(mip));
            emitted = true;
            if (!settings.generateMips) break;
        }
        if (w == 1 && h == 1) break;
        std::vector<u8>& target = cur == bufA.data() ? bufB : bufA;
        u32 nw, nh;
        downsample(cur, w, h, target, nw, nh, filter, jobs);
        cur = target.data();
        w = nw;
        h = nh;
    }
    if (tex.mips.empty()) return false;

    // The size cap can land on a base level that is not a multiple of 4 (non power of two sources):
    // block formats need that for mip 0, so such a texture is stored uncompressed instead.
    if (blockFormat && (tex.width % 4 != 0 || tex.height % 4 != 0)) {
        ImportSettings plain = settings;
        plain.compressTextures = false;
        return CookTexture(rgba, width, height, name, role, plain, jobs, out, warning);
    }
    out = std::move(tex);
    return true;
}

} // namespace foh
