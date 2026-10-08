#pragma once

// Asset sources of the workshop. Every provider talks to a documented, free web API:
//   ambientCG   https://docs.ambientcg.com/api/  (CC0 PBR materials, zip downloads, no key)
//   Poly Haven  https://github.com/Poly-Haven/Public-API  (CC0 textures and models, plain files, no key)
//   Sketchfab   https://docs.sketchfab.com/data-api/v3/  (search is open; downloads need the user's free API token)
// FreePBR (freepbr.com) has no public API and its terms do not allow scraping, so it is not included;
// adding another source means implementing WorkshopProvider.
//
// Thread-safety: main thread only (callbacks arrive from HttpClient::poll()).

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "http_client.h"

namespace foh {

enum class WorkshopKind { Material, Model };

struct WorkshopAsset {
    std::string provider; // provider name
    std::string id;
    std::string name;
    std::string author;
    std::string license;       // human readable, as the source states it
    bool nonCommercial = false; // license forbids commercial use: shown as a warning
    std::string pageUrl;
    std::string thumbUrl;
    std::string category;
    std::vector<std::string> tags;
    WorkshopKind kind = WorkshopKind::Material;
    std::vector<std::string> variants; // resolution / format choices
    size_t defaultVariant = 0;
    unsigned long long downloads = 0;
    // Provider private: label -> direct download url (ambientCG).
    std::vector<std::pair<std::string, std::string>> variantUrls;
};

struct SearchQuery {
    std::string text;
    WorkshopKind kind = WorkshopKind::Material;
    unsigned page = 0;
    std::string license; // provider specific filter ("" = any)
};

struct SearchResult {
    bool ok = false;
    std::string error;
    std::vector<WorkshopAsset> items;
    bool hasMore = false;
};

// One file of a download. `path` is relative to the package folder (validated by the caller).
struct DownloadFile {
    std::string url;
    std::string path;
    bool isZip = false; // extracted into the package folder, then deleted
    std::vector<std::string> headers;
};

struct DownloadPlan {
    bool ok = false;
    std::string error;
    std::vector<DownloadFile> files;
};

class WorkshopProvider {
public:
    virtual ~WorkshopProvider() = default;
    virtual const char* name() const = 0;
    virtual bool supports(WorkshopKind kind) const = 0;
    virtual bool needsToken() const { return false; }
    virtual const char* tokenHelpUrl() const { return ""; }
    // Provider specific license filter choices for the UI ("" = any is implicit).
    virtual std::vector<std::pair<std::string, std::string>> licenseFilters() const { return {}; }

    void setToken(std::string token) { m_token = std::move(token); }

    using SearchDone = std::function<void(SearchResult&)>;
    using PlanDone = std::function<void(DownloadPlan&)>;
    virtual void search(HttpClient& http, const SearchQuery& query, SearchDone done) = 0;
    virtual void plan(HttpClient& http, const WorkshopAsset& asset, const std::string& variant, PlanDone done) = 0;

protected:
    std::string m_token;
};

std::vector<std::unique_ptr<WorkshopProvider>> CreateWorkshopProviders();

std::string UrlEncode(const std::string& text);

} // namespace foh
