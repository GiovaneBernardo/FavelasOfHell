#include <doctest/doctest.h>

#include <cstdint>

#include "foh/jobs.h"
#include "foh/rhi/render_graph.h"

using namespace foh;
using nvrhi::ResourceStates;

// These tests exercise the part of the render graph that needs no GPU: culling,
// transient lifetimes and reuse, command list grouping and barrier planning.
// compile() only compares resource pointers, it never dereferences them, so fake
// non-null imports are enough. Recording/submission is covered by the
// editor/runtime smoke runs on a real device.

namespace {

nvrhi::ITexture* fakeTexture(std::uintptr_t id) { return reinterpret_cast<nvrhi::ITexture*>(id * 0x1000); }

class CountingFactory final : public RgResourceFactory {
public:
    nvrhi::TextureHandle createTexture(const RgTextureDesc&) override {
        ++textures;
        return nullptr; // the graph tolerates null handles when nothing is recorded
    }
    nvrhi::BufferHandle createBuffer(const RgBufferDesc&) override {
        ++buffers;
        return nullptr;
    }
    int textures = 0;
    int buffers = 0;
};

RgTextureDesc rt(u32 w, u32 h, const char* name) {
    RgTextureDesc d;
    d.width = w;
    d.height = h;
    d.format = nvrhi::Format::RGBA16_FLOAT;
    d.renderTarget = true;
    d.name = name;
    return d;
}

struct Empty {};
auto noop = [](const Empty&, RgPassContext&) {};

} // namespace

TEST_CASE("render graph culls passes whose outputs nobody uses") {
    RenderGraph g;
    CountingFactory f;
    g.reset(1);
    const RgTexture out = g.importTexture(fakeTexture(1), ResourceStates::ShaderResource,
                                          ResourceStates::ShaderResource, "out");
    RgTexture a, unused;
    g.addPass<Empty>("produce", [&](RgPassBuilder& b, Empty&) { a = b.colorTarget(0, b.createTexture(rt(64, 64, "a"))); }, noop);
    g.addPass<Empty>("orphan", [&](RgPassBuilder& b, Empty&) { unused = b.colorTarget(0, b.createTexture(rt(64, 64, "u"))); }, noop);
    g.addPass<Empty>("consume", [&](RgPassBuilder& b, Empty&) {
        b.read(a);
        b.colorTarget(0, out);
    }, noop);
    g.addPass<Empty>("debug", [&](RgPassBuilder& b, Empty&) {
        b.colorTarget(0, b.createTexture(rt(8, 8, "dbg")));
        b.neverCull();
    }, noop);
    g.compile(f);

    const auto infos = g.passInfos();
    REQUIRE(infos.size() == 4);
    CHECK_FALSE(infos[0].culled);
    CHECK(infos[1].culled);
    CHECK_FALSE(infos[2].culled);
    CHECK_FALSE(infos[3].culled); // NeverCull keeps it although nothing reads its output
    CHECK(g.stats().culledPasses == 1);
    CHECK(infos[1].group == kRgInvalid);
}

TEST_CASE("a pass feeding only a culled pass is culled too") {
    RenderGraph g;
    CountingFactory f;
    g.reset(1);
    const RgTexture out = g.importTexture(fakeTexture(1), ResourceStates::Common, ResourceStates::Common, "out");
    RgTexture mid, dead;
    g.addPass<Empty>("live-input", [&](RgPassBuilder& b, Empty&) { mid = b.colorTarget(0, b.createTexture(rt(16, 16, "mid"))); }, noop);
    g.addPass<Empty>("dead-chain-1", [&](RgPassBuilder& b, Empty&) { dead = b.colorTarget(0, b.createTexture(rt(16, 16, "d1"))); }, noop);
    g.addPass<Empty>("dead-chain-2", [&](RgPassBuilder& b, Empty&) {
        b.read(dead);
        b.colorTarget(0, b.createTexture(rt(16, 16, "d2")));
    }, noop);
    g.addPass<Empty>("final", [&](RgPassBuilder& b, Empty&) {
        b.read(mid);
        b.colorTarget(0, out);
    }, noop);
    g.compile(f);
    const auto infos = g.passInfos();
    CHECK_FALSE(infos[0].culled);
    CHECK(infos[1].culled);
    CHECK(infos[2].culled);
    CHECK_FALSE(infos[3].culled);
    CHECK(g.stats().physicalTextures == 1); // only "mid" was ever allocated
}

TEST_CASE("transients with disjoint lifetimes share memory; overlapping ones do not") {
    CountingFactory f;
    RenderGraph g;

    auto build = [&](bool overlap) {
        g.reset(1);
        const RgTexture out = g.importTexture(fakeTexture(2), ResourceStates::Common, ResourceStates::Common, "out");
        RgTexture t1, t2;
        g.addPass<Empty>("p1", [&](RgPassBuilder& b, Empty&) { t1 = b.colorTarget(0, b.createTexture(rt(32, 32, "t1"))); }, noop);
        if (!overlap) {
            // t1 dies here (last use), t2 is created afterwards.
            g.addPass<Empty>("p2", [&](RgPassBuilder& b, Empty&) {
                b.read(t1);
                b.colorTarget(0, out);
            }, noop);
        }
        g.addPass<Empty>("p3", [&](RgPassBuilder& b, Empty&) { t2 = b.colorTarget(0, b.createTexture(rt(32, 32, "t2"))); }, noop);
        g.addPass<Empty>("p4", [&](RgPassBuilder& b, Empty&) {
            b.read(t2);
            if (overlap) b.read(t1);
            b.colorTarget(0, out);
        }, noop);
        g.compile(f);
    };

    build(false);
    CHECK(g.stats().transientTextures == 2);
    CHECK(g.stats().physicalTextures == 1);
    CHECK(f.textures == 1);

    build(true);
    CHECK(g.stats().physicalTextures == 2);
    CHECK(f.textures == 2); // one more than before, the first is pooled and reused
}

TEST_CASE("the pool survives frames: a repeated graph creates nothing new") {
    CountingFactory f;
    RenderGraph g;
    for (u64 frame = 1; frame <= 5; ++frame) {
        g.reset(frame);
        const RgTexture out = g.importTexture(fakeTexture(3), ResourceStates::Common, ResourceStates::Common, "out");
        RgTexture t;
        g.addPass<Empty>("p1", [&](RgPassBuilder& b, Empty&) { t = b.colorTarget(0, b.createTexture(rt(128, 128, "t"))); }, noop);
        g.addPass<Empty>("p2", [&](RgPassBuilder& b, Empty&) {
            b.read(t);
            b.colorTarget(0, out);
        }, noop);
        g.compile(f);
    }
    CHECK(f.textures == 1);

    // A resolution change allocates the new size and retires the old one later.
    for (u64 frame = 6; frame <= 12; ++frame) {
        g.reset(frame);
        const RgTexture out = g.importTexture(fakeTexture(3), ResourceStates::Common, ResourceStates::Common, "out");
        RgTexture t;
        g.addPass<Empty>("p1", [&](RgPassBuilder& b, Empty&) { t = b.colorTarget(0, b.createTexture(rt(256, 128, "t"))); }, noop);
        g.addPass<Empty>("p2", [&](RgPassBuilder& b, Empty&) {
            b.read(t);
            b.colorTarget(0, out);
        }, noop);
        g.compile(f);
    }
    CHECK(f.textures == 2);
    CHECK(g.stats().pooledTextures == 1); // the 128x128 one was retired
}

TEST_CASE("barriers follow the declared accesses, and imports end in their final state") {
    CountingFactory f;
    RenderGraph g;
    g.reset(1);
    const RgTexture backbuffer =
        g.importTexture(fakeTexture(4), ResourceStates::Present, ResourceStates::Present, "backbuffer");
    RgTexture scene;
    g.addPass<Empty>("draw", [&](RgPassBuilder& b, Empty&) { scene = b.colorTarget(0, b.createTexture(rt(64, 64, "scene"))); }, noop);
    g.addPass<Empty>("post", [&](RgPassBuilder& b, Empty&) {
        b.read(scene);
        b.colorTarget(0, backbuffer);
    }, noop);
    g.compile(f);

    CHECK(g.finalStateOf(backbuffer) == ResourceStates::Present);
    CHECK(g.finalStateOf(scene) == ResourceStates::ShaderResource);
    // Common -> RenderTarget, RenderTarget -> ShaderResource (scene);
    // Present -> RenderTarget, RenderTarget -> Present (backbuffer).
    CHECK(g.stats().barrierTransitions == 4);
}

TEST_CASE("large graphs split into contiguous recording groups") {
    JobSystem jobs(2);
    RenderGraphConfig cfg;
    cfg.jobs = &jobs;
    cfg.maxRecordingGroups = 3;
    cfg.minPassesPerGroup = 4;
    RenderGraph g(cfg);
    CountingFactory f;

    g.reset(1);
    const RgTexture out = g.importTexture(fakeTexture(5), ResourceStates::Common, ResourceStates::Common, "out");
    for (int i = 0; i < 12; ++i) {
        g.addPass<Empty>("step", [&](RgPassBuilder& b, Empty&) { b.write(out, RgAccess::StorageWrite); }, noop);
    }
    g.compile(f);
    CHECK(g.stats().groups == 3);
    const auto infos = g.passInfos();
    u32 last = 0;
    for (const auto& p : infos) {
        CHECK(p.group >= last); // groups are contiguous slices in declaration order
        last = p.group;
    }
    CHECK(infos.front().group == 0);
    CHECK(infos.back().group == 2);

    // A small graph stays one group even with a job system.
    g.reset(2);
    const RgTexture out2 = g.importTexture(fakeTexture(5), ResourceStates::Common, ResourceStates::Common, "out");
    for (int i = 0; i < 3; ++i) {
        g.addPass<Empty>("step", [&](RgPassBuilder& b, Empty&) { b.write(out2, RgAccess::StorageWrite); }, noop);
    }
    g.compile(f);
    CHECK(g.stats().groups == 1);
}

TEST_CASE("access states map to the expected NVRHI states") {
    CHECK(ToResourceStates(RgAccess::SampledRead) == ResourceStates::ShaderResource);
    CHECK(ToResourceStates(RgAccess::ColorTarget) == ResourceStates::RenderTarget);
    CHECK(ToResourceStates(RgAccess::DepthWrite) == ResourceStates::DepthWrite);
    CHECK(ToResourceStates(RgAccess::DepthRead) == ResourceStates::DepthRead);
    CHECK(ToResourceStates(RgAccess::CopyDst) == ResourceStates::CopyDest);
    CHECK(IsWriteAccess(RgAccess::StorageWrite));
    CHECK_FALSE(IsWriteAccess(RgAccess::VertexRead));
}
