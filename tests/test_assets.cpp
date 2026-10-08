#include <doctest/doctest.h>

#include "gtabr/asset_container.h"
#include "gtabr/gpu_format.h"
#include "gtabr/physics_system.h"

using namespace gtabr;

TEST_CASE("Compression bounds are sane") {
    CHECK(CompressBound(ChunkCodec::None, 1000) == 1000);
    CHECK(CompressBound(ChunkCodec::Lz4, 1000) >= 1000);
    CHECK(CompressBound(ChunkCodec::Zstd, 1000) >= 1000);
}

TEST_CASE("NVRHI is linked and reports format sizes") {
    CHECK(BytesPerBlock(GpuFormat::Rgba8Unorm) == 4);
    CHECK(BytesPerBlock(GpuFormat::Rgba16Float) == 8);
    CHECK(BytesPerBlock(GpuFormat::Bc7Unorm) == 16);
}

TEST_CASE("Jolt initializes and shuts down") {
    PhysicsSystem::Init();
    PhysicsSystem::Shutdown();
}
