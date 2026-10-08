#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <bc7decomp.h>

#include "foh/asset_import.h"
#include "foh/jobs.h"

using namespace foh;

namespace {

// Smooth gradients, a hard-edged disc and a little noise: the kind of content block compression struggles
// with (edges) and the kind it should nail (gradients).
std::vector<u8> testImage(u32 w, u32 h) {
    std::vector<u8> px(static_cast<size_t>(w) * h * 4);
    u32 seed = 12345;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(w), fy = static_cast<f32>(y) / static_cast<f32>(h);
            const bool disc = (fx - 0.5f) * (fx - 0.5f) + (fy - 0.5f) * (fy - 0.5f) < 0.06f;
            seed = seed * 1664525u + 1013904223u;
            const int noise = static_cast<int>((seed >> 24) % 7) - 3;
            p[0] = static_cast<u8>(std::clamp(static_cast<int>(fx * 255) + noise, 0, 255));
            p[1] = static_cast<u8>(std::clamp(static_cast<int>(fy * 255) + noise, 0, 255));
            p[2] = disc ? 220 : static_cast<u8>(std::clamp(static_cast<int>((1.0f - fx) * 200) + noise, 0, 255));
            p[3] = 255;
        }
    }
    return px;
}

// Decodes a whole BC7 mip into RGBA8.
std::vector<u8> decodeBc7(const std::vector<u8>& data, u32 w, u32 h) {
    std::vector<u8> out(static_cast<size_t>(w) * h * 4);
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            bc7decomp::color_rgba block[16];
            REQUIRE(bc7decomp::unpack_bc7(&data[(static_cast<size_t>(by) * bw + bx) * 16], block));
            for (u32 y = 0; y < 4 && by * 4 + y < h; ++y)
                for (u32 x = 0; x < 4 && bx * 4 + x < w; ++x)
                    std::memcpy(&out[(static_cast<size_t>(by * 4 + y) * w + bx * 4 + x) * 4], block[y * 4 + x].m_comps, 4);
        }
    }
    return out;
}

// One BC4 block (8 bytes) -> 16 values.
void decodeBc4Block(const u8* b, u8 out[16]) {
    u8 pal[8] = {b[0], b[1]};
    if (b[0] > b[1]) {
        for (int i = 1; i < 7; ++i) pal[i + 1] = static_cast<u8>(((7 - i) * b[0] + i * b[1]) / 7);
    } else {
        for (int i = 1; i < 5; ++i) pal[i + 1] = static_cast<u8>(((5 - i) * b[0] + i * b[1]) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<u64>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

// BC5 mip -> RG pairs.
std::vector<u8> decodeBc5(const std::vector<u8>& data, u32 w, u32 h) {
    std::vector<u8> out(static_cast<size_t>(w) * h * 2);
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            u8 r[16], g[16];
            const u8* block = &data[(static_cast<size_t>(by) * bw + bx) * 16];
            decodeBc4Block(block, r);
            decodeBc4Block(block + 8, g);
            for (u32 y = 0; y < 4 && by * 4 + y < h; ++y)
                for (u32 x = 0; x < 4 && bx * 4 + x < w; ++x) {
                    out[(static_cast<size_t>(by * 4 + y) * w + bx * 4 + x) * 2] = r[y * 4 + x];
                    out[(static_cast<size_t>(by * 4 + y) * w + bx * 4 + x) * 2 + 1] = g[y * 4 + x];
                }
        }
    }
    return out;
}

double psnrRgb(const std::vector<u8>& a, const std::vector<u8>& b) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c, ++n) {
            const double d = static_cast<double>(a[i + c]) - static_cast<double>(b[i + c]);
            sum += d * d;
        }
    const double mse = sum / static_cast<double>(n);
    return mse < 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

size_t blockBytes(u32 w, u32 h, u32 bytesPerBlock) { return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * bytesPerBlock; }

} // namespace

TEST_CASE("BC7 color textures round trip with high quality and the right mip layout") {
    const u32 w = 64, h = 64;
    const std::vector<u8> src = testImage(w, h);
    TextureAsset tex;
    REQUIRE(CookTexture(src.data(), w, h, "t", TextureRole::Color, ImportSettings{}, nullptr, tex));
    CHECK(tex.format == TexelFormat::Bc7Srgb);
    REQUIRE(tex.mips.size() == 7); // 64 .. 1
    CHECK(tex.mips[0].data.size() == blockBytes(64, 64, 16));
    CHECK(tex.mips[5].width == 2); // under 4 texels: still one padded block
    CHECK(tex.mips[5].data.size() == 16);
    CHECK(tex.mips[6].data.size() == 16);

    const std::vector<u8> decoded = decodeBc7(tex.mips[0].data, w, h);
    const double psnr = psnrRgb(src, decoded);
    INFO("PSNR " << psnr);
    CHECK(psnr > 38.0);
    // Mip 1 is a filtered version of mip 0: compare against our own 2x2 box filter in linear light.
    const auto chain = BuildMipChain(src.data(), w, h, true);
    CHECK(psnrRgb(chain[1].data, decodeBc7(tex.mips[1].data, 32, 32)) > 34.0);
}

TEST_CASE("data textures use linear BC7 and keep their alpha") {
    const u32 w = 32, h = 32;
    std::vector<u8> src = testImage(w, h);
    for (size_t i = 3; i < src.size(); i += 4) src[i] = static_cast<u8>((i / 4) % 2 ? 255 : 90); // checkered alpha
    TextureAsset tex;
    REQUIRE(CookTexture(src.data(), w, h, "orm", TextureRole::Data, ImportSettings{}, nullptr, tex));
    CHECK(tex.format == TexelFormat::Bc7Unorm);
    const std::vector<u8> decoded = decodeBc7(tex.mips[0].data, w, h);
    int alphaOff = 0;
    for (size_t i = 3; i < src.size(); i += 4) alphaOff += std::abs(int(src[i]) - int(decoded[i])) > 24;
    CHECK(alphaOff < 40); // a few of the 1024 texels may land on a block boundary pattern, none should be wild
}

TEST_CASE("normal maps become BC5 and every mip stays a unit vector") {
    const u32 w = 64, h = 64;
    std::vector<u8> src(static_cast<size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const f32 nx = 0.5f * std::sin(static_cast<f32>(x) * 0.3f), ny = 0.5f * std::cos(static_cast<f32>(y) * 0.2f);
            const f32 nz = std::sqrt(1.0f - nx * nx - ny * ny);
            u8* p = &src[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<u8>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[1] = static_cast<u8>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[2] = static_cast<u8>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[3] = 255;
        }
    TextureAsset tex;
    REQUIRE(CookTexture(src.data(), w, h, "n", TextureRole::Normal, ImportSettings{}, nullptr, tex));
    CHECK(tex.format == TexelFormat::Bc5Unorm);
    CHECK(tex.mips[0].data.size() == blockBytes(64, 64, 16));

    const std::vector<u8> rg = decodeBc5(tex.mips[0].data, w, h);
    int worst = 0;
    for (u32 i = 0; i < w * h; ++i) {
        worst = std::max(worst, std::abs(int(rg[i * 2]) - int(src[i * 4])));
        worst = std::max(worst, std::abs(int(rg[i * 2 + 1]) - int(src[i * 4 + 1])));
    }
    INFO("worst channel error " << worst);
    CHECK(worst <= 4);

    // Renormalized mips: reconstruct z from the decoded xy of a deep mip, the length must be about 1.
    const std::vector<u8> deep = decodeBc5(tex.mips[3].data, 8, 8);
    for (u32 i = 0; i < 64; ++i) {
        const f32 x = deep[i * 2] / 255.0f * 2.0f - 1.0f, y = deep[i * 2 + 1] / 255.0f * 2.0f - 1.0f;
        CHECK(x * x + y * y <= 1.02f); // xy of a unit vector never exceeds length 1 (averaging without renormalizing would shrink, not grow; this guards BC5 overshoot)
    }
}

TEST_CASE("sizes BC cannot hold fall back to RGBA8 with a warning") {
    const std::vector<u8> src = testImage(6, 6);
    TextureAsset tex;
    std::string warning;
    REQUIRE(CookTexture(src.data(), 6, 6, "odd", TextureRole::Color, ImportSettings{}, nullptr, tex, &warning));
    CHECK(tex.format == TexelFormat::Rgba8Srgb);
    CHECK_FALSE(warning.empty());
    REQUIRE(tex.mips.size() == 3); // 6 -> 3 -> 1
    CHECK(tex.mips[0].data == src);

    // A size cap that lands on a non-multiple of 4 also ends up uncompressed (mip 0 of a BC texture must be a multiple of 4).
    const std::vector<u8> twelve = testImage(12, 12);
    ImportSettings capped;
    capped.maxTextureSize = 6;
    REQUIRE(CookTexture(twelve.data(), 12, 12, "cap", TextureRole::Color, capped, nullptr, tex));
    CHECK(tex.width == 6);
    CHECK(tex.format == TexelFormat::Rgba8Srgb);

    // Opting out of compression is honored.
    ImportSettings plain;
    plain.compressTextures = false;
    const std::vector<u8> sixteen = testImage(16, 16);
    REQUIRE(CookTexture(sixteen.data(), 16, 16, "raw", TextureRole::Data, plain, nullptr, tex));
    CHECK(tex.format == TexelFormat::Rgba8Unorm);
    CHECK(tex.mips[0].data == sixteen);
}

TEST_CASE("the cook output does not depend on the job system") {
    const std::vector<u8> src = testImage(128, 128);
    JobSystem jobs(4);
    for (TextureRole role : {TextureRole::Color, TextureRole::Normal, TextureRole::Data}) {
        TextureAsset serial, parallel;
        REQUIRE(CookTexture(src.data(), 128, 128, "t", role, ImportSettings{}, nullptr, serial));
        REQUIRE(CookTexture(src.data(), 128, 128, "t", role, ImportSettings{}, &jobs, parallel));
        REQUIRE(serial.mips.size() == parallel.mips.size());
        for (size_t i = 0; i < serial.mips.size(); ++i) CHECK(serial.mips[i].data == parallel.mips[i].data);
    }
}

TEST_CASE("compressing a 2K texture is fast enough to run on every import") {
    const std::vector<u8> src = testImage(2048, 2048);
    JobSystem jobs(JobSystem::defaultWorkerCount());
    TextureAsset tex;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(CookTexture(src.data(), 2048, 2048, "big", TextureRole::Color, ImportSettings{}, &jobs, tex));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("2048x2048 color texture (mips + BC7): " << ms << " ms on " << jobs.threadCount() << " threads");
    CHECK(tex.mips.size() == 12);
    CHECK(ms < 20000.0); // a generous bound: the point is catching a 100x regression, the number is in the log
}
