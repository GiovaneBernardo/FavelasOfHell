#pragma once

// The Assets Workshop window: browse free asset sources (see providers.h), preview thumbnails,
// and download + import with one click. Zip packages are extracted safely (zip_extract.h) into
// <assets>/workshop/<source>/<asset>_<variant>/, every package gets an ATTRIBUTION.txt with its
// license and origin, and the importable files are handed to the asset importer.
//
// Downloads run in the background (HttpClient is non-blocking, extraction runs on the job system);
// update() must be called every frame, also while the window is closed, so transfers finish.
//
// Thread-safety: main thread only.

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "editor_prefs.h"
#include "foh/jobs.h"
#include "http_client.h"
#include "material_recipe.h"
#include "providers.h"
#include "thumbnails.h"
#include "zip_extract.h"

namespace foh {

// A material to author once its textures are imported. All paths are relative to the asset root.
struct MaterialRecipe {
    std::string name;
    std::string outPath; // the ".fmat" to write
    std::string colorMap, normalMap, ormMap, emissiveMap;
};

struct WorkshopHost {
    std::filesystem::path assetRoot;
    // Starts importing one file (model or texture) without placing it; returns the import ticket (0 = refused).
    std::function<unsigned long long(const std::filesystem::path& file)> importFile;
    // Creates a material from imported textures: called right after the imports were started, the host
    // finishes the material when their tickets complete.
    std::function<void(const MaterialRecipe& recipe, const std::vector<unsigned long long>& tickets)> createMaterial;
    // Places an imported model (path relative to the asset root) into the active scene.
    std::function<void(const std::string& relPath)> placeModel;
    // Something new appeared on disk: let the content browser re-list.
    std::function<void()> refreshBrowser;
};

class Workshop {
public:
    Workshop();
    ~Workshop();

    void init(JobSystem& jobs, ThumbnailService& thumbs, EditorPrefs& prefs, WorkshopHost host);
    void update();
    // Waits for background extraction and drops pending transfers. Call before the job system goes away.
    void shutdown();
    // QA: once the first search returns, downloads its first result at the smallest variant.
    void enableSelfTest() { m_selfTest = true; }
    void draw(bool* open, float scale);

private:
    struct ZipTask;
    struct PackTask;
    struct Download;

    // search
    void runSearch(bool append);
    void selectProvider(size_t index);
    ThumbnailService::State thumbnailFor(const WorkshopAsset& asset, ThumbRect& rect);

    // ui
    void drawToolbar(float scale);
    void drawResults(float scale);
    void drawTile(const WorkshopAsset& asset, size_t index, ImVec2 pos, float tile, float textH, float scale);
    void drawDetail(float scale);
    void drawDownloads(float scale);

    // downloads
    void startDownload(const WorkshopAsset& asset, const std::string& variant, bool place);
    Download* findDownload(unsigned id);
    void onPlan(unsigned id, DownloadPlan& plan);
    void onFileDone(unsigned id, size_t index, HttpResult& result);
    void afterTransfers(Download& d);
    void beginImport(Download& d);
    void importMaterialSet(Download& d, const PbrFiles& pbr);
    void stepImport(Download& d);
    void finish(Download& d, bool ok, const std::string& message);
    void writeAttribution(const Download& d);

    JobSystem* m_jobs = nullptr;
    ThumbnailService* m_thumbs = nullptr;
    EditorPrefs* m_prefs = nullptr;
    WorkshopHost m_host;

    std::vector<std::unique_ptr<WorkshopProvider>> m_providers; // declared before m_http: callbacks die first
    HttpClient m_http;

    size_t m_provider = 0;
    WorkshopKind m_kind = WorkshopKind::Material;
    char m_query[128] = {};
    int m_licenseFilter = 0; // index into the provider's filters + 1 (0 = any)
    std::vector<WorkshopAsset> m_results;
    unsigned m_page = 0;
    bool m_hasMore = false;
    bool m_searching = false;
    std::string m_searchError;
    unsigned m_serial = 0;
    bool m_firstOpen = true;
    int m_selected = -1;
    int m_variant = 0;
    bool m_placeAfter = true;
    bool m_showSettings = false;
    char m_token[160] = {};

    std::unordered_set<unsigned long long> m_thumbFetching;
    std::unordered_set<unsigned long long> m_thumbFailed;

    std::deque<std::unique_ptr<Download>> m_downloads;
    unsigned m_nextDownload = 1;
    float m_scale = 1.0f;
    bool m_selfTest = false;
};

} // namespace foh
