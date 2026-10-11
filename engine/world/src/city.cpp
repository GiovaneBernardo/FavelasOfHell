#include "foh/world/city.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "city_internal.h"
#include "foh/log.h"
#include "foh/world/noise.h"

namespace foh {

using namespace city;

namespace {

// Natural vegetation stays off paving and sand (the surface mask of the ground patch).
class PatchMask final : public VegetationMask {
public:
    explicit PatchMask(std::shared_ptr<const GroundPatch> g) : m_ground(std::move(g)) {}
    f32 density(f32 x, f32 z) const override {
        if (!m_ground || !m_ground->contains(x, z)) return 1.0f;
        f32 s[3];
        m_ground->surfaceAt(x, z, s);
        return std::clamp((1.0f - s[0]) * (1.0f - s[1]), 0.0f, 1.0f);
    }

private:
    std::shared_ptr<const GroundPatch> m_ground;
};

constexpr AssetId kMaskTexture{kHi, 0x2F00ull};
constexpr u32 kTreeKinds = 4; // broadleaf variants; kind 4 is the palm

} // namespace

struct City::Impl {
    struct Chunk {
        i32 x = 0, z = 0;
        u32 lod = 0, wantLod = 0;
        std::vector<u32> sets;
        std::vector<AssetId> meshes;
        u64 triangles = 0;
        u32 buildings = 0;
        bool built = false, inFlight = false, stale = true;
        f32 dist = 0.0f;
    };
    struct Task {
        Impl* impl;
        const Terrain* terrain;
        CitySettings settings;
        i32 x, z;
        u32 lod;
        u64 generation;
    };
    struct Result {
        u64 key;
        u64 generation;
        ChunkParts parts;
    };

    Impl(Renderer& r, World& w, JobSystem* j) : renderer(r), jobs(j) {
        if (const RenderSceneLink* link = w.resource<RenderSceneLink>()) scene = link->scene;
    }

    Renderer& renderer;
    JobSystem* jobs;
    RenderScene* scene = nullptr;
    CityData data;
    CityIndex index;
    bool loaded = false, contentUp = false;
    std::unordered_map<u64, Chunk> chunks;
    std::mutex mutex;
    std::vector<Result> done;
    JobCounter counter;
    u32 inFlight = 0;
    u64 generation = 1;
    u32 meshSerial = 1;
    std::shared_ptr<VegetationPlants> plants;

    static void runTask(void* arg) {
        std::unique_ptr<Task> t(static_cast<Task*>(arg));
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        r.key = ChunkKey(t->x, t->z);
        r.generation = t->generation;
        r.parts.lod = t->lod;
        const CityIndex& index = t->impl->index;
        if (t->settings.roads) BuildRoadChunk(index, *t->terrain, t->x, t->z, t->lod, r.parts);
        if (t->settings.buildings) BuildBuildingChunk(index, *t->terrain, t->settings, t->x, t->z, t->lod, r.parts);
        if (t->settings.props && t->lod == 0) BuildPropChunk(index, *t->terrain, t->settings, t->x, t->z, r.parts);
        r.parts.ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard lock(t->impl->mutex);
        t->impl->done.push_back(std::move(r));
    }

    bool hasContent(i32 x, i32 z) const {
        const u64 k = ChunkKey(x, z);
        return index.edgesByChunk.count(k) || index.junctionsByChunk.count(k) || index.buildingsByChunk.count(k) || index.pointsByChunk.count(k);
    }

    void clearChunk(Chunk& c) {
        if (scene)
            for (const u32 id : c.sets) scene->removeStaticSet(id);
        for (const AssetId& m : c.meshes) renderer.releaseMeshAsset(m);
        c.sets.clear();
        c.meshes.clear();
        c.triangles = 0;
        c.buildings = 0;
        c.built = false;
    }

    void plant(Chunk& c, Result& r) {
        clearChunk(c);
        if (!scene) {
            c.built = true;
            return;
        }
        const Vec3 centre((f32(c.x) + 0.5f) * kChunk, 40.0f, (f32(c.z) + 0.5f) * kChunk);
        for (auto& [slot, m] : r.parts.parts) {
            if (m.empty()) continue;
            const AssetId id = ChunkMesh(meshSerial++);
            const MeshIndexRange whole{0, u32(m.idx.size())};
            if (!renderer.uploadMeshAsset(id, MeshGeometry{m.p, m.n, m.uv, m.t, m.idx}, std::span(&whole, 1))) {
                FOH_ERROR("city: could not upload a chunk mesh");
                continue;
            }
            Vec3 lo = m.p[0], hi = m.p[0];
            for (const Vec3& p : m.p) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            StaticInstanceSet set;
            set.mesh = id;
            set.material = MaterialId(slot);
            set.instances.push_back(MakeStaticInstance(Vec3(0.0f), 0.0f, 1.0f));
            set.center = (lo + hi) * 0.5f;
            set.radius = glm::length(hi - lo) * 0.5f + 1.0f;
            set.castsShadow = slot >= kFacade0 && slot != kWire && r.parts.lod == 0; // the ground does not need to cast, and far chunks are beyond the cascades
            set.inProbes = true;
            c.sets.push_back(scene->addStaticSet(std::move(set)));
            c.meshes.push_back(id);
            c.triangles += m.idx.size() / 3;
        }
        for (PropBatch& pb : r.parts.props) {
            if (pb.instances.empty()) continue;
            StaticInstanceSet set;
            set.mesh = PropMesh(pb.prop);
            set.material = MaterialId(pb.slot);
            set.instances = std::move(pb.instances);
            set.center = centre;
            set.radius = kChunk * 0.75f + 60.0f;
            set.castsShadow = true;
            c.sets.push_back(scene->addStaticSet(std::move(set)));
        }
        c.buildings = r.parts.buildings;
        c.lod = r.parts.lod;
        c.built = true;
        c.stale = false;
    }
};

City::City(Renderer& renderer, World& world, JobSystem* jobs) : m_impl(std::make_unique<Impl>(renderer, world, jobs)) {}

City::~City() {
    Impl& im = *m_impl;
    if (im.jobs) im.jobs->wait(im.counter);
    for (auto& [k, c] : im.chunks) im.clearChunk(c);
    if (im.contentUp) {
        ReleaseCityContent(im.renderer);
        im.renderer.releaseTexture(kMaskTexture);
    }
}

bool City::load(const std::filesystem::path& cityFile) {
    Impl& im = *m_impl;
    CityData d;
    if (!d.load(cityFile)) return false;
    if (im.jobs) im.jobs->wait(im.counter);
    for (auto& [k, c] : im.chunks) im.clearChunk(c);
    im.chunks.clear();
    im.inFlight = 0;
    im.done.clear();
    im.data = std::move(d);
    im.index = CityIndex{};
    im.index.data = &im.data;
    const CityData& data = im.data;

    BuildIndex(im.index);

    if (!im.contentUp) {
        UploadCityContent(im.renderer);
        im.contentUp = true;
    }

    // Plants: surveyed trees, street trees, park trees.
    auto plants = std::make_shared<VegetationPlants>();
    std::vector<Vec2> placed;
    auto tooClose = [&](Vec2 p, f32 r) {
        for (const Vec2& q : placed)
            if (glm::dot(p - q, p - q) < r * r) return true;
        return false;
    };
    auto insideBuilding = [&](Vec2 p) {
        const auto it = im.index.buildingGrid.find(CityIndex::GridKey(CityIndex::GridOf(p.x), CityIndex::GridOf(p.y)));
        if (it == im.index.buildingGrid.end()) return false;
        for (const u32 bi : it->second) {
            const CityBuilding& b = data.buildings[bi];
            if (p.x >= b.lo.x - 1.0f && p.x <= b.hi.x + 1.0f && p.y >= b.lo.y - 1.0f && p.y <= b.hi.y + 1.0f && InsideRings(b.rings, p)) return true;
        }
        return false;
    };
    auto addPlant = [&](Vec2 p, u32 salt, f32 scaleLo, f32 scaleHi, bool palm) {
        const f32 r1 = noise::Rand01(i32(p.x * 3.0f), i32(p.y * 3.0f), salt), r2 = noise::Rand01(i32(p.x * 3.0f), i32(p.y * 3.0f), salt + 1);
        VegetationPlant v;
        v.x = p.x;
        v.z = p.y;
        v.kind = palm ? u8(kTreeKinds) : u8(u8(r1 * f32(kTreeKinds)) % kTreeKinds);
        v.scale = scaleLo + (scaleHi - scaleLo) * r2;
        v.yaw = r2 * 6.2831853f;
        const f32 shade = 0.80f + 0.25f * r1;
        auto q = [](f32 a) { return u32(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f); };
        v.tint = q(shade * 0.95f) | (q(shade) << 8) | (q(shade * 0.88f) << 16) | 0xFF000000u;
        plants->add(v);
        placed.push_back(p);
    };
    for (const CityPoint& p : data.points)
        if (p.kind == PointKind::Tree && !insideBuilding(p.p)) addPlant(p.p, 11, 0.55f, 0.85f, false);
    const u32 surveyed = u32(placed.size());
    // Street trees: both sides of the streets, every 12-15 m, where the sidewalk is wide enough and nothing is built.
    for (size_t ei = 0; ei < data.edges.size(); ++ei) {
        const CityEdge& e = data.edges[ei];
        const EdgeGeo& g = im.index.edgeGeo[ei];
        if (!g.valid || e.cls == RoadClass::Trunk || u8(e.cls) > u8(RoadClass::Living)) continue;
        for (int side = -1; side <= 1; side += 2) {
            const f32 w = side > 0 ? e.sidewalkR : e.sidewalkL;
            const bool promenade = w > 6.5f;
            if (w < 2.0f) continue;
            const f32 offset = e.width * 0.5f + (promenade ? 0.9f : 1.0f);
            f32 s = g.tA + 4.0f + noise::Rand01(i32(ei), side, 5) * 6.0f;
            const f32 spacing = promenade ? 12.0f : 13.0f;
            while (s < g.length - g.tB - 4.0f) {
                const auto it = std::upper_bound(g.cum.begin(), g.cum.end(), s);
                const size_t seg = size_t(std::clamp<i64>(i64(it - g.cum.begin()) - 1, 0, i64(e.pts.size()) - 2));
                const f32 len = std::max(g.cum[seg + 1] - g.cum[seg], 1e-3f);
                const Vec2 dir = (e.pts[seg + 1] - e.pts[seg]) / len;
                const Vec2 pos = e.pts[seg] + dir * (s - g.cum[seg]) + RightOf(dir) * (f32(side) * offset);
                if (noise::Rand01(i32(ei) * 7 + side, i32(s), 6) < 0.78f && !tooClose(pos, 5.0f) && !insideBuilding(pos)) addPlant(pos, 21 + u32(ei), 0.5f, 0.78f, promenade && noise::Rand01(i32(s), side, 9) < 0.7f);
                s += spacing * (0.8f + 0.4f * noise::Rand01(i32(ei), i32(s), 7));
            }
        }
    }
    // Parks and gardens: a jittered grid inside the polygon.
    for (const CityArea& a : data.areas) {
        if (a.kind != AreaKind::Park && a.kind != AreaKind::Garden && a.kind != AreaKind::Grass) continue;
        if (a.rings.empty()) continue;
        Vec2 lo = a.rings[0][0], hi = a.rings[0][0];
        for (const Vec2& p : a.rings[0]) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        if ((hi.x - lo.x) * (hi.y - lo.y) < 400.0f || (hi.x - lo.x) > 1500.0f) continue; // tiny lawns stay bare; huge ones (the racecourse) are natural
        const f32 step = a.kind == AreaKind::Garden ? 14.0f : 9.5f;
        for (f32 z = lo.y; z < hi.y; z += step)
            for (f32 x = lo.x; x < hi.x; x += step) {
                const f32 jx = noise::Rand01(i32(x), i32(z), 31), jz = noise::Rand01(i32(x), i32(z), 32);
                const Vec2 p(x + jx * step, z + jz * step);
                if (noise::Rand01(i32(x), i32(z), 33) > 0.55f) continue;
                if (!InsideRings(a.rings, p) || insideBuilding(p) || im.index.distanceToStreet(p, 3.0f) < 2.5f || tooClose(p, 4.5f)) continue;
                addPlant(p, 41, 0.75f, 1.2f, noise::Rand01(i32(x), i32(z), 34) < 0.12f);
            }
    }
    im.plants = plants;
    im.loaded = true;
    FOH_INFO("city: {} trees placed ({} from the survey)", placed.size(), surveyed);
    m_stats = {};
    return true;
}

bool City::loaded() const { return m_impl->loaded; }
const CityData& City::data() const { return m_impl->data; }
std::shared_ptr<const VegetationPlants> City::plants() const { return m_impl->plants; }
std::shared_ptr<const VegetationMask> City::vegetationMask(std::shared_ptr<const GroundPatch> ground) const { return std::make_shared<PatchMask>(std::move(ground)); }

bool City::uploadSurfaceMask(const GroundPatch& ground, AssetId& id, Vec4& rect) {
    const u32 w = ground.surfaceWidth(), h = ground.surfaceHeight();
    if (w < 2 || h < 2) return false;
    TextureUpload up;
    up.id = kMaskTexture;
    up.format = TextureFormat::Rgba8Unorm;
    up.width = w;
    up.height = h;
    up.mips.push_back(ground.surface());
    u32 cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        const u32 nw = std::max(1u, cw / 2), nh = std::max(1u, ch / 2);
        const std::vector<u8>& s = up.mips.back();
        std::vector<u8> d(size_t(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (u32 c = 0; c < 4; ++c) {
                    u32 sum = 0, cnt = 0;
                    for (u32 dy = 0; dy < 2; ++dy)
                        for (u32 dx = 0; dx < 2; ++dx) {
                            const u32 sx = std::min(x * 2 + dx, cw - 1), sy = std::min(y * 2 + dy, ch - 1);
                            sum += s[(size_t(sy) * cw + sx) * 4 + c];
                            ++cnt;
                        }
                    d[(size_t(y) * nw + x) * 4 + c] = u8(sum / cnt);
                }
        up.mips.push_back(std::move(d));
        cw = nw;
        ch = nh;
    }
    if (!m_impl->renderer.uploadTexture(std::move(up))) return false;
    id = kMaskTexture;
    rect = Vec4(ground.minX(), ground.minZ(), 1.0f / (f32(w) * ground.surfaceCell()), 1.0f / (f32(h) * ground.surfaceCell()));
    return true;
}

Vec2 City::pushOutOfBuildings(Vec2 centre, f32 radius) const {
    const Impl& im = *m_impl;
    if (!im.loaded) return centre;
    for (int pass = 0; pass < 2; ++pass) {
        bool moved = false;
        const i32 x0 = CityIndex::GridOf(centre.x - radius), x1 = CityIndex::GridOf(centre.x + radius);
        const i32 z0 = CityIndex::GridOf(centre.y - radius), z1 = CityIndex::GridOf(centre.y + radius);
        for (i32 gz = z0; gz <= z1; ++gz)
            for (i32 gx = x0; gx <= x1; ++gx) {
                const auto it = im.index.buildingGrid.find(CityIndex::GridKey(gx, gz));
                if (it == im.index.buildingGrid.end()) continue;
                for (const u32 bi : it->second) {
                    const CityBuilding& b = im.data.buildings[bi];
                    if (b.kind == BuildingKind::Roof) continue; // canopies on poles: walk under them
                    if (centre.x < b.lo.x - radius || centre.x > b.hi.x + radius || centre.y < b.lo.y - radius || centre.y > b.hi.y + radius) continue;
                    const Vec2 pushed = PushOutOfFootprint(b.rings, centre, radius);
                    if (pushed != centre) {
                        centre = pushed;
                        moved = true;
                    }
                }
            }
        if (!moved) break;
    }
    return centre;
}

void City::update(const Terrain& terrain, Vec3 focus) {
    Impl& im = *m_impl;
    if (!im.loaded) return;
    // Plant what the jobs finished (a few per frame: uploads are on this thread).
    {
        std::vector<Impl::Result> done;
        {
            std::lock_guard lock(im.mutex);
            done.swap(im.done);
        }
        u32 budget = 6;
        std::vector<Impl::Result> later;
        for (Impl::Result& r : done) {
            if (budget == 0) {
                later.push_back(std::move(r));
                continue;
            }
            --budget;
            const auto it = im.chunks.find(r.key);
            if (im.inFlight > 0) --im.inFlight;
            if (it == im.chunks.end()) continue;
            Impl::Chunk& c = it->second;
            c.inFlight = false;
            if (r.generation != im.generation) {
                c.stale = true;
                continue;
            }
            im.plant(c, r);
            m_stats.lastChunkMs = r.parts.ms;
        }
        if (!later.empty()) {
            std::lock_guard lock(im.mutex);
            for (auto& r : later) im.done.push_back(std::move(r));
        }
    }

    const f32 range = settings.range, farRange = std::max(settings.farRange, settings.range);
    const f32 reach = settings.buildings || settings.roads ? farRange : range;
    const i32 x0 = ChunkOf(focus.x - reach), x1 = ChunkOf(focus.x + reach), z0 = ChunkOf(focus.z - reach), z1 = ChunkOf(focus.z + reach);
    std::vector<Impl::Chunk*> todo;
    m_stats.waiting = 0;
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) {
            const f32 cx = (f32(x) + 0.5f) * kChunk, cz = (f32(z) + 0.5f) * kChunk;
            const f32 d = std::sqrt((cx - focus.x) * (cx - focus.x) + (cz - focus.z) * (cz - focus.z));
            if (d > reach + kChunk * 0.71f || !im.hasContent(x, z)) continue;
            auto [it, inserted] = im.chunks.try_emplace(ChunkKey(x, z));
            Impl::Chunk& c = it->second;
            if (inserted) {
                c.x = x;
                c.z = z;
            }
            c.dist = d;
            // Full detail inside `range`, the far version out to farRange; the switch has hysteresis so a chunk on the border does not flicker.
            u32 want = c.built ? c.lod : (d <= range ? 0u : 1u);
            if (c.built && c.lod == 1 && d < range - 30.0f) want = 0;
            if (c.built && c.lod == 0 && d > range + 60.0f) want = 1;
            if (want != c.lod && c.built) c.stale = true;
            c.wantLod = want;
            if ((!c.built || c.stale) && !c.inFlight) todo.push_back(&c);
            else if (c.inFlight) ++m_stats.waiting;
        }
    std::sort(todo.begin(), todo.end(), [](const Impl::Chunk* a, const Impl::Chunk* b) { return a->dist < b->dist; });
    m_stats.waiting += u32(todo.size());
    const u32 maxInFlight = im.jobs ? 6u : 1u;
    for (Impl::Chunk* c : todo) {
        if (im.inFlight >= maxInFlight) break;
        c->inFlight = true;
        c->stale = false;
        ++im.inFlight;
        --m_stats.waiting;
        auto* task = new Impl::Task{&im, &terrain, settings, c->x, c->z, c->wantLod, im.generation};
        if (im.jobs) im.jobs->submit(&Impl::runTask, task, im.counter, JobPriority::Low);
        else Impl::runTask(task);
    }
    // Chunks behind the camera go, with hysteresis.
    for (auto it = im.chunks.begin(); it != im.chunks.end();) {
        Impl::Chunk& c = it->second;
        const f32 cx = (f32(c.x) + 0.5f) * kChunk, cz = (f32(c.z) + 0.5f) * kChunk;
        const f32 d = std::sqrt((cx - focus.x) * (cx - focus.x) + (cz - focus.z) * (cz - focus.z));
        if (d > reach + kChunk * 1.5f + 60.0f && !c.inFlight) {
            im.clearChunk(c);
            it = im.chunks.erase(it);
        } else {
            ++it;
        }
    }
    m_stats.chunks = 0;
    m_stats.triangles = 0;
    m_stats.buildings = 0;
    for (const auto& [k, c] : im.chunks)
        if (c.built) {
            ++m_stats.chunks;
            m_stats.triangles += c.triangles;
            m_stats.buildings += c.buildings;
        }
}

void City::rebuildAll() {
    Impl& im = *m_impl;
    ++im.generation;
    for (auto& [k, c] : im.chunks) c.stale = true;
}

} // namespace foh
