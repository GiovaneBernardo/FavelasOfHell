#include "providers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "json.h"

namespace foh {

namespace {

constexpr unsigned kPageSize = 48;
constexpr std::uint64_t kListingLimit = 32ull << 20;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string baseName(const std::string& url) {
    std::string s = url.substr(0, url.find_first_of("?#"));
    const size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

bool parseBody(HttpResult& r, Json& out, std::string& error) {
    if (!r.ok) {
        error = r.error;
        return false;
    }
    std::string perr;
    if (!Json::Parse(r.body, out, &perr)) {
        error = "unexpected response (" + perr + ")";
        return false;
    }
    return true;
}

// ---- ambientCG -------------------------------------------------------------------------------------

class AmbientCg final : public WorkshopProvider {
public:
    const char* name() const override { return "ambientCG"; }
    bool supports(WorkshopKind kind) const override { return kind == WorkshopKind::Material; }

    void search(HttpClient& http, const SearchQuery& q, SearchDone done) override {
        std::string url = "https://ambientcg.com/api/v2/full_json?type=Material&sort=Popular&limit=" + std::to_string(kPageSize) +
                          "&offset=" + std::to_string(q.page * kPageSize) + "&include=downloadData,imageData";
        if (!q.text.empty()) url += "&q=" + UrlEncode(q.text);
        HttpRequest req;
        req.url = url;
        req.maxBytes = kListingLimit;
        http.submit(std::move(req), [done = std::move(done), page = q.page](HttpResult& r) {
            SearchResult out;
            Json j;
            if (!parseBody(r, j, out.error)) return done(out);
            const Json& found = j["foundAssets"];
            for (const Json& a : found.items()) {
                WorkshopAsset w;
                w.provider = "ambientCG";
                w.id = a["assetId"].str();
                if (w.id.empty()) continue;
                w.name = a["displayName"].str(w.id);
                w.author = "ambientCG (Lennart Demes)";
                w.license = "CC0 1.0";
                w.pageUrl = "https://ambientcg.com/view?id=" + w.id;
                w.category = a["displayCategory"].str();
                w.downloads = static_cast<unsigned long long>(a["downloadCount"].num());
                for (const Json& t : a["tags"].items()) w.tags.push_back(t.str());
                w.thumbUrl = a["previewImage"]["256-PNG"].str();
                if (w.thumbUrl.empty()) w.thumbUrl = a["previewImage"]["128-PNG"].str();
                for (const Json& d : a["downloadFolders"]["default"]["downloadFiletypeCategories"]["zip"]["downloads"].items()) {
                    const std::string label = d["attribute"].str();
                    const std::string dl = d["fullDownloadPath"].str();
                    if (label.empty() || dl.empty()) continue;
                    w.variants.push_back(label);
                    w.variantUrls.emplace_back(label, dl);
                }
                if (w.variants.empty()) continue;
                // 2K JPG is the sweet spot for a first look: small, sharp enough for a 1080p scene.
                for (size_t i = 0; i < w.variants.size(); ++i)
                    if (w.variants[i] == "2K-JPG") w.defaultVariant = i;
                out.items.push_back(std::move(w));
            }
            out.ok = true;
            const double total = j["numberOfResults"].num();
            out.hasMore = static_cast<double>((page + 1) * kPageSize) < total;
            done(out);
        });
    }

    void plan(HttpClient&, const WorkshopAsset& a, const std::string& variant, PlanDone done) override {
        DownloadPlan p;
        for (const auto& [label, url] : a.variantUrls) {
            if (label != variant) continue;
            DownloadFile f;
            f.url = url;
            f.path = a.id + "_" + variant + ".zip";
            f.isZip = true;
            p.files.push_back(std::move(f));
            p.ok = true;
        }
        if (!p.ok) p.error = "variant '" + variant + "' is not available";
        done(p);
    }
};

// ---- Poly Haven ------------------------------------------------------------------------------------

class PolyHaven final : public WorkshopProvider {
public:
    const char* name() const override { return "Poly Haven"; }
    bool supports(WorkshopKind) const override { return true; }

    void search(HttpClient& http, const SearchQuery& q, SearchDone done) override {
        const int slot = q.kind == WorkshopKind::Material ? 0 : 1;
        if (m_loaded[slot]) return finishSearch(slot, q, done);
        m_waiting[slot].emplace_back(q, std::move(done));
        if (m_waiting[slot].size() > 1) return; // a listing request is already in flight
        HttpRequest req;
        req.url = std::string("https://api.polyhaven.com/assets?t=") + (slot == 0 ? "textures" : "models");
        req.maxBytes = kListingLimit;
        http.submit(std::move(req), [this, slot](HttpResult& r) {
            std::vector<std::pair<SearchQuery, SearchDone>> waiting = std::move(m_waiting[slot]);
            m_waiting[slot].clear();
            Json j;
            std::string error;
            if (!parseBody(r, j, error)) {
                for (auto& [q, cb] : waiting) {
                    SearchResult out;
                    out.error = error;
                    cb(out);
                }
                return;
            }
            parseListing(slot, j);
            m_loaded[slot] = true;
            for (auto& [q, cb] : waiting) finishSearch(slot, q, cb);
        });
    }

    void plan(HttpClient& http, const WorkshopAsset& a, const std::string& variant, PlanDone done) override {
        HttpRequest req;
        req.url = "https://api.polyhaven.com/files/" + UrlEncode(a.id);
        req.maxBytes = kListingLimit;
        const WorkshopKind kind = a.kind;
        http.submit(std::move(req), [done = std::move(done), kind, variant, id = a.id](HttpResult& r) {
            DownloadPlan p;
            Json j;
            if (!parseBody(r, j, p.error)) return done(p);
            if (kind == WorkshopKind::Model) {
                const Json& g = j["gltf"][variant]["gltf"];
                if (g["url"].str().empty()) {
                    p.error = "no glTF at " + variant + " for this model";
                    return done(p);
                }
                DownloadFile main;
                main.url = g["url"].str();
                main.path = baseName(main.url);
                p.files.push_back(std::move(main));
                for (const auto& [path, v] : g["include"].members()) {
                    DownloadFile f;
                    f.url = v["url"].str();
                    f.path = path; // keeps "textures/..." so the glTF references still resolve
                    if (!f.url.empty()) p.files.push_back(std::move(f));
                }
            } else {
                // diffuse + GL normal + packed AO/Roughness/Metal (maps straight onto glTF's ORM) + height.
                // Canonical suffixes: the importer decides color / normal / data from the file name.
                static const struct { const char* map; const char* suffix; } kMaps[] = {
                    {"Diffuse", "diff"}, {"nor_gl", "normal"}, {"arm", "arm"}, {"Displacement", "disp"}};
                for (const auto& [map, suffix] : kMaps) {
                    const Json& byFormat = j[map][variant];
                    std::string url = byFormat["jpg"]["url"].str();
                    if (url.empty()) url = byFormat["png"]["url"].str();
                    if (url.empty()) continue;
                    DownloadFile f;
                    f.url = url;
                    const std::string base = baseName(url);
                    f.path = id + "_" + variant + "_" + suffix + base.substr(base.find_last_of('.'));
                    p.files.push_back(std::move(f));
                }
            }
            p.ok = !p.files.empty();
            if (!p.ok) p.error = "no files available at " + variant + " for '" + id + "'";
            done(p);
        });
    }

private:
    void parseListing(int slot, const Json& j) {
        std::vector<WorkshopAsset>& list = m_assets[slot];
        list.clear();
        for (const auto& [id, a] : j.members()) {
            WorkshopAsset w;
            w.provider = "Poly Haven";
            w.id = id;
            w.name = a["name"].str(id);
            for (const auto& [author, role] : a["authors"].members()) {
                if (!w.author.empty()) w.author += ", ";
                w.author += author;
            }
            w.license = "CC0 1.0";
            w.pageUrl = "https://polyhaven.com/a/" + id;
            w.kind = slot == 0 ? WorkshopKind::Material : WorkshopKind::Model;
            w.downloads = static_cast<unsigned long long>(a["download_count"].num());
            for (const Json& t : a["tags"].items()) w.tags.push_back(t.str());
            for (const Json& c : a["categories"].items()) w.category += (w.category.empty() ? "" : ", ") + c.str();
            std::string thumb = a["thumbnail_url"].str();
            const size_t webp = thumb.find("format=webp"); // stb_image cannot decode webp: ask the CDN for jpg
            if (webp != std::string::npos) thumb.replace(webp, 11, "format=jpg");
            w.thumbUrl = thumb;
            w.variants = slot == 0 ? std::vector<std::string>{"1k", "2k", "4k", "8k"} : std::vector<std::string>{"1k", "2k", "4k"};
            w.defaultVariant = 1;
            list.push_back(std::move(w));
        }
        std::stable_sort(list.begin(), list.end(),
                         [](const WorkshopAsset& a, const WorkshopAsset& b) { return a.downloads > b.downloads; });
    }

    void finishSearch(int slot, const SearchQuery& q, const SearchDone& done) {
        SearchResult out;
        out.ok = true;
        const std::string needle = lower(q.text);
        std::vector<const WorkshopAsset*> hits;
        for (const WorkshopAsset& a : m_assets[slot]) {
            if (!needle.empty()) {
                bool match = lower(a.name).find(needle) != std::string::npos || lower(a.category).find(needle) != std::string::npos;
                for (const std::string& t : a.tags) match = match || lower(t).find(needle) != std::string::npos;
                if (!match) continue;
            }
            hits.push_back(&a);
        }
        const size_t begin = static_cast<size_t>(q.page) * kPageSize;
        for (size_t i = begin; i < hits.size() && i < begin + kPageSize; ++i) out.items.push_back(*hits[i]);
        out.hasMore = begin + kPageSize < hits.size();
        done(out);
    }

    bool m_loaded[2] = {false, false};
    std::vector<WorkshopAsset> m_assets[2];
    std::vector<std::pair<SearchQuery, SearchDone>> m_waiting[2];
};

// ---- Sketchfab -------------------------------------------------------------------------------------

class Sketchfab final : public WorkshopProvider {
public:
    const char* name() const override { return "Sketchfab"; }
    bool supports(WorkshopKind kind) const override { return kind == WorkshopKind::Model; }
    bool needsToken() const override { return true; }
    const char* tokenHelpUrl() const override { return "https://sketchfab.com/settings/password"; }
    std::vector<std::pair<std::string, std::string>> licenseFilters() const override {
        return {{"CC0 (public domain)", "cc0"}, {"CC BY (attribution)", "by"}, {"CC BY-SA", "by-sa"}};
    }

    void search(HttpClient& http, const SearchQuery& q, SearchDone done) override {
        std::string url = "https://api.sketchfab.com/v3/search?type=models&downloadable=true&count=" + std::to_string(kPageSize) +
                          "&cursor=" + std::to_string(q.page * kPageSize);
        if (!q.text.empty()) url += "&q=" + UrlEncode(q.text);
        else url += "&sort_by=-likeCount";
        if (!q.license.empty()) url += "&license=" + UrlEncode(q.license);
        HttpRequest req;
        req.url = url;
        req.maxBytes = kListingLimit;
        http.submit(std::move(req), [done = std::move(done)](HttpResult& r) {
            SearchResult out;
            Json j;
            if (!parseBody(r, j, out.error)) return done(out);
            for (const Json& m : j["results"].items()) {
                WorkshopAsset w;
                w.provider = "Sketchfab";
                w.id = m["uid"].str();
                if (w.id.empty()) continue;
                w.name = m["name"].str(w.id);
                w.author = m["user"]["displayName"].str(m["user"]["username"].str());
                w.license = m["license"]["label"].str("see the model page");
                const std::string slug = m["license"]["slug"].str();
                w.nonCommercial = slug.find("nc") != std::string::npos;
                w.pageUrl = m["viewerUrl"].str("https://sketchfab.com/3d-models/" + w.id);
                w.kind = WorkshopKind::Model;
                w.downloads = static_cast<unsigned long long>(m["viewCount"].num());
                for (const Json& t : m["tags"].items()) w.tags.push_back(t["name"].str());
                // Smallest preview that is still at least 256 px wide.
                double best = 1e9;
                for (const Json& img : m["thumbnails"]["images"].items()) {
                    const double width = img["width"].num();
                    if (width >= 256 && width < best && !img["url"].str().empty()) {
                        best = width;
                        w.thumbUrl = img["url"].str();
                    }
                }
                w.variants = {"glTF"};
                out.items.push_back(std::move(w));
            }
            out.ok = true;
            out.hasMore = !j["next"].isNull() && !j["next"].str().empty();
            done(out);
        });
    }

    void plan(HttpClient& http, const WorkshopAsset& a, const std::string&, PlanDone done) override {
        if (m_token.empty()) {
            DownloadPlan p;
            p.error = "Sketchfab downloads need your free API token (Workshop > Settings).";
            return done(p);
        }
        HttpRequest req;
        req.url = "https://api.sketchfab.com/v3/models/" + UrlEncode(a.id) + "/download";
        req.headers.push_back("Authorization: Token " + m_token);
        http.submit(std::move(req), [done = std::move(done), id = a.id](HttpResult& r) {
            DownloadPlan p;
            Json j;
            if (r.status == 401 || r.status == 403) {
                p.error = "Sketchfab rejected the API token (HTTP " + std::to_string(r.status) + ")";
                return done(p);
            }
            if (!parseBody(r, j, p.error)) return done(p);
            const std::string url = j["gltf"]["url"].str();
            if (url.empty()) {
                p.error = "this model has no glTF download";
                return done(p);
            }
            DownloadFile f; // a signed storage URL: it must NOT receive the API token
            f.url = url;
            f.path = id + ".zip";
            f.isZip = true;
            p.files.push_back(std::move(f));
            p.ok = true;
            done(p);
        });
    }
};

} // namespace

std::string UrlEncode(const std::string& text) {
    std::string out;
    char buf[4];
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::vector<std::unique_ptr<WorkshopProvider>> CreateWorkshopProviders() {
    std::vector<std::unique_ptr<WorkshopProvider>> list;
    list.push_back(std::make_unique<AmbientCg>());
    list.push_back(std::make_unique<PolyHaven>());
    list.push_back(std::make_unique<Sketchfab>());
    return list;
}

} // namespace foh
