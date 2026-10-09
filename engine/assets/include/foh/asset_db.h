#pragma once

// The asset database (README "Assets" item 3, ADR 0004).
//
//   <sourceRoot>/models/house.gltf          source file (tracked in git)
//   <sourceRoot>/models/house.gltf.meta     source GUID, import settings, sub-asset table (text, mergeable)
//   <cacheRoot>/<guid>.fmesh|fmat|ftex|fprefab    cooked sub-assets (git-ignored, rebuilt on demand)
//   <sourceRoot>/**/*.fvariant              authored prefab variants (container files, tracked)
//
// A source file is an asset with sub-assets; each sub-asset is its own container file with
// its own GUID, and assets reference each other by GUID only. The sub-asset table maps a
// stable source key (mesh/material/image name) to its GUID so identities survive reimports;
// keys that disappear stay in the table as tombstones until remapped or pruned.
//
// Thread-safety: not thread safe. Use from one thread (the editor's main thread); loads
// return plain data that may then be handed to other threads. Imports started with beginImport()
// run on the job system but never touch the database: they work on a snapshot of the source's
// record and the main thread commits the outcome in pollImports().

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "foh/asset_container.h"
#include "foh/asset_import.h"
#include "foh/asset_types.h"
#include "foh/jobs.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

inline constexpr u32 kImporterVersion = 3; // bump when cooking output changes: forces a reimport

struct AssetDbDesc {
    std::filesystem::path sourceRoot;
    std::filesystem::path cacheRoot;
    // Runs imports on worker threads (low priority) and parallelizes texture cooking. Without it every
    // import runs inline on the calling thread. Must outlive the database.
    JobSystem* jobs = nullptr;
};

using ImportTicket = u64; // names one beginImport() request; 0 = none

struct SubAssetRecord {
    std::string key;
    std::string name;
    AssetKind kind = AssetKind::None;
    AssetId guid;
    bool tombstone = false; // present in an earlier import, absent from the latest one
    std::vector<AssetId> dependencies;
};

struct SourceRecord {
    AssetId guid;
    std::string path;   // relative to the source root, '/' separators
    std::string format; // "gltf" | "fbx" | "image"
    u32 importerVersion = 0;
    ImportSettings settings;
    u64 contentHash = 0;               // main file + external files
    u64 stamp = 0;                     // size + mtime of the same files: cheap change detection
    std::vector<std::string> files;    // external files, relative to the main file's directory
    std::vector<SubAssetRecord> subAssets;
};

struct ImportResult {
    ImportTicket ticket = 0;
    std::string displayName; // what the user asked for (file name), for progress UI
    bool ok = false;
    bool upToDate = false; // nothing changed, nothing written
    std::string error;
    std::string sourcePath; // relative to the source root
    AssetId source;
    AssetId prefab;                    // invalid when none was generated
    std::vector<AssetId> assets;       // live sub-assets
    std::vector<AssetId> changed;      // sub-assets whose cooked file was (re)written: hot reload these
    std::vector<std::string> warnings;
};

struct AssetInfo {
    AssetKind kind = AssetKind::None;
    AssetId source;      // owning source asset (invalid for authored assets such as variants)
    std::string key;     // key within the source
    std::string name;
    std::filesystem::path file; // cooked container (or the authored file)
};

class FOH_API AssetDb {
public:
    explicit AssetDb(AssetDbDesc desc);
    ~AssetDb(); // waits for running imports
    struct Outcome; // internal: what an import worker hands back
    AssetDb(const AssetDb&) = delete;
    AssetDb& operator=(const AssetDb&) = delete;

    // Reads every .meta and .fvariant under the source root and builds the index.
    void scan();

    // ---- importing ----
    // Starts importing (or reimporting) a file and returns at once. Files outside the source root are
    // copied in first, with the files they reference, under <sourceRoot>/imported/<name>/ (so the
    // project stays self contained). Re-dropping the same file reimports it and keeps every GUID.
    // Imports run one at a time, in request order, each internally parallel; a source is never
    // imported twice concurrently. Results arrive through pollImports().
    ImportTicket beginImport(const std::filesystem::path& file, bool force = false);
    // Main thread, once per frame: commits finished imports (database, .meta, index) and returns
    // their results. Cheap when nothing finished.
    std::vector<ImportResult> pollImports();
    bool importsPending() const;
    std::vector<std::string> pendingImportNames() const; // running first, then queued

    // Blocking convenience: beginImport + wait for that import. Other finished imports stay queued
    // for the next pollImports().
    ImportResult importFile(const std::filesystem::path& file, bool force = false);
    // Blocks until nothing is pending and returns every result that was not polled yet.
    std::vector<ImportResult> waitForImports();

    // Hot reload tick: starts a reimport for every source whose files changed since the last import
    // (stat calls only, cheap when nothing changed) and returns the tickets.
    std::vector<ImportTicket> beginReimportChanged();
    // Blocking form of the above, for tools and tests.
    std::vector<ImportResult> reimportChanged();

    // Applies new import settings to a source and reimports it.
    ImportResult setImportSettings(const std::string& sourcePath, const ImportSettings& settings);

    // A renamed key in the source normally creates a new sub-asset and tombstones the old one.
    // This makes `newKey` take over `oldKey`'s GUID (so existing references keep working).
    ImportResult remapSubAsset(const std::string& sourcePath, const std::string& oldKey, const std::string& newKey);
    // Removes tombstones of a source.
    void pruneTombstones(const std::string& sourcePath);

    // ---- queries ----
    const AssetInfo* find(AssetId id) const;
    const SourceRecord* findSource(const std::string& sourcePath) const;
    const SourceRecord* findSource(AssetId sourceId) const;
    std::vector<AssetId> assetsOfKind(AssetKind kind) const;
    std::vector<const SourceRecord*> sources() const;
    // Direct references of an asset / assets that reference it. Hot reload uses the latter to
    // tell dependents (a changed texture reloads the materials that use it).
    std::vector<AssetId> dependencies(AssetId id) const;
    std::vector<AssetId> dependents(AssetId id) const;

    // ---- loading (synchronous; the async loader/manager builds on these) ----
    bool load(AssetId id, MeshAsset& out, std::string* error = nullptr) const;
    bool load(AssetId id, MaterialAsset& out, std::string* error = nullptr) const;
    // `firstMip` skips the largest mips (see DeserializeTexture).
    bool load(AssetId id, TextureAsset& out, u32 firstMip = 0, std::string* error = nullptr) const;
    bool load(AssetId id, PrefabAsset& out, std::string* error = nullptr) const;
    bool load(AssetId id, PrefabVariantAsset& out, std::string* error = nullptr) const;
    bool load(AssetId id, SkeletonAsset& out, std::string* error = nullptr) const;
    bool load(AssetId id, ClipAsset& out, std::string* error = nullptr) const;
    // A prefab with a variant's overrides applied. Accepts a prefab or a variant id.
    bool loadResolvedPrefab(AssetId id, VariantApplyResult& out, std::string* error = nullptr) const;

    // Writes an authored variant at <sourceRoot>/<relativePath> (".fvariant" is appended if missing)
    // and indexes it. Returns its id (existing file keeps its id).
    AssetId saveVariant(const std::string& relativePath, const PrefabVariantAsset& variant);
    // Same for an authored material (".fmat"): materials made in the editor, e.g. from a downloaded texture set.
    // Rewriting an existing file keeps its id, so everything that uses the material follows the new content.
    AssetId saveMaterial(const std::string& relativePath, const MaterialAsset& material);
    // The authored asset (variant or material) stored at `relativePath`, or an invalid id.
    AssetId findAuthored(const std::string& relativePath) const;

    const std::filesystem::path& sourceRoot() const { return m_desc.sourceRoot; }
    const std::filesystem::path& cacheRoot() const { return m_desc.cacheRoot; }

private:
    struct Async;
    ImportTicket enqueue(const std::filesystem::path& external, const std::string& relPath, bool force, std::string display);
    ImportResult importSourceBlocking(const std::string& relPath, bool force);
    void dispatch();
    ImportResult waitFor(ImportTicket ticket);
    void commit(Outcome& outcome, ImportResult& result);
    bool openContainer(AssetId id, AssetKind expected, std::optional<ContainerReader>& reader, std::string* error) const;
    void indexSource(const SourceRecord& record);
    void saveMeta(const SourceRecord& record) const;
    bool loadMeta(const std::filesystem::path& metaFile, SourceRecord& out) const;
    u64 computeStamp(const SourceRecord& record) const;
    std::filesystem::path cachedFile(AssetId id, AssetKind kind) const;

    AssetDbDesc m_desc;
    std::unordered_map<std::string, SourceRecord> m_sources; // by relative source path
    std::unordered_map<AssetId, AssetInfo, AssetIdHash> m_assets;
    std::unordered_map<AssetId, std::vector<AssetId>, AssetIdHash> m_deps;
    // Stamp of the last failed import per source: a broken file is reported once, not every poll.
    std::unordered_map<std::string, u64> m_failedStamps;
    std::unique_ptr<Async> m_async;
};

} // namespace foh
