#include "foh/asset_db.h"
#include "foh/utf8_path.h"

#include <algorithm>
#include <charconv>
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

AssetDb::AssetDb(AssetDbDesc desc) : m_desc(std::move(desc)) {}

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
    out << "setting maxTextureSize " << r.settings.maxTextureSize << '\n';
    out << "setting textureRole " << r.settings.textureRole << '\n';
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
            else if (name == "maxTextureSize") r.settings.maxTextureSize = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
            else if (name == "textureRole") r.settings.textureRole = v;
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

// ---- import --------------------------------------------------------------------------------

ImportResult AssetDb::importFile(const fs::path& file, bool force) {
    std::error_code ec;
    if (file.is_relative()) {
        ImportResult r = importSource(toUtf8(file), force);
        return r;
    }
    const fs::path root = fs::weakly_canonical(m_desc.sourceRoot, ec);
    const fs::path abs = fs::weakly_canonical(file, ec);
    const fs::path rel = abs.lexically_relative(root);
    if (!rel.empty() && *rel.begin() != "..") return importSource(toUtf8(rel), force);
    return copyIn(abs, force);
}

ImportResult AssetDb::copyIn(const fs::path& file, bool force) {
    ImportResult result;
    const SourceFormat fmt = DetectSourceFormat(file);
    if (fmt == SourceFormat::Unknown) {
        result.error = "unsupported file type: " + toUtf8(file.filename());
        return result;
    }
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        result.error = "file not found: " + toUtf8(file);
        return result;
    }

    // Parse once at the original location to learn which files the source references.
    std::vector<fs::path> files{file};
    if (fmt != SourceFormat::Image) {
        ImportedScene scene;
        std::string error;
        const bool ok = fmt == SourceFormat::Gltf ? ImportGltf(file, scene, error) : ImportFbx(file, scene, error);
        if (!ok) {
            result.error = error;
            return result;
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
        result.error = "the file and the files it references share no common folder";
        return result;
    }

    const fs::path destDir = m_desc.sourceRoot / "imported" / file.stem();
    fs::path destMain;
    for (const fs::path& f : files) {
        const fs::path dest = destDir / f.lexically_relative(base);
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(f, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.error = "cannot copy " + toUtf8(f) + ": " + ec.message();
            return result;
        }
        if (f == files.front()) destMain = dest;
    }
    return importSource(toUtf8(destMain.lexically_relative(m_desc.sourceRoot)), force);
}

ImportResult AssetDb::importSource(const std::string& relPath, bool force) {
    ImportResult result;
    result.sourcePath = relPath;
    const fs::path full = m_desc.sourceRoot / fromUtf8(relPath);
    const SourceFormat fmt = DetectSourceFormat(full);
    if (fmt == SourceFormat::Unknown) {
        result.error = "unsupported file type: " + relPath;
        return result;
    }
    std::error_code ec;
    if (!fs::exists(full, ec)) {
        result.error = "file not found: " + relPath;
        return result;
    }

    SourceRecord record;
    const auto existing = m_sources.find(relPath);
    const bool known = existing != m_sources.end();
    if (known) {
        record = existing->second;
    } else {
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
    auto contentHash = [&](const SourceRecord& r) {
        const fs::path main = m_desc.sourceRoot / fromUtf8(r.path);
        std::vector<u8> bytes;
        u64 h = 0;
        ReadFileBytes(main, bytes);
        h = HashBytes(bytes, h);
        for (const std::string& f : r.files) {
            ReadFileBytes(main.parent_path() / fromUtf8(f), bytes);
            h = HashBytes(bytes, h);
        }
        return h;
    };

    // Up to date? Cheap checks first: stamp, then content hash.
    if (known && !force && record.importerVersion == kImporterVersion) {
        bool cooked = true;
        for (const SubAssetRecord& s : record.subAssets)
            if (!s.tombstone && !fs::exists(cachedFile(s.guid, s.kind), ec)) cooked = false;
        if (cooked) {
            const u64 stamp = computeStamp(record);
            if (stamp == record.stamp || contentHash(record) == record.contentHash) {
                if (stamp != record.stamp) {
                    record.stamp = stamp;
                    saveMeta(record);
                    existing->second.stamp = stamp;
                }
                fillResult(record);
                result.upToDate = true;
                return result;
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
        m_failedStamps[relPath] = computeStamp(record);
        return result;
    }
    m_failedStamps.erase(relPath);

    auto idFor = [&](AssetKind kind, const std::string& key) {
        for (const SubAssetRecord& s : record.subAssets)
            if (s.kind == kind && s.key == key) return s.guid;
        return DeriveAssetId(record.guid, std::string(AssetKindName(kind)) + ":" + key);
    };
    std::vector<CookedAsset> cooked = CookScene(scene, record.settings, idFor, fmt == SourceFormat::Image);

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
            fs::remove(cachedFile(old.guid, old.kind), ec);
            result.changed.push_back(old.guid);
        }
        old.tombstone = true;
        record.subAssets.push_back(std::move(old));
        m_assets.erase(record.subAssets.back().guid);
        m_deps.erase(record.subAssets.back().guid);
    }

    for (const CookedAsset& c : cooked) {
        const fs::path out = cachedFile(c.guid, c.kind);
        if (sameBytes(out, c.bytes)) continue;
        if (!WriteFileBytes(out, c.bytes)) {
            result.error = "cannot write " + toUtf8(out);
            return result;
        }
        result.changed.push_back(c.guid);
    }

    record.files.clear();
    for (const fs::path& f : scene.externalFiles) record.files.push_back(toUtf8(f.lexically_relative(full.parent_path())));
    record.importerVersion = kImporterVersion;
    record.contentHash = contentHash(record);
    record.stamp = computeStamp(record);
    saveMeta(record);
    indexSource(record);
    fillResult(record);
    result.warnings = std::move(scene.warnings);
    m_sources[relPath] = std::move(record);
    return result;
}

std::vector<ImportResult> AssetDb::reimportChanged() {
    std::vector<ImportResult> results;
    std::vector<std::string> paths;
    paths.reserve(m_sources.size());
    for (const auto& [path, record] : m_sources) paths.push_back(path);
    std::error_code ec;
    for (const std::string& path : paths) {
        const SourceRecord& record = m_sources.at(path);
        if (!fs::exists(m_desc.sourceRoot / fromUtf8(path), ec)) continue;
        const u64 stamp = computeStamp(record);
        if (stamp == record.stamp) continue;
        if (const auto failed = m_failedStamps.find(path); failed != m_failedStamps.end() && failed->second == stamp) continue;
        ImportResult r = importSource(path, false);
        if (!r.ok) m_failedStamps[path] = stamp;
        if (!r.upToDate) results.push_back(std::move(r));
    }
    return results;
}

ImportResult AssetDb::setImportSettings(const std::string& sourcePath, const ImportSettings& settings) {
    const auto it = m_sources.find(sourcePath);
    if (it == m_sources.end()) {
        ImportResult r;
        r.error = "unknown source: " + sourcePath;
        return r;
    }
    it->second.settings = settings;
    return importSource(sourcePath, true);
}

ImportResult AssetDb::remapSubAsset(const std::string& sourcePath, const std::string& oldKey, const std::string& newKey) {
    ImportResult r;
    const auto it = m_sources.find(sourcePath);
    if (it == m_sources.end()) {
        r.error = "unknown source: " + sourcePath;
        return r;
    }
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
    return importSource(sourcePath, true);
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

} // namespace foh
