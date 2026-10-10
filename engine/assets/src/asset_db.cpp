#include "foh/asset_db.h"
#include "foh/utf8_path.h"

#include <algorithm>
#include <charconv>
#include <deque>
#include <fstream>
#include <sstream>

#include "foh/log.h"

namespace foh {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMetaHeader = "foh-meta 1";

std::string toUtf8(const fs::path& p) { return PathToUtf8(p); }
fs::path fromUtf8(const std::string& s) { return PathFromUtf8(s); }

std::string hex64(u64 v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}
u64 parseHex64(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = line.find(sep, start);
        if (at == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, at - start));
        start = at + 1;
    }
}

std::string singleLine(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

u64 fileStamp(const fs::path& p) {
    std::error_code ec;
    const u64 size = fs::file_size(p, ec);
    const auto time = fs::last_write_time(p, ec);
    struct {
        u64 size;
        i64 time;
    } stamp{ec ? 0 : size, static_cast<i64>(time.time_since_epoch().count())};
    return HashBytes(&stamp, sizeof(stamp), HashString(toUtf8(p.filename())));
}

bool sameBytes(const fs::path& file, const std::vector<u8>& bytes) {
    std::error_code ec;
    if (fs::file_size(file, ec) != bytes.size() || ec) return false;
    std::vector<u8> existing;
    return ReadFileBytes(file, existing) && existing == bytes;
}

} // namespace


fs::path AssetDb::cachedFile(AssetId id, AssetKind kind) const {
    return m_desc.cacheRoot / (ToString(id) + "." + AssetKindExtension(kind));
}

// ---- .meta files ---------------------------------------------------------------------------

void AssetDb::saveMeta(const SourceRecord& r) const {
    std::ostringstream out;
    out << kMetaHeader << '\n';
    out << "guid " << ToString(r.guid) << '\n';
    out << "format " << r.format << '\n';
    out << "importer " << r.importerVersion << '\n';
    out << "content " << hex64(r.contentHash) << '\n';
    out << "stamp " << hex64(r.stamp) << '\n';
    out << "setting generatePrefab " << (r.settings.generatePrefab ? 1 : 0) << '\n';
    out << "setting generateMips " << (r.settings.generateMips ? 1 : 0) << '\n';
    out << "setting compressTextures " << (r.settings.compressTextures ? 1 : 0) << '\n';
    out << "setting maxTextureSize " << r.settings.maxTextureSize << '\n';
    out << "setting textureRole " << r.settings.textureRole << '\n';
    out << "setting clipLoop " << r.settings.clipLoop << '\n';
    out << "setting clipRootMotion " << r.settings.clipRootMotion << '\n';
    out << "setting clipRootJoint " << r.settings.clipRootJoint << '\n';
    out << "setting clipRotationToleranceDeg " << r.settings.clipRotationToleranceDeg << '\n';
    out << "setting clipTranslationTolerance " << r.settings.clipTranslationTolerance << '\n';
    out << "setting animationOnly " << (r.settings.animationOnly ? 1 : 0) << '\n';
    out << "setting clipSegments " << singleLine(r.settings.clipSegments) << '\n';
    for (const std::string& f : r.files) out << "file " << f << '\n';
    // sub <kind> <guid> <live|dead> <deps comma separated or -> <key> <name>, tab separated
    for (const SubAssetRecord& s : r.subAssets) {
        std::string deps;
        for (const AssetId& d : s.dependencies) deps += (deps.empty() ? "" : ",") + ToString(d);
        out << "sub\t" << AssetKindName(s.kind) << '\t' << ToString(s.guid) << '\t' << (s.tombstone ? "dead" : "live") << '\t'
            << (deps.empty() ? "-" : deps) << '\t' << singleLine(s.key) << '\t' << singleLine(s.name) << '\n';
    }
    const std::string text = out.str();
    const fs::path metaFile = m_desc.sourceRoot / fromUtf8(r.path + ".meta");
    WriteFileBytes(metaFile, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

bool AssetDb::loadMeta(const fs::path& metaFile, SourceRecord& out) const {
    std::vector<u8> bytes;
    if (!ReadFileBytes(metaFile, bytes)) return false;
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    std::string line;
    if (!std::getline(in, line) || line.rfind(kMetaHeader, 0) != 0) return false;

    SourceRecord r;
    r.settings.textureRole = "auto";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("sub\t", 0) == 0) {
            const auto f = split(line, '\t');
            if (f.size() < 7) continue;
            SubAssetRecord s;
            s.kind = AssetKindFromName(f[1]);
            if (s.kind == AssetKind::None || !ParseAssetId(f[2], s.guid)) continue;
            s.tombstone = f[3] == "dead";
            if (f[4] != "-")
                for (const std::string& d : split(f[4], ',')) {
                    AssetId id;
                    if (ParseAssetId(d, id)) s.dependencies.push_back(id);
                }
            s.key = f[5];
            s.name = f[6];
            r.subAssets.push_back(std::move(s));
            continue;
        }
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp), value = line.substr(sp + 1);
        if (key == "guid") ParseAssetId(value, r.guid);
        else if (key == "format") r.format = value;
        else if (key == "importer") r.importerVersion = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "content") r.contentHash = parseHex64(value);
        else if (key == "stamp") r.stamp = parseHex64(value);
        else if (key == "file") r.files.push_back(value);
        else if (key == "setting") {
            const size_t sp2 = value.find(' ');
            if (sp2 == std::string::npos) continue;
            const std::string name = value.substr(0, sp2), v = value.substr(sp2 + 1);
            if (name == "generatePrefab") r.settings.generatePrefab = v != "0";
            else if (name == "generateMips") r.settings.generateMips = v != "0";
            else if (name == "compressTextures") r.settings.compressTextures = v != "0";
            else if (name == "maxTextureSize") r.settings.maxTextureSize = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
            else if (name == "textureRole") r.settings.textureRole = v;
            else if (name == "clipLoop") r.settings.clipLoop = v;
            else if (name == "clipRootMotion") r.settings.clipRootMotion = v;
            else if (name == "clipRootJoint") r.settings.clipRootJoint = v;
            else if (name == "clipRotationToleranceDeg") r.settings.clipRotationToleranceDeg = std::strtof(v.c_str(), nullptr);
            else if (name == "clipTranslationTolerance") r.settings.clipTranslationTolerance = std::strtof(v.c_str(), nullptr);
            else if (name == "animationOnly") r.settings.animationOnly = v != "0";
            else if (name == "clipSegments") r.settings.clipSegments = v;
        }
    }
    if (!r.guid.isValid()) return false;
    out = std::move(r);
    return true;
}

// ---- index ---------------------------------------------------------------------------------

void AssetDb::indexSource(const SourceRecord& record) {
    for (const SubAssetRecord& s : record.subAssets) {
        if (s.tombstone) {
            m_assets.erase(s.guid);
            m_deps.erase(s.guid);
            continue;
        }
        AssetInfo info;
        info.kind = s.kind;
        info.source = record.guid;
        info.key = s.key;
        info.name = s.name;
        info.file = cachedFile(s.guid, s.kind);
        m_assets[s.guid] = std::move(info);
        m_deps[s.guid] = s.dependencies;
    }
}

void AssetDb::scan() {
    m_sources.clear();
    m_assets.clear();
    m_deps.clear();
    std::error_code ec;
    if (!fs::exists(m_desc.sourceRoot, ec)) return;
    for (fs::recursive_directory_iterator it(m_desc.sourceRoot, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        const std::string ext = p.extension().string();
        if (ext == ".meta") {
            SourceRecord record;
            if (!loadMeta(p, record)) {
                FOH_WARN("asset db: unreadable meta file {}", toUtf8(p));
                continue;
            }
            fs::path sourceFile = p;
            sourceFile.replace_extension(); // drops ".meta"
            record.path = toUtf8(sourceFile.lexically_relative(m_desc.sourceRoot));
            indexSource(record);
            m_sources[record.path] = std::move(record);
        } else if (ext == ".fmat") {
            std::vector<u8> bytes;
            std::string error;
            if (!ReadFileBytes(p, bytes)) continue;
            auto reader = ContainerReader::Open(std::move(bytes), &error);
            if (!reader || reader->typeId() != static_cast<u32>(AssetKind::Material)) {
                FOH_WARN("asset db: bad material file {}: {}", toUtf8(p), error);
                continue;
            }
            AssetInfo info;
            info.kind = AssetKind::Material;
            info.name = p.stem().string();
            info.key = toUtf8(p.lexically_relative(m_desc.sourceRoot));
            info.file = p;
            m_assets[reader->id()] = std::move(info);
            m_deps[reader->id()] = ReadDependencies(*reader);
        } else if (ext == ".fvariant") {
            std::vector<u8> bytes;
            std::string error;
            if (!ReadFileBytes(p, bytes)) continue;
            auto reader = ContainerReader::Open(std::move(bytes), &error);
            if (!reader || reader->typeId() != static_cast<u32>(AssetKind::PrefabVariant)) {
                FOH_WARN("asset db: bad variant file {}: {}", toUtf8(p), error);
                continue;
            }
            AssetInfo info;
            info.kind = AssetKind::PrefabVariant;
            info.name = p.stem().string();
            info.key = toUtf8(p.lexically_relative(m_desc.sourceRoot));
            info.file = p;
            m_assets[reader->id()] = std::move(info);
            m_deps[reader->id()] = ReadDependencies(*reader);
        }
    }
}

u64 AssetDb::computeStamp(const SourceRecord& r) const {
    const fs::path main = m_desc.sourceRoot / fromUtf8(r.path);
    u64 stamp = fileStamp(main);
    for (const std::string& f : r.files) stamp = HashBytes(&stamp, sizeof(stamp), fileStamp(main.parent_path() / fromUtf8(f)));
    return stamp;
}

// ---- import ----------------------------------------------------------------------------------
//
// An import has two halves. The worker half (runImport/runCopy) is a pure function of its inputs: it gets a
// snapshot of the source's record and writes only cooked files that belong to that source. The main thread
// half (commit) is the only place the database changes. That is what lets imports run on worker threads
// without locking anything.

namespace {

struct ImportContext {
    fs::path sourceRoot;
    fs::path cacheRoot;
    JobSystem* jobs = nullptr;
};

fs::path cacheFileOf(const fs::path& cacheRoot, AssetId id, AssetKind kind) {
    return cacheRoot / (ToString(id) + "." + AssetKindExtension(kind));
}

u64 stampOf(const fs::path& sourceRoot, const SourceRecord& r) {
    const fs::path main = sourceRoot / PathFromUtf8(r.path);
    u64 stamp = fileStamp(main);
    for (const std::string& f : r.files) stamp = HashBytes(&stamp, sizeof(stamp), fileStamp(main.parent_path() / PathFromUtf8(f)));
    return stamp;
}

u64 contentHashOf(const fs::path& sourceRoot, const SourceRecord& r) {
    const fs::path main = sourceRoot / PathFromUtf8(r.path);
    std::vector<u8> bytes;
    ReadFileBytes(main, bytes);
    u64 h = HashBytes(bytes, 0);
    for (const std::string& f : r.files) {
        ReadFileBytes(main.parent_path() / PathFromUtf8(f), bytes);
        h = HashBytes(bytes, h);
    }
    return h;
}

} // namespace

// What a finished worker hands back to the main thread.
struct AssetDb::Outcome {
    ImportResult result;
    SourceRecord record; // the new record when `store`
    bool store = false;  // commit: replace the stored record, write the .meta, refresh the index
    bool failed = false;
    u64 failedStamp = 0;
};

namespace {

struct CopyOutcome {
    bool ok = false;
    std::string error;
    std::string destRel;
};

// Copies a dropped file and the files it references into <sourceRoot>/imported/<name>/.
CopyOutcome runCopy(const ImportContext& ctx, const fs::path& file) {
    CopyOutcome out;
    const SourceFormat fmt = DetectSourceFormat(file);
    if (fmt == SourceFormat::Unknown) {
        out.error = "unsupported file type: " + PathToUtf8(file.filename());
        return out;
    }
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        out.error = "file not found: " + PathToUtf8(file);
        return out;
    }

    // Parse once at the original location to learn which files the source references. (Images stay
    // encoded in the parse, so this is cheap: no decoding, no cooking.)
    std::vector<fs::path> files{file};
    if (fmt != SourceFormat::Image) {
        ImportedScene scene;
        std::string error;
        const bool ok = fmt == SourceFormat::Gltf ? ImportGltf(file, scene, error) : ImportFbx(file, scene, error);
        if (!ok) {
            out.error = error;
            return out;
        }
        for (const fs::path& f : scene.externalFiles) files.push_back(fs::weakly_canonical(f, ec));
    }

    // Common ancestor directory, so the relative layout the source expects is preserved.
    fs::path base = file.parent_path();
    for (const fs::path& f : files) {
        while (!base.empty()) {
            const fs::path rel = f.lexically_relative(base);
            if (!rel.empty() && *rel.begin() != "..") break;
            const fs::path parent = base.parent_path();
            if (parent == base) {
                base.clear();
                break;
            }
            base = parent;
        }
    }
    if (base.empty()) {
        out.error = "the file and the files it references share no common folder";
        return out;
    }

    const fs::path destDir = ctx.sourceRoot / "imported" / file.stem();
    fs::path destMain;
    for (const fs::path& f : files) {
        const fs::path dest = destDir / f.lexically_relative(base);
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(f, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            out.error = "cannot copy " + PathToUtf8(f) + ": " + ec.message();
            return out;
        }
        if (f == files.front()) destMain = dest;
    }
    out.destRel = PathToUtf8(destMain.lexically_relative(ctx.sourceRoot));
    out.ok = true;
    return out;
}

AssetDb::Outcome runImport(const ImportContext& ctx, const std::string& relPath, SourceRecord record, bool known, bool force) {
    AssetDb::Outcome outcome;
    ImportResult& result = outcome.result;
    result.sourcePath = relPath;
    const fs::path full = ctx.sourceRoot / PathFromUtf8(relPath);
    const SourceFormat fmt = DetectSourceFormat(full);
    if (fmt == SourceFormat::Unknown) {
        result.error = "unsupported file type: " + relPath;
        return outcome;
    }
    std::error_code ec;
    if (!fs::exists(full, ec)) {
        result.error = "file not found: " + relPath;
        return outcome;
    }

    if (!known) {
        record.guid = GenerateAssetId();
        record.path = relPath;
        record.settings.textureRole = "auto";
    }
    record.format = fmt == SourceFormat::Gltf ? "gltf" : fmt == SourceFormat::Fbx ? "fbx" : "image";

    auto fillResult = [&](const SourceRecord& r) {
        result.ok = true;
        result.source = r.guid;
        for (const SubAssetRecord& s : r.subAssets) {
            if (s.tombstone) continue;
            result.assets.push_back(s.guid);
            if (s.kind == AssetKind::Prefab) result.prefab = s.guid;
        }
    };

    // Up to date? Cheap checks first: stamp, then content hash.
    if (known && !force && record.importerVersion == kImporterVersion) {
        bool cooked = true;
        for (const SubAssetRecord& s : record.subAssets)
            if (!s.tombstone && !fs::exists(cacheFileOf(ctx.cacheRoot, s.guid, s.kind), ec)) cooked = false;
        if (cooked) {
            const u64 stamp = stampOf(ctx.sourceRoot, record);
            if (stamp == record.stamp || contentHashOf(ctx.sourceRoot, record) == record.contentHash) {
                if (stamp != record.stamp) {
                    record.stamp = stamp;
                    outcome.record = record;
                    outcome.store = true;
                }
                fillResult(record);
                result.upToDate = true;
                return outcome;
            }
        }
    }

    ImportedScene scene;
    std::string error;
    const bool parsed = fmt == SourceFormat::Gltf   ? ImportGltf(full, scene, error)
                        : fmt == SourceFormat::Fbx ? ImportFbx(full, scene, error)
                                                   : ImportImageFile(full, scene, error);
    if (!parsed) {
        result.error = error;
        outcome.failed = true;
        outcome.failedStamp = stampOf(ctx.sourceRoot, record);
        return outcome;
    }

    auto idFor = [&](AssetKind kind, const std::string& key) {
        for (const SubAssetRecord& s : record.subAssets)
            if (s.kind == kind && s.key == key) return s.guid;
        return DeriveAssetId(record.guid, std::string(AssetKindName(kind)) + ":" + key);
    };
    std::vector<std::string> cookWarnings;
    std::vector<CookedAsset> cooked = CookScene(scene, record.settings, idFor, fmt == SourceFormat::Image, ctx.jobs, &cookWarnings);

    // Update the sub-asset table: reuse records by (kind, key); anything not produced this time becomes a tombstone.
    std::vector<SubAssetRecord> previous = std::move(record.subAssets);
    record.subAssets.clear();
    for (const CookedAsset& c : cooked) {
        SubAssetRecord s;
        s.key = c.key;
        s.name = c.name;
        s.kind = c.kind;
        s.guid = c.guid;
        s.dependencies = c.dependencies;
        std::sort(s.dependencies.begin(), s.dependencies.end());
        s.dependencies.erase(std::unique(s.dependencies.begin(), s.dependencies.end()), s.dependencies.end());
        record.subAssets.push_back(std::move(s));
    }
    for (SubAssetRecord& old : previous) {
        const bool stillThere = std::any_of(record.subAssets.begin(), record.subAssets.end(),
                                            [&](const SubAssetRecord& s) { return s.kind == old.kind && s.key == old.key; });
        if (stillThere) continue;
        if (!old.tombstone) {
            fs::remove(cacheFileOf(ctx.cacheRoot, old.guid, old.kind), ec);
            result.changed.push_back(old.guid);
        }
        old.tombstone = true;
        record.subAssets.push_back(std::move(old));
    }

    for (const CookedAsset& c : cooked) {
        const fs::path out = cacheFileOf(ctx.cacheRoot, c.guid, c.kind);
        if (sameBytes(out, c.bytes)) continue;
        if (!WriteFileBytes(out, c.bytes)) {
            result.error = "cannot write " + PathToUtf8(out);
            outcome.failed = true;
            return outcome;
        }
        result.changed.push_back(c.guid);
    }

    record.files.clear();
    for (const fs::path& f : scene.externalFiles) record.files.push_back(PathToUtf8(f.lexically_relative(full.parent_path())));
    record.importerVersion = kImporterVersion;
    record.contentHash = contentHashOf(ctx.sourceRoot, record);
    record.stamp = stampOf(ctx.sourceRoot, record);
    fillResult(record);
    result.warnings = std::move(scene.warnings);
    result.warnings.insert(result.warnings.end(), cookWarnings.begin(), cookWarnings.end());
    outcome.record = std::move(record);
    outcome.store = true;
    return outcome;
}

} // namespace

// ---- the queue ------------------------------------------------------------------------------------

struct AssetDb::Async {
    struct Task {
        ImportTicket ticket = 0;
        enum class Stage { Copy, Import } stage = Stage::Import;
        std::optional<ImportSettings> initial; // settings of a source that is new
        fs::path external;   // Copy: the dropped file
        std::string relPath; // Import: source-root relative
        bool force = false;
        std::string display;
    };
    struct Running {
        Task task;
        ImportContext ctx;
        SourceRecord snapshot; // Import
        bool known = false;
        CopyOutcome copy;
        Outcome outcome;
        JobCounter counter;
    };

    std::deque<Task> queue;
    std::unique_ptr<Running> running; // one import at a time: each one is already parallel inside
    std::vector<ImportResult> finished;
    ImportTicket nextTicket = 1;
};

AssetDb::AssetDb(AssetDbDesc desc) : m_desc(std::move(desc)), m_async(std::make_unique<Async>()) {}

AssetDb::~AssetDb() {
    if (m_async && m_async->running && m_desc.jobs) m_desc.jobs->wait(m_async->running->counter);
}

ImportTicket AssetDb::enqueue(const fs::path& external, const std::string& relPath, bool force, std::string display,
                              const ImportSettings* initial) {
    Async::Task t;
    if (initial) t.initial = *initial;
    t.ticket = m_async->nextTicket++;
    t.stage = external.empty() ? Async::Task::Stage::Import : Async::Task::Stage::Copy;
    t.external = external;
    t.relPath = relPath;
    t.force = force;
    t.display = std::move(display);
    const ImportTicket ticket = t.ticket;
    m_async->queue.push_back(std::move(t));
    dispatch();
    return ticket;
}

// Starts the next queued task if nothing is running.
void AssetDb::dispatch() {
    Async& a = *m_async;
    if (a.running || a.queue.empty()) return;
    a.running = std::make_unique<Async::Running>();
    Async::Running& r = *a.running;
    r.task = std::move(a.queue.front());
    a.queue.pop_front();
    r.ctx = {m_desc.sourceRoot, m_desc.cacheRoot, m_desc.jobs};
    if (r.task.stage == Async::Task::Stage::Import) {
        const auto it = m_sources.find(r.task.relPath);
        r.known = it != m_sources.end();
        if (r.known) r.snapshot = it->second;
        else if (r.task.initial) r.snapshot.settings = *r.task.initial;
    }
    auto body = [](void* p) {
        auto* run = static_cast<Async::Running*>(p);
        if (run->task.stage == Async::Task::Stage::Copy) run->copy = runCopy(run->ctx, run->task.external);
        else run->outcome = runImport(run->ctx, run->task.relPath, run->snapshot, run->known, run->task.force);
    };
    if (m_desc.jobs && m_desc.jobs->workerCount() > 0) {
        m_desc.jobs->submit(body, &r, r.counter, JobPriority::Low);
    } else {
        body(&r); // no workers: inline, finished by the next poll
    }
}

void AssetDb::commit(Outcome& o, ImportResult& result) {
    result = std::move(o.result);
    const std::string& rel = result.sourcePath;
    if (o.failed) {
        m_failedStamps[rel] = o.failedStamp;
        return;
    }
    if (!result.ok) return;
    m_failedStamps.erase(rel);
    if (!o.store) return;
    // Entries of the previous record leave the index first: sub-assets that became tombstones must disappear.
    if (const auto old = m_sources.find(rel); old != m_sources.end())
        for (const SubAssetRecord& s : old->second.subAssets) {
            m_assets.erase(s.guid);
            m_deps.erase(s.guid);
        }
    saveMeta(o.record);
    indexSource(o.record);
    m_sources[rel] = std::move(o.record);
}

std::vector<ImportResult> AssetDb::pollImports() {
    Async& a = *m_async;
    std::vector<ImportResult> done = std::move(a.finished);
    a.finished.clear();
    // Several tasks can finish within one poll (Copy -> Import chain, inline mode), so loop.
    for (;;) {
        if (!a.running) {
            if (a.queue.empty()) break;
            dispatch();
            if (!a.running) break;
        }
        if (a.running->counter.pending.load(std::memory_order_acquire) != 0) break;
        std::unique_ptr<Async::Running> r = std::move(a.running);
        if (r->task.stage == Async::Task::Stage::Copy) {
            if (r->copy.ok) {
                // Second half: import the copy, same ticket, ahead of anything queued.
                Async::Task next;
                next.ticket = r->task.ticket;
                next.stage = Async::Task::Stage::Import;
                next.relPath = r->copy.destRel;
                next.force = r->task.force;
                next.display = r->task.display;
                next.initial = r->task.initial;
                a.queue.push_front(std::move(next));
            } else {
                ImportResult res;
                res.ticket = r->task.ticket;
                res.displayName = r->task.display;
                res.error = r->copy.error;
                done.push_back(std::move(res));
            }
        } else {
            ImportResult res;
            commit(r->outcome, res);
            res.ticket = r->task.ticket;
            res.displayName = r->task.display;
            done.push_back(std::move(res));
        }
    }
    return done;
}

bool AssetDb::importsPending() const { return m_async->running || !m_async->queue.empty(); }

std::vector<std::string> AssetDb::pendingImportNames() const {
    std::vector<std::string> names;
    if (m_async->running) names.push_back(m_async->running->task.display);
    for (const Async::Task& t : m_async->queue) names.push_back(t.display);
    return names;
}

ImportResult AssetDb::waitFor(ImportTicket ticket) {
    Async& a = *m_async;
    for (;;) {
        std::vector<ImportResult> done = pollImports();
        ImportResult mine;
        bool found = false;
        for (ImportResult& r : done) {
            if (!found && r.ticket == ticket) {
                mine = std::move(r);
                found = true;
            } else {
                a.finished.push_back(std::move(r));
            }
        }
        if (found) return mine;
        if (!a.running && a.queue.empty()) {
            mine.ticket = ticket;
            mine.error = "import was lost"; // cannot happen; keeps the loop finite
            return mine;
        }
        if (a.running && m_desc.jobs) m_desc.jobs->wait(a.running->counter);
    }
}

std::vector<ImportResult> AssetDb::waitForImports() {
    Async& a = *m_async;
    std::vector<ImportResult> all;
    for (;;) {
        for (ImportResult& r : pollImports()) all.push_back(std::move(r));
        if (!a.running && a.queue.empty()) break;
        if (a.running && m_desc.jobs) m_desc.jobs->wait(a.running->counter);
    }
    return all;
}

ImportTicket AssetDb::beginImport(const fs::path& file, bool force, const ImportSettings* initial) {
    const std::string display = PathToUtf8(file.filename());
    if (file.is_relative()) return enqueue({}, PathToUtf8(file), force, display, initial);
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(m_desc.sourceRoot, ec);
    const fs::path abs = fs::weakly_canonical(file, ec);
    const fs::path rel = abs.lexically_relative(root);
    if (!rel.empty() && *rel.begin() != "..") return enqueue({}, PathToUtf8(rel), force, display, initial);
    return enqueue(abs, {}, force, display, initial);
}

ImportResult AssetDb::importFile(const fs::path& file, bool force, const ImportSettings* initial) {
    return waitFor(beginImport(file, force, initial));
}

ImportResult AssetDb::importSourceBlocking(const std::string& relPath, bool force) {
    return waitFor(enqueue({}, relPath, force, relPath));
}

std::vector<ImportTicket> AssetDb::beginReimportChanged() {
    std::vector<ImportTicket> tickets;
    std::error_code ec;
    for (const auto& [path, record] : m_sources) {
        if (!fs::exists(m_desc.sourceRoot / PathFromUtf8(path), ec)) continue;
        const u64 stamp = stampOf(m_desc.sourceRoot, record);
        if (stamp == record.stamp) continue;
        if (const auto failed = m_failedStamps.find(path); failed != m_failedStamps.end() && failed->second == stamp) continue;
        // Already queued or running for this source: that import reads the newest files when it starts,
        // so do not pile up duplicates.
        bool busy = m_async->running && m_async->running->task.relPath == path;
        for (const Async::Task& t : m_async->queue) busy = busy || t.relPath == path;
        if (!busy) tickets.push_back(enqueue({}, path, false, path));
    }
    return tickets;
}

std::vector<ImportResult> AssetDb::reimportChanged() {
    beginReimportChanged();
    std::vector<ImportResult> results;
    for (ImportResult& r : waitForImports())
        if (!r.upToDate) results.push_back(std::move(r));
    return results;
}

ImportResult AssetDb::setImportSettings(const std::string& sourcePath, const ImportSettings& settings) {
    const auto it = m_sources.find(sourcePath);
    if (it == m_sources.end()) {
        ImportResult r;
        r.error = "unknown source: " + sourcePath;
        return r;
    }
    waitForImports(); // nothing may be in flight for the record we are about to edit
    it->second.settings = settings;
    return importSourceBlocking(sourcePath, true);
}

ImportResult AssetDb::remapSubAsset(const std::string& sourcePath, const std::string& oldKey, const std::string& newKey) {
    ImportResult r;
    const auto it = m_sources.find(sourcePath);
    if (it == m_sources.end()) {
        r.error = "unknown source: " + sourcePath;
        return r;
    }
    waitForImports();
    SourceRecord& record = it->second;
    auto find = [&](const std::string& key, bool tombstone) {
        return std::find_if(record.subAssets.begin(), record.subAssets.end(),
                            [&](const SubAssetRecord& s) { return s.key == key && s.tombstone == tombstone; });
    };
    auto oldIt = find(oldKey, true);
    auto newIt = find(newKey, false);
    if (oldIt == record.subAssets.end() || newIt == record.subAssets.end() || oldIt->kind != newIt->kind) {
        r.error = "remap needs a tombstoned old key and a live new key of the same kind";
        return r;
    }
    std::error_code ec;
    fs::remove(cachedFile(newIt->guid, newIt->kind), ec);
    m_assets.erase(newIt->guid);
    m_deps.erase(newIt->guid);
    newIt->guid = oldIt->guid; // the reimport below cooks everything again against this id
    record.subAssets.erase(oldIt);
    saveMeta(record);
    return importSourceBlocking(sourcePath, true);
}

void AssetDb::pruneTombstones(const std::string& sourcePath) {
    const auto it = m_sources.find(sourcePath);
    if (it == m_sources.end()) return;
    auto& subs = it->second.subAssets;
    subs.erase(std::remove_if(subs.begin(), subs.end(), [](const SubAssetRecord& s) { return s.tombstone; }), subs.end());
    saveMeta(it->second);
}

// ---- queries -------------------------------------------------------------------------------

const AssetInfo* AssetDb::find(AssetId id) const {
    const auto it = m_assets.find(id);
    return it == m_assets.end() ? nullptr : &it->second;
}

const SourceRecord* AssetDb::findSource(const std::string& sourcePath) const {
    const auto it = m_sources.find(sourcePath);
    return it == m_sources.end() ? nullptr : &it->second;
}

const SourceRecord* AssetDb::findSource(AssetId sourceId) const {
    for (const auto& [path, record] : m_sources)
        if (record.guid == sourceId) return &record;
    return nullptr;
}

std::vector<AssetId> AssetDb::assetsOfKind(AssetKind kind) const {
    std::vector<AssetId> out;
    for (const auto& [id, info] : m_assets)
        if (info.kind == kind) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const SourceRecord*> AssetDb::sources() const {
    std::vector<const SourceRecord*> out;
    for (const auto& [path, record] : m_sources) out.push_back(&record);
    std::sort(out.begin(), out.end(), [](const SourceRecord* a, const SourceRecord* b) { return a->path < b->path; });
    return out;
}

std::vector<AssetId> AssetDb::dependencies(AssetId id) const {
    const auto it = m_deps.find(id);
    return it == m_deps.end() ? std::vector<AssetId>{} : it->second;
}

std::vector<AssetId> AssetDb::dependents(AssetId id) const {
    std::vector<AssetId> out;
    for (const auto& [asset, deps] : m_deps)
        if (std::find(deps.begin(), deps.end(), id) != deps.end()) out.push_back(asset);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- loading -------------------------------------------------------------------------------

bool AssetDb::openContainer(AssetId id, AssetKind expected, std::optional<ContainerReader>& reader, std::string* error) const {
    const AssetInfo* info = find(id);
    if (!info || info->kind != expected) {
        if (error) *error = "asset not found or of another type";
        return false;
    }
    std::vector<u8> bytes;
    if (!ReadFileBytes(info->file, bytes)) {
        if (error) *error = "cannot read " + toUtf8(info->file);
        return false;
    }
    reader = ContainerReader::Open(std::move(bytes), error);
    if (!reader) return false;
    if (reader->id() != id) {
        if (error) *error = "container id does not match the database (stale cache?)";
        return false;
    }
    return true;
}

bool AssetDb::load(AssetId id, MeshAsset& out, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::Mesh, reader, error) && DeserializeMesh(*reader, out, error);
}
bool AssetDb::load(AssetId id, MaterialAsset& out, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::Material, reader, error) && DeserializeMaterial(*reader, out, error);
}
bool AssetDb::load(AssetId id, TextureAsset& out, u32 firstMip, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::Texture, reader, error) && DeserializeTexture(*reader, out, firstMip, error);
}
bool AssetDb::load(AssetId id, SkeletonAsset& out, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::Skeleton, reader, error) && DeserializeSkeleton(*reader, out, error);
}
bool AssetDb::load(AssetId id, ClipAsset& out, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::AnimationClip, reader, error) && DeserializeClip(*reader, out, error);
}
bool AssetDb::load(AssetId id, PrefabAsset& out, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::Prefab, reader, error) && DeserializePrefab(*reader, out, error);
}
bool AssetDb::load(AssetId id, PrefabVariantAsset& out, std::string* error) const {
    std::optional<ContainerReader> reader;
    return openContainer(id, AssetKind::PrefabVariant, reader, error) && DeserializePrefabVariant(*reader, out, error);
}

bool AssetDb::loadResolvedPrefab(AssetId id, VariantApplyResult& out, std::string* error) const {
    const AssetInfo* info = find(id);
    if (!info) {
        if (error) *error = "asset not found";
        return false;
    }
    if (info->kind == AssetKind::Prefab) {
        out = {};
        return load(id, out.prefab, error);
    }
    PrefabVariantAsset variant;
    PrefabAsset base;
    if (!load(id, variant, error) || !load(variant.base, base, error)) return false;
    out = ApplyVariant(base, variant);
    return true;
}

AssetId AssetDb::saveVariant(const std::string& relativePath, const PrefabVariantAsset& variant) {
    std::string rel = relativePath;
    if (fs::path(rel).extension() != ".fvariant") rel += ".fvariant";
    const fs::path file = m_desc.sourceRoot / fromUtf8(rel);

    AssetId id;
    std::vector<u8> existing;
    if (ReadFileBytes(file, existing))
        if (auto reader = ContainerReader::Open(std::move(existing))) id = reader->id();
    if (!id.isValid()) id = GenerateAssetId();

    if (!WriteFileBytes(file, SerializePrefabVariant(variant, id))) return {};
    AssetInfo info;
    info.kind = AssetKind::PrefabVariant;
    info.name = variant.name;
    info.key = rel;
    info.file = file;
    m_assets[id] = std::move(info);
    std::vector<AssetId> deps{variant.base};
    for (const PrefabOverride& o : variant.overrides)
        if (o.material.isValid()) deps.push_back(o.material);
    m_deps[id] = std::move(deps);
    return id;
}

AssetId AssetDb::saveMaterial(const std::string& relativePath, const MaterialAsset& material) {
    std::string rel = relativePath;
    if (fs::path(rel).extension() != ".fmat") rel += ".fmat";
    const fs::path file = m_desc.sourceRoot / fromUtf8(rel);

    AssetId id = findAuthored(rel);
    if (!id.isValid()) {
        std::vector<u8> existing;
        if (ReadFileBytes(file, existing))
            if (auto reader = ContainerReader::Open(std::move(existing))) id = reader->id();
    }
    if (!id.isValid()) id = GenerateAssetId();

    if (!WriteFileBytes(file, SerializeMaterial(material, id))) return {};
    AssetInfo info;
    info.kind = AssetKind::Material;
    info.name = material.name;
    info.key = rel;
    info.file = file;
    m_assets[id] = std::move(info);
    std::vector<AssetId> deps;
    for (const TextureRef* t : {&material.baseColorTexture, &material.normalTexture, &material.metallicRoughnessTexture,
                                &material.occlusionTexture, &material.emissiveTexture})
        if (t->isValid() && std::find(deps.begin(), deps.end(), t->texture) == deps.end()) deps.push_back(t->texture);
    m_deps[id] = std::move(deps);
    return id;
}

AssetId AssetDb::findAuthored(const std::string& relativePath) const {
    for (const auto& [id, info] : m_assets)
        if (!info.source.isValid() && info.key == relativePath) return id;
    return {};
}

} // namespace foh
