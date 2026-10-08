#include "foh/ecs/snapshot.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "foh/assert.h"
#include "foh/log.h"
#include "schema_ops.h"
#include "world_impl.h"

namespace foh {

namespace {

constexpr char kMagic[4] = {'F', 'O', 'H', 'S'};
constexpr u32 kVersion = 1;
constexpr u32 kNoBuffer = 0xFFFFFFFFu; // buffer count marker: "this handle was empty"

// Limits for untrusted input: a snapshot may come from a mod or a download.
constexpr u32 kMaxSchemas = 1u << 16;
constexpr u32 kMaxFields = 1u << 12;
constexpr u32 kMaxDepth = 16;
constexpr u32 kMaxName = 1u << 12;
constexpr u32 kMaxValueSize = 1u << 24;

using schema::Fields;

// ---- byte streams ---------------------------------------------------------------

struct Writer {
    std::vector<std::byte>& out;
    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <class T>
    void put(T v) { raw(&v, sizeof(T)); }
    void str(const std::string& s) {
        put<u32>(static_cast<u32>(s.size()));
        raw(s.data(), s.size());
    }
};

struct Reader {
    const std::byte* p;
    const std::byte* end;
    bool ok = true;

    size_t remaining() const { return static_cast<size_t>(end - p); }
    bool bytes(void* dst, size_t n) {
        if (!ok || remaining() < n) return ok = false;
        if (dst) std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    template <class T>
    T get() {
        T v{};
        bytes(&v, sizeof(T));
        return v;
    }
    std::string str() {
        const u32 n = get<u32>();
        if (!ok || n > kMaxName || remaining() < n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

// ---- schema (de)serialization -----------------------------------------------------

void writeField(Writer& w, const SchemaField& f) {
    w.str(f.name);
    w.str(f.formerName);
    w.put<u8>(static_cast<u8>(f.kind));
    w.put<u32>(f.offset);
    w.put<u32>(f.count);
    w.put<u32>(f.elemSize);
    w.put<u32>(f.childSize);
    w.put<u32>(static_cast<u32>(f.children.size()));
    for (const SchemaField& c : f.children) writeField(w, c);
}

void writeSchema(Writer& w, const Schema& s) {
    w.str(s.name);
    w.put<u32>(s.size);
    w.put<u32>(s.alignment);
    w.put<u8>(s.reflected ? 1 : 0);
    w.put<u32>(static_cast<u32>(s.fields.size()));
    for (const SchemaField& f : s.fields) writeField(w, f);
}

bool readField(Reader& r, SchemaField& f, u32 depth) {
    if (depth > kMaxDepth) return false;
    f.name = r.str();
    f.formerName = r.str();
    const u8 kind = r.get<u8>();
    f.offset = r.get<u32>();
    f.count = r.get<u32>();
    f.elemSize = r.get<u32>();
    f.childSize = r.get<u32>();
    const u32 children = r.get<u32>();
    if (!r.ok || kind > static_cast<u8>(FieldKind::Buffer) || children > kMaxFields) return false;
    f.kind = static_cast<FieldKind>(kind);
    f.children.resize(children);
    for (SchemaField& c : f.children) {
        if (!readField(r, c, depth + 1)) return false;
    }
    // Everything the walkers multiply must be sane before they ever see it.
    if (f.count == 0 || f.count > kMaxValueSize || f.elemSize > kMaxValueSize || f.childSize > kMaxValueSize) return false;
    return true;
}

// The size every kind of leaf must have; the walkers copy that many bytes.
u32 canonicalSize(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: case FieldKind::I8: case FieldKind::U8: case FieldKind::Text: return 1;
        case FieldKind::I16: case FieldKind::U16: return 2;
        case FieldKind::I32: case FieldKind::U32: case FieldKind::F32: return 4;
        case FieldKind::I64: case FieldKind::U64: case FieldKind::F64: case FieldKind::Entity:
        case FieldKind::Vec2: case FieldKind::Buffer: return 8;
        case FieldKind::Vec3: return 12;
        case FieldKind::Vec4: case FieldKind::Quat: return 16;
        case FieldKind::Struct: return 0; // any size
    }
    return 0;
}

// Every field must lie inside its container (the value, a nested struct, or one
// buffer element). The walkers do pointer arithmetic from these numbers, so a
// crafted file must not be able to point them outside the data.
bool validateFields(const Fields& fields, u32 containerSize) {
    for (const SchemaField& f : fields) {
        if (f.kind != FieldKind::Struct && f.elemSize != canonicalSize(f.kind)) return false;
        const u64 count = f.kind == FieldKind::Text ? 1 : f.count;
        const u64 end = u64{f.offset} + u64{f.kind == FieldKind::Text ? f.count : f.elemSize * count};
        const u64 span = f.kind == FieldKind::Text ? end : u64{f.offset} + u64{f.elemSize} * count;
        if (span > containerSize) return false;
        if (f.kind == FieldKind::Struct && !validateFields(f.children, f.elemSize)) return false;
        if (f.kind == FieldKind::Buffer && (f.childSize == 0 || !validateFields(f.children, f.childSize))) return false;
    }
    return true;
}

bool readSchema(Reader& r, Schema& s) {
    s.name = r.str();
    s.size = r.get<u32>();
    s.alignment = r.get<u32>();
    s.reflected = r.get<u8>() != 0;
    const u32 fields = r.get<u32>();
    if (!r.ok || s.size > kMaxValueSize || fields > kMaxFields) return false;
    s.fields.resize(fields);
    for (SchemaField& f : s.fields) {
        if (!readField(r, f, 0)) return false;
    }
    return r.ok && validateFields(s.fields, s.size);
}

// ---- values (struct bytes followed by the blobs of the buffers they own) ----------------

void writeValue(Writer& w, const BufferPool& pool, const Fields& fields, u32 size, const std::byte* src) {
    w.raw(src, size);
    schema::visitLeaves(fields, const_cast<std::byte*>(src), [&](const SchemaField& f, std::byte* p) {
        if (f.kind != FieldKind::Buffer) return;
        u64 handle;
        std::memcpy(&handle, p, sizeof(handle));
        if (!handle || !pool.isValid(handle) || pool.elemSize(handle) != f.childSize) {
            w.put<u32>(kNoBuffer);
            return;
        }
        const u32 n = pool.size(handle);
        w.put<u32>(n);
        const auto* data = static_cast<const std::byte*>(pool.data(handle, f.childSize));
        for (u32 i = 0; i < n; ++i) {
            writeValue(w, pool, f.children, f.childSize, data + static_cast<size_t>(i) * f.childSize);
        }
    });
}

// Reads one value into dst (size bytes). With dry == true nothing is created and dst
// is only scratch: this walks the stream to validate it.
bool readValue(Reader& r, BufferPool& pool, const Fields& fields, u32 size, std::byte* dst, bool dry) {
    if (!r.bytes(dst, size)) return false;
    bool ok = true;
    schema::visitLeaves(fields, dst, [&](const SchemaField& f, std::byte* p) {
        if (!ok || f.kind != FieldKind::Buffer) return;
        const u32 n = r.get<u32>();
        if (!r.ok) {
            ok = false;
            return;
        }
        u64 handle = 0;
        if (n != kNoBuffer) {
            // Every element takes at least its raw bytes: reject counts the data cannot hold.
            if (f.childSize == 0 || u64{n} * f.childSize > r.remaining()) {
                ok = false;
                return;
            }
            std::byte* data = nullptr;
            std::vector<std::byte> scratch;
            if (!dry) {
                handle = pool.create(f.childSize, nullptr, n);
                data = static_cast<std::byte*>(pool.data(handle, f.childSize));
            } else {
                scratch.resize(f.childSize);
                data = scratch.data();
            }
            for (u32 i = 0; i < n && ok; ++i) {
                std::byte* element = dry ? data : data + static_cast<size_t>(i) * f.childSize;
                ok = readValue(r, pool, f.children, f.childSize, element, dry);
            }
            if (!ok && !dry) {
                // Do not leak what was built before the failure.
                for (u32 i = 0; i < n; ++i) {
                    schema::releaseBuffers(pool, f.children, data + static_cast<size_t>(i) * f.childSize);
                }
                pool.release(handle);
                handle = 0;
            }
        }
        std::memcpy(p, &handle, sizeof(handle));
    });
    return ok && r.ok;
}

bool hasEntityFields(const Fields& fields) {
    for (const SchemaField& f : fields) {
        if (f.kind == FieldKind::Entity) return true;
        if ((f.kind == FieldKind::Struct || f.kind == FieldKind::Buffer) && hasEntityFields(f.children)) return true;
    }
    return false;
}

} // namespace

// ---- save ------------------------------------------------------------------------------

std::vector<std::byte> saveSnapshot(const World& world, const SnapshotOptions& options) {
    World::Impl& impl = WorldAccess::impl(world);
    ecs_world_t* w = impl.world;

    std::vector<ecs_entity_t> entities;
    {
        ecs_query_desc_t qd{};
        qd.terms[0].id = impl.spawnedTag;
        qd.cache_kind = EcsQueryCacheNone;
        ecs_query_t* q = ecs_query_init(w, &qd);
        ecs_iter_t it = ecs_query_iter(w, q);
        while (ecs_query_next(&it)) {
            for (i32 i = 0; i < it.count; ++i) entities.push_back(it.entities[i]);
        }
        ecs_query_fini(q);
    }

    std::vector<ecs_entity_t> schemaOrder;
    std::unordered_map<ecs_entity_t, u32> schemaIndex;
    auto indexOf = [&](ecs_entity_t component) {
        const auto [it, inserted] = schemaIndex.try_emplace(component, static_cast<u32>(schemaOrder.size()));
        if (inserted) schemaOrder.push_back(component);
        return it->second;
    };

    // Sorted by name so the output does not depend on hash order.
    std::vector<ecs_entity_t> sparseComponents;
    for (const auto& [id, info] : impl.components) {
        if (info.sparse) sparseComponents.push_back(id);
    }
    std::sort(sparseComponents.begin(), sparseComponents.end(), [&](ecs_entity_t a, ecs_entity_t b) {
        return impl.components.at(a).schema.name < impl.components.at(b).schema.name;
    });

    std::vector<std::byte> body;
    Writer bw{body};
    bw.put<u64>(entities.size());
    for (const ecs_entity_t e : entities) {
        struct CompRec { ecs_entity_t id; };
        struct PairRec { ecs_entity_t relation; ecs_entity_t target; };
        std::vector<CompRec> comps;
        std::vector<PairRec> pairs;
        ecs_entity_t parent = 0;
        const ecs_type_t* type = ecs_get_type(w, e);
        for (i32 i = 0; type && i < type->count; ++i) {
            const ecs_id_t id = type->array[i];
            if (ECS_IS_PAIR(id)) {
                const ecs_entity_t first = ecs_pair_first(w, id);
                const ecs_entity_t second = ecs_pair_second(w, id);
                if (first == EcsChildOf) parent = second;
                else if (impl.components.count(first) && ecs_has_id(w, second, impl.spawnedTag)) pairs.push_back({first, second});
            } else if (id != impl.spawnedTag && impl.components.count(id)) {
                comps.push_back({id});
            }
        }
        // Sparse components are not part of the entity's type: ask for each one.
        for (const ecs_entity_t id : sparseComponents) {
            if (ecs_has_id(w, e, id)) comps.push_back({id});
        }
        bw.put<u64>(e);
        bw.put<u64>(parent);
        bw.put<u32>(static_cast<u32>(comps.size()));
        bw.put<u32>(static_cast<u32>(pairs.size()));
        for (const CompRec& c : comps) {
            const Schema& s = impl.components.at(c.id).schema;
            bw.put<u32>(indexOf(c.id));
            if (s.size > 0) {
                const auto* value = static_cast<const std::byte*>(ecs_get_id(w, e, c.id));
                writeValue(bw, impl.buffers, s.fields, s.size, value);
            }
        }
        for (const PairRec& p : pairs) {
            bw.put<u32>(indexOf(p.relation));
            bw.put<u64>(p.target);
        }
    }

    // Resources, sorted by name so the output does not depend on hash order.
    std::vector<ecs_entity_t> resources;
    if (options.includeResources) {
        for (const auto& [id, info] : impl.components) {
            if (info.schema.size > 0 && ecs_has_id(w, id, id)) resources.push_back(id);
        }
        std::sort(resources.begin(), resources.end(), [&](ecs_entity_t a, ecs_entity_t b) {
            return impl.components.at(a).schema.name < impl.components.at(b).schema.name;
        });
    }
    bw.put<u32>(static_cast<u32>(resources.size()));
    for (const ecs_entity_t id : resources) {
        const Schema& s = impl.components.at(id).schema;
        bw.put<u32>(indexOf(id));
        writeValue(bw, impl.buffers, s.fields, s.size, static_cast<const std::byte*>(ecs_get_id(w, id, id)));
    }

    std::vector<std::byte> out;
    Writer ow{out};
    ow.raw(kMagic, sizeof(kMagic));
    ow.put<u32>(kVersion);
    ow.put<u32>(static_cast<u32>(schemaOrder.size()));
    for (const ecs_entity_t id : schemaOrder) writeSchema(ow, impl.components.at(id).schema);
    ow.raw(body.data(), body.size());
    return out;
}

// ---- load ------------------------------------------------------------------------------

namespace {

struct FileSchema {
    Schema file;
    ecs_entity_t current = 0;       // registered component with this name, or 0
    const Schema* currentSchema = nullptr;
    enum class Mode { Unknown, Same, Convert } mode = Mode::Unknown;
    bool hasEntities = false;
};

struct Loader {
    World& world;
    World::Impl& impl;
    std::vector<FileSchema>& schemas;
    Reader r;
    bool dry;
    SnapshotReport& report;

    std::unordered_map<u64, u64> idMap; // saved id -> new id
    struct Fixup { ecs_entity_t entity; u32 schema; };
    std::vector<Fixup> entityFixups;
    struct ParentFix { u64 child, parent; };
    std::vector<ParentFix> parents;
    struct PairFix { u64 entity; ecs_entity_t relation; u64 target; };
    std::vector<PairFix> pairs;

    // Reads one saved value for schema `index`. Returns the value in the CURRENT
    // layout in `out` (empty when the component is unknown/skipped).
    bool readComponent(u32 index, std::vector<std::byte>& out, bool& usable) {
        FileSchema& fs = schemas[index];
        usable = false;
        const u32 fileSize = fs.file.size;
        std::vector<std::byte> raw(fileSize);
        if (!readValue(r, impl.buffers, fs.file.fields, fileSize, raw.data(), dry)) return false;
        if (dry) return true;
        if (fs.mode == FileSchema::Mode::Unknown) {
            schema::releaseBuffers(impl.buffers, fs.file.fields, raw.data());
            return true;
        }
        if (fs.mode == FileSchema::Mode::Same) {
            out = std::move(raw);
        } else {
            out.assign(fs.currentSchema->size, std::byte{0});
            schema::convertRecord(impl.buffers, fs.file.fields, raw.data(), fs.currentSchema->fields, out.data());
        }
        usable = true;
        return true;
    }

    bool run() {
        const u64 entityCount = r.get<u64>();
        if (!r.ok || entityCount > (1ull << 32)) return false;
        for (u64 n = 0; n < entityCount; ++n) {
            const u64 savedId = r.get<u64>();
            const u64 savedParent = r.get<u64>();
            const u32 compCount = r.get<u32>();
            const u32 pairCount = r.get<u32>();
            if (!r.ok || compCount > schemas.size() + 1 || pairCount > (1u << 20)) return false;

            Entity e{};
            if (!dry) {
                e = world.spawn();
                idMap[savedId] = e.id;
                ++report.entities;
                if (savedParent) parents.push_back({savedId, savedParent});
            }
            for (u32 c = 0; c < compCount; ++c) {
                const u32 index = r.get<u32>();
                if (!r.ok || index >= schemas.size()) return false;
                FileSchema& fs = schemas[index];
                std::vector<std::byte> value;
                bool usable = false;
                if (fs.file.size > 0) {
                    if (!readComponent(index, value, usable)) return false;
                } else {
                    usable = !dry && fs.mode != FileSchema::Mode::Unknown; // tag
                }
                if (dry) continue;
                if (!usable) {
                    ++report.skippedComponents;
                    continue;
                }
                world.setComponent(e, fs.current, value.empty() ? nullptr : value.data(), static_cast<u32>(value.size()));
                ++report.components;
                if (fs.mode == FileSchema::Mode::Convert) ++report.migratedComponents;
                if (fs.hasEntities) entityFixups.push_back({e.id, index});
            }
            for (u32 p = 0; p < pairCount; ++p) {
                const u32 index = r.get<u32>();
                const u64 target = r.get<u64>();
                if (!r.ok || index >= schemas.size()) return false;
                if (!dry && schemas[index].mode != FileSchema::Mode::Unknown) {
                    pairs.push_back({savedId, schemas[index].current, target});
                }
            }
        }

        const u32 resourceCount = r.get<u32>();
        if (!r.ok || resourceCount > schemas.size()) return false;
        std::vector<std::pair<u32, std::vector<std::byte>>> resources;
        for (u32 n = 0; n < resourceCount; ++n) {
            const u32 index = r.get<u32>();
            if (!r.ok || index >= schemas.size()) return false;
            std::vector<std::byte> value;
            bool usable = false;
            if (!readComponent(index, value, usable)) return false;
            if (!dry && usable && !value.empty()) resources.emplace_back(index, std::move(value));
            else if (!dry) ++report.skippedComponents;
        }
        if (r.remaining() != 0) return false; // trailing garbage
        if (dry) return true;

        // All entities exist now: resolve every reference to them.
        for (const Fixup& f : entityFixups) {
            FileSchema& fs = schemas[f.schema];
            if (auto* value = static_cast<std::byte*>(world.componentPtr(Entity{f.entity}, fs.current))) {
                schema::remapEntities(impl.buffers, fs.currentSchema->fields, value, idMap);
            }
        }
        for (const ParentFix& p : parents) {
            const auto child = idMap.find(p.child);
            const auto parent = idMap.find(p.parent);
            if (child != idMap.end() && parent != idMap.end()) world.setParent(Entity{child->second}, Entity{parent->second});
        }
        for (const PairFix& p : pairs) {
            const auto from = idMap.find(p.entity);
            const auto to = idMap.find(p.target);
            if (from != idMap.end() && to != idMap.end()) world.addPair(Entity{from->second}, p.relation, Entity{to->second});
        }
        for (auto& [index, value] : resources) {
            FileSchema& fs = schemas[index];
            if (fs.hasEntities) schema::remapEntities(impl.buffers, fs.currentSchema->fields, value.data(), idMap);
            // Replacing a resource that owns buffers must free the old ones first.
            if (auto* old = static_cast<std::byte*>(world.resourcePtr(fs.current))) {
                schema::releaseBuffers(impl.buffers, fs.currentSchema->fields, old);
            }
            world.setResource(fs.current, value.data(), static_cast<u32>(value.size()));
            ++report.resources;
        }
        return true;
    }
};

} // namespace

SnapshotReport loadSnapshot(World& world, std::span<const std::byte> data, const SnapshotOptions&) {
    SnapshotReport report;
    World::Impl& impl = WorldAccess::impl(world);
    FOH_VERIFY(!world.inParallelPhase(), "loadSnapshot during the parallel phase");

    Reader r{data.data(), data.data() + data.size()};
    char magic[4];
    r.bytes(magic, 4);
    if (!r.ok || std::memcmp(magic, kMagic, 4) != 0) {
        FOH_ERROR("Snapshot rejected: not a snapshot file");
        return report;
    }
    if (r.get<u32>() != kVersion) {
        FOH_ERROR("Snapshot rejected: unsupported version");
        return report;
    }
    const u32 schemaCount = r.get<u32>();
    if (!r.ok || schemaCount > kMaxSchemas) return report;

    std::vector<FileSchema> schemas(schemaCount);
    for (FileSchema& fs : schemas) {
        if (!readSchema(r, fs.file)) {
            FOH_ERROR("Snapshot rejected: malformed schema table");
            return report;
        }
        fs.hasEntities = hasEntityFields(fs.file.fields);
        // Find the component this build knows under that name.
        const ecs_entity_t id = ecs_lookup_symbol(impl.world, fs.file.name.c_str(), false, false);
        const auto known = id ? impl.components.find(id) : impl.components.end();
        if (known == impl.components.end()) continue;
        fs.current = id;
        fs.currentSchema = &known->second.schema;
        const Schema& cur = known->second.schema;
        if (sameLayout(fs.file, cur)) {
            fs.mode = FileSchema::Mode::Same;
        } else if (fs.file.reflected && cur.reflected) {
            fs.mode = FileSchema::Mode::Convert;
        } else if (fs.file.size == cur.size && fs.file.alignment == cur.alignment) {
            fs.mode = FileSchema::Mode::Same; // opaque on one side: trust an unchanged size
        } else {
            FOH_WARN("Snapshot component '{}' cannot be converted (no field information); skipped", fs.file.name);
        }
    }

    const Reader bodyStart = r;
    SnapshotReport scratch;
    {
        Loader validator{world, impl, schemas, bodyStart, true, scratch};
        if (!validator.run()) {
            FOH_ERROR("Snapshot rejected: truncated or malformed data (the world was not changed)");
            return report;
        }
    }

    // Valid: replace the world's entities.
    std::vector<ecs_entity_t> existing;
    {
        ecs_query_desc_t qd{};
        qd.terms[0].id = impl.spawnedTag;
        qd.cache_kind = EcsQueryCacheNone;
        ecs_query_t* q = ecs_query_init(impl.world, &qd);
        ecs_iter_t it = ecs_query_iter(impl.world, q);
        while (ecs_query_next(&it)) {
            for (i32 i = 0; i < it.count; ++i) existing.push_back(it.entities[i]);
        }
        ecs_query_fini(q);
    }
    for (const ecs_entity_t e : existing) world.despawn(Entity{e});

    Loader loader{world, impl, schemas, bodyStart, false, report};
    report.ok = loader.run();
    return report;
}

} // namespace foh
