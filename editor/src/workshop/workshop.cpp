#include "workshop.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>

#include <imgui.h>

#include "file_icons.h"
#include "foh/log.h"
#include "foh/shell.h"
#include "foh/utf8_path.h"
#include "theme.h"
#include "thumb_loaders.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxThumbFetches = 4; // leaves transfer slots for searches and downloads

unsigned long long fnv(const std::string& s) {
    unsigned long long h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string safeName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (std::isalnum(c) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    return out.empty() ? "asset" : out;
}

std::string ellipsize(ImFont* font, float fs, const std::string& text, float width) {
    if (font->CalcTextSizeA(fs, 1e9f, 0.0f, text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty()) {
        do cut.pop_back(); while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
        if (font->CalcTextSizeA(fs, 1e9f, 0.0f, (cut + "...").c_str()).x <= width) return cut + "...";
    }
    return "...";
}

bool isImportable(const std::string& ext) {
    return ext == "gltf" || ext == "glb" || ext == "fbx" || ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "bmp";
}
bool isModelExt(const std::string& ext) { return ext == "gltf" || ext == "glb" || ext == "fbx"; }

// Texture variants that only duplicate another file of the same set (DirectX normal maps, preview renders).
bool isRedundantImage(const std::string& lowerName) {
    return lowerName.find("normaldx") != std::string::npos || lowerName.find("nor_dx") != std::string::npos ||
           lowerName.find("preview") != std::string::npos;
}

} // namespace

struct Workshop::ZipTask {
    JobCounter counter;
    fs::path archive;
    fs::path destination;
    ZipResult result;
    static void run(void* arg) {
        auto* t = static_cast<ZipTask*>(arg);
        t->result = ExtractZip(t->archive, t->destination);
    }
};

struct Workshop::Download {
    enum class Stage { Planning, Downloading, Extracting, Importing, Done, Failed };
    unsigned id = 0;
    WorkshopAsset asset;
    std::string variant;
    bool place = false;
    Stage stage = Stage::Planning;
    std::string message;
    fs::path dir;
    std::vector<DownloadFile> files;
    std::vector<HttpClient::Id> http;
    size_t filesDone = 0;
    bool cancelled = false;
    std::vector<fs::path> zips;
    size_t zipIndex = 0;
    std::unique_ptr<ZipTask> zip;
    std::vector<fs::path> imports;
    size_t importIndex = 0;
    std::vector<std::string> skipped;
    size_t extractedFiles = 0;
};

Workshop::Workshop() = default;

Workshop::~Workshop() { shutdown(); }

void Workshop::shutdown() {
    // Extraction jobs write into the project: let them finish instead of abandoning them mid-file.
    if (m_jobs) {
        for (auto& d : m_downloads)
            if (d->zip) m_jobs->wait(d->zip->counter);
    }
    m_downloads.clear();
    m_jobs = nullptr;
}

void Workshop::init(JobSystem& jobs, ThumbnailService& thumbs, EditorPrefs& prefs, WorkshopHost host) {
    m_jobs = &jobs;
    m_thumbs = &thumbs;
    m_prefs = &prefs;
    m_host = std::move(host);
    m_providers = CreateWorkshopProviders();
    const std::string token = prefs.get("workshop.sketchfab.token");
    std::snprintf(m_token, sizeof m_token, "%s", token.c_str());
    for (auto& p : m_providers)
        if (p->needsToken()) p->setToken(token);
    m_provider = 0;
}

// ---- search --------------------------------------------------------------------------------------------------

void Workshop::selectProvider(size_t index) {
    if (index == m_provider && !m_results.empty()) return;
    m_provider = index;
    if (!m_providers[m_provider]->supports(m_kind)) m_kind = m_kind == WorkshopKind::Material ? WorkshopKind::Model : WorkshopKind::Material;
    m_licenseFilter = 0;
    runSearch(false);
}

void Workshop::runSearch(bool append) {
    WorkshopProvider& provider = *m_providers[m_provider];
    if (!append) {
        m_page = 0;
        m_results.clear();
        m_selected = -1;
    } else {
        ++m_page;
    }
    m_searching = true;
    m_searchError.clear();
    const unsigned serial = ++m_serial;
    SearchQuery q;
    q.text = m_query;
    q.kind = m_kind;
    q.page = m_page;
    const auto filters = provider.licenseFilters();
    if (m_licenseFilter > 0 && static_cast<size_t>(m_licenseFilter) <= filters.size()) q.license = filters[static_cast<size_t>(m_licenseFilter) - 1].second;
    provider.search(m_http, q, [this, serial, kind = m_kind](SearchResult& r) {
        if (serial != m_serial) return; // a newer search replaced this one
        m_searching = false;
        if (!r.ok) {
            m_searchError = r.error;
            return;
        }
        for (WorkshopAsset& a : r.items) {
            a.kind = kind;
            m_results.push_back(std::move(a));
        }
        m_hasMore = r.hasMore;
    });
}

ThumbnailService::State Workshop::thumbnailFor(const WorkshopAsset& asset, ThumbRect& rect) {
    if (asset.thumbUrl.empty()) return ThumbnailService::State::Failed;
    const unsigned long long key = fnv(asset.thumbUrl);
    ThumbnailService::State st = m_thumbs->lookup(key, rect);
    if (st != ThumbnailService::State::Unknown) return st;
    if (m_thumbFailed.count(key)) return ThumbnailService::State::Failed;
    if (!m_thumbFetching.count(key) && m_thumbFetching.size() < kMaxThumbFetches) {
        m_thumbFetching.insert(key);
        HttpRequest req;
        req.url = asset.thumbUrl;
        req.maxBytes = 12ull << 20;
        m_http.submit(std::move(req), [this, key](HttpResult& r) {
            m_thumbFetching.erase(key);
            if (!r.ok) {
                m_thumbFailed.insert(key);
                return;
            }
            auto bytes = std::make_shared<std::string>(std::move(r.body));
            m_thumbs->request(key, [bytes](ThumbPixels& out) {
                return DecodeImageThumbnail(reinterpret_cast<const u8*>(bytes->data()), bytes->size(), out);
            });
        });
    }
    return ThumbnailService::State::Pending;
}

// ---- downloads ---------------------------------------------------------------------------------------------

Workshop::Download* Workshop::findDownload(unsigned id) {
    for (auto& d : m_downloads)
        if (d->id == id) return d.get();
    return nullptr;
}

void Workshop::startDownload(const WorkshopAsset& asset, const std::string& variant, bool place) {
    auto d = std::make_unique<Download>();
    d->id = m_nextDownload++;
    d->asset = asset;
    d->variant = variant;
    d->place = place && asset.kind == WorkshopKind::Model;
    d->message = "Resolving files...";
    d->dir = m_host.assetRoot / "workshop" / safeName(asset.provider) / (safeName(asset.id) + "_" + safeName(variant));
    const unsigned id = d->id;
    WorkshopProvider* provider = nullptr;
    for (auto& p : m_providers)
        if (asset.provider == p->name()) provider = p.get();
    m_downloads.push_back(std::move(d));
    if (!provider) {
        finish(*m_downloads.back(), false, "unknown source");
        return;
    }
    provider->plan(m_http, asset, variant, [this, id](DownloadPlan& plan) { onPlan(id, plan); });
}

void Workshop::onPlan(unsigned id, DownloadPlan& plan) {
    Download* d = findDownload(id);
    if (!d || d->stage != Download::Stage::Planning) return;
    if (d->cancelled) return finish(*d, false, "cancelled");
    if (!plan.ok) return finish(*d, false, plan.error.empty() ? "nothing to download" : plan.error);

    // Everything in a plan comes from a web API: validate each target path before it touches the disk.
    for (DownloadFile& f : plan.files) {
        std::string why;
        std::string clean = SanitizeRelativePath(f.path, why);
        if (clean.empty()) return finish(*d, false, "refusing unsafe file path '" + f.path + "' (" + why + ")");
        f.path = std::move(clean);
    }
    d->files = std::move(plan.files);
    d->stage = Download::Stage::Downloading;
    d->message = "Downloading...";
    std::error_code ec;
    fs::create_directories(d->dir, ec);
    for (size_t i = 0; i < d->files.size(); ++i) {
        const DownloadFile& f = d->files[i];
        HttpRequest req;
        req.url = f.url;
        req.headers = f.headers;
        req.toFile = d->dir / PathFromUtf8(f.path);
        req.maxBytes = 3ull << 30;
        d->http.push_back(m_http.submit(std::move(req), [this, id, i](HttpResult& r) { onFileDone(id, i, r); }));
    }
}

void Workshop::onFileDone(unsigned id, size_t index, HttpResult& result) {
    Download* d = findDownload(id);
    if (!d || d->stage != Download::Stage::Downloading) return;
    if (!result.ok) {
        // First failure ends the package: cancel the rest so no half packages accumulate.
        d->cancelled = true;
        for (HttpClient::Id h : d->http) m_http.cancel(h);
        return finish(*d, false, d->files[index].path + ": " + result.error);
    }
    if (d->files[index].isZip) d->zips.push_back(result.file);
    ++d->filesDone;
    if (d->filesDone == d->files.size()) afterTransfers(*d);
}

void Workshop::afterTransfers(Download& d) {
    if (d.zips.empty()) return beginImport(d);
    d.stage = Download::Stage::Extracting;
    d.message = "Extracting...";
    d.zipIndex = 0;
    d.zip = std::make_unique<ZipTask>();
    d.zip->archive = d.zips[0];
    d.zip->destination = d.dir;
    m_jobs->submit(&ZipTask::run, d.zip.get(), d.zip->counter);
}

void Workshop::beginImport(Download& d) {
    writeAttribution(d);
    // What to import: models when the package has any (their textures come along through the model),
    // otherwise the texture set.
    std::vector<fs::path> models, images;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(d.dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = lowerAscii(PathToUtf8(it->path().filename()));
        std::string ext = lowerAscii(PathToUtf8(it->path().extension()));
        if (!ext.empty()) ext.erase(0, 1);
        if (!isImportable(ext)) continue;
        if (isModelExt(ext)) models.push_back(it->path());
        else if (!isRedundantImage(name) && lowerAscii(PathToUtf8(it->path().stem())) != lowerAscii(d.asset.id)) images.push_back(it->path()); // <id>.png is the pack's preview render
    }
    if (!models.empty()) {
        // glTF/GLB first: the engine's primary format. FBX only when it is all there is.
        std::vector<fs::path> gltfs;
        for (const fs::path& m : models) {
            const std::string e = lowerAscii(PathToUtf8(m.extension()));
            if (e == ".gltf" || e == ".glb") gltfs.push_back(m);
        }
        d.imports = gltfs.empty() ? models : gltfs;
    } else {
        d.imports = images;
    }
    std::sort(d.imports.begin(), d.imports.end());
    if (d.imports.empty()) {
        finish(d, true, "Downloaded (no importable files found; see the package folder)");
        return;
    }
    d.stage = Download::Stage::Importing;
    d.message = "Importing...";
    d.importIndex = 0;
}

void Workshop::stepImport(Download& d) {
    if (d.importIndex < d.imports.size()) {
        if (m_host.importFile) m_host.importFile(d.imports[d.importIndex]);
        ++d.importIndex;
        d.message = "Importing " + std::to_string(d.importIndex) + "/" + std::to_string(d.imports.size());
        return;
    }
    if (d.place && m_host.placeModel && !d.imports.empty()) {
        std::error_code ec;
        const fs::path rel = d.imports.front().lexically_relative(m_host.assetRoot);
        if (!rel.empty()) m_host.placeModel(PathToUtf8(rel));
    }
    finish(d, true, "Imported " + std::to_string(d.imports.size()) + " file(s)" + (d.place ? " and placed in the scene" : ""));
}

void Workshop::finish(Download& d, bool ok, const std::string& message) {
    d.stage = ok ? Download::Stage::Done : Download::Stage::Failed;
    d.message = message;
    if (ok) {
        FOH_INFO("workshop: {} '{}' ({}): {}", d.asset.provider, d.asset.name, d.variant, message);
        if (m_host.refreshBrowser) m_host.refreshBrowser();
    } else {
        FOH_WARN("workshop: {} '{}' failed: {}", d.asset.provider, d.asset.name, message);
        std::error_code ec; // leave nothing half-downloaded behind when no file made it
        if (d.filesDone == 0 && fs::exists(d.dir, ec) && fs::is_empty(d.dir, ec)) fs::remove(d.dir, ec);
    }
}

void Workshop::writeAttribution(const Download& d) {
    std::ofstream out(d.dir / "ATTRIBUTION.txt", std::ios::trunc);
    if (!out) return;
    const std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    std::strftime(date, sizeof date, "%Y-%m-%d", &tmv);
    out << "Asset:      " << d.asset.name << "\n"
        << "Source:     " << d.asset.provider << "\n"
        << "Author:     " << d.asset.author << "\n"
        << "License:    " << d.asset.license << (d.asset.nonCommercial ? "  (NON-COMMERCIAL: do not ship in a commercial build)" : "") << "\n"
        << "Page:       " << d.asset.pageUrl << "\n"
        << "Variant:    " << d.variant << "\n"
        << "Downloaded: " << date << " (UTC)\n";
}

void Workshop::update() {
    if (!m_jobs) return;
    m_http.poll();
    if (m_selfTest && !m_results.empty() && !m_results[0].variants.empty()) {
        m_selfTest = false;
        startDownload(m_results[0], m_results[0].variants[0], false);
    }
    for (auto& up : m_downloads) {
        Download& d = *up;
        if (d.stage == Download::Stage::Extracting && d.zip && d.zip->counter.pending.load(std::memory_order_acquire) == 0) {
            ZipResult& r = d.zip->result;
            for (const std::string& s : r.skipped) {
                d.skipped.push_back(s);
                FOH_WARN("workshop: skipped zip entry {}", s);
            }
            const fs::path archive = d.zip->archive;
            d.zip.reset();
            if (!r.ok) {
                finish(d, false, "extraction failed: " + r.error);
                continue;
            }
            d.extractedFiles += r.files.size();
            std::error_code ec;
            fs::remove(archive, ec); // the extracted files replace it
            if (++d.zipIndex < d.zips.size()) {
                d.zip = std::make_unique<ZipTask>();
                d.zip->archive = d.zips[d.zipIndex];
                d.zip->destination = d.dir;
                m_jobs->submit(&ZipTask::run, d.zip.get(), d.zip->counter);
            } else {
                beginImport(d);
            }
        } else if (d.stage == Download::Stage::Importing) {
            stepImport(d); // one file per frame: imports are synchronous and must not freeze the UI
            break;
        }
    }
}

// ---- ui ------------------------------------------------------------------------------------------------------

void Workshop::draw(bool* open, float scale) {
    m_scale = scale;
    ImGui::SetNextWindowSize(ImVec2(1180.0f * scale, 760.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Assets Workshop", open)) {
        ImGui::End();
        return;
    }
    if (m_firstOpen) {
        m_firstOpen = false;
        runSearch(false);
    }
    drawToolbar(scale);
    ImGui::Separator();

    const float downloadsH = std::min(150.0f * scale, ImGui::GetContentRegionAvail().y * 0.3f);
    const float bodyH = ImGui::GetContentRegionAvail().y - downloadsH - ImGui::GetStyle().ItemSpacing.y;
    const float detailW = 320.0f * scale;

    ImGui::BeginChild("##results", ImVec2(ImGui::GetContentRegionAvail().x - detailW - ImGui::GetStyle().ItemSpacing.x, bodyH), ImGuiChildFlags_Borders);
    drawResults(scale);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##detail", ImVec2(0, bodyH), ImGuiChildFlags_Borders);
    drawDetail(scale);
    ImGui::EndChild();

    ImGui::BeginChild("##downloads", ImVec2(0, 0), ImGuiChildFlags_Borders);
    drawDownloads(scale);
    ImGui::EndChild();
    ImGui::End();
}

void Workshop::drawToolbar(float scale) {
    if (ImGui::BeginTabBar("##sources")) {
        for (size_t i = 0; i < m_providers.size(); ++i) {
            if (ImGui::BeginTabItem(m_providers[i]->name())) {
                if (i != m_provider) selectProvider(i);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    WorkshopProvider& provider = *m_providers[m_provider];

    // Kind selector (only what the source offers).
    if (provider.supports(WorkshopKind::Material) && provider.supports(WorkshopKind::Model)) {
        ImGui::SetNextItemWidth(130.0f * scale);
        const char* kinds[] = {"Materials", "Models"};
        int k = m_kind == WorkshopKind::Material ? 0 : 1;
        if (ImGui::Combo("##kind", &k, kinds, 2)) {
            m_kind = k == 0 ? WorkshopKind::Material : WorkshopKind::Model;
            runSearch(false);
        }
        ImGui::SameLine();
    } else {
        m_kind = provider.supports(WorkshopKind::Material) ? WorkshopKind::Material : WorkshopKind::Model;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", m_kind == WorkshopKind::Material ? "Materials" : "Models");
        ImGui::SameLine();
    }

    ImGui::SetNextItemWidth(320.0f * scale);
    const bool enter = ImGui::InputTextWithHint("##q", "Search (wood, brick, car...)", m_query, sizeof m_query, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search") || enter) runSearch(false);

    const auto filters = provider.licenseFilters();
    if (!filters.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170.0f * scale);
        const char* preview = m_licenseFilter == 0 ? "Any license" : filters[static_cast<size_t>(m_licenseFilter) - 1].first.c_str();
        if (ImGui::BeginCombo("##lic", preview)) {
            if (ImGui::Selectable("Any license", m_licenseFilter == 0)) {
                m_licenseFilter = 0;
                runSearch(false);
            }
            for (size_t i = 0; i < filters.size(); ++i) {
                if (ImGui::Selectable(filters[i].first.c_str(), m_licenseFilter == static_cast<int>(i) + 1)) {
                    m_licenseFilter = static_cast<int>(i) + 1;
                    runSearch(false);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings")) m_showSettings = true;

    if (m_showSettings) {
        ImGui::OpenPopup("Workshop settings");
        m_showSettings = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Workshop settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Sketchfab API token");
        ImGui::TextDisabled("Searching is free; downloading needs your personal token\n(sketchfab.com > Settings > Password & API).");
        ImGui::SetNextItemWidth(420.0f * scale);
        if (ImGui::InputText("##token", m_token, sizeof m_token, ImGuiInputTextFlags_Password)) {
            m_prefs->set("workshop.sketchfab.token", m_token);
            for (auto& p : m_providers)
                if (p->needsToken()) p->setToken(m_token);
        }
        ImGui::TextDisabled("Stored in plain text in editor_prefs.txt next to the editor. Never commit that file.");
        if (ImGui::Button("Open token page")) OpenUrl("https://sketchfab.com/settings/password");
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100.0f * scale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Workshop::drawResults(float scale) {
    if (!m_searchError.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "Search failed: %s", m_searchError.c_str());
        if (ImGui::Button("Retry")) runSearch(false);
        return;
    }
    if (m_results.empty()) {
        ImGui::Dummy(ImVec2(0, 40.0f * scale));
        ImGui::TextDisabled(m_searching ? "Loading..." : "No results.");
        return;
    }
    const float tile = 150.0f * scale;
    const float gap = 10.0f * scale;
    const float textH = ImGui::GetTextLineHeight() * 3.0f + 10.0f * scale;
    const float availW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    const int cols = std::max(1, static_cast<int>((availW + gap) / (tile + gap)));
    const int count = static_cast<int>(m_results.size());
    const int rows = (count + cols - 1) / cols;
    const float rowH = tile + textH + gap;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGuiListClipper clipper;
    clipper.Begin(rows, rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int i = r * cols + c;
                if (i >= count) break;
                drawTile(m_results[static_cast<size_t>(i)], static_cast<size_t>(i),
                         ImVec2(origin.x + gap * 0.5f + static_cast<float>(c) * (tile + gap), origin.y + gap * 0.5f + static_cast<float>(r) * rowH),
                         tile, textH, scale);
            }
        }
    }
    clipper.End();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(rows) * rowH));
    ImGui::Dummy(ImVec2(1, 1));

    // Infinite scroll: ask for the next page when the end is near.
    const bool nearEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - rowH * 1.5f;
    if (m_hasMore && !m_searching && nearEnd) runSearch(true);
    if (m_searching) ImGui::TextDisabled("Loading...");
}

void Workshop::drawTile(const WorkshopAsset& a, size_t index, ImVec2 pos, float tile, float textH, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(static_cast<int>(index));
    ImGui::InvisibleButton("##tile", ImVec2(tile, tile + textH));
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = static_cast<int>(index) == m_selected;
    if (ImGui::IsItemClicked()) {
        if (m_selected != static_cast<int>(index)) m_variant = static_cast<int>(a.defaultVariant);
        m_selected = static_cast<int>(index);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) startDownload(a, a.variants[a.defaultVariant], m_placeAfter);

    const float round = 6.0f * scale;
    const ImVec2 max(pos.x + tile, pos.y + tile + textH);
    dl->AddRectFilled(pos, max, selected ? IM_COL32(40, 64, 98, 255) : hovered ? IM_COL32(50, 53, 60, 255) : EditorTheme::tileBg(), round);
    const float inset = 4.0f * scale;
    const ImVec2 pa(pos.x + inset, pos.y + inset), pb(pos.x + tile - inset, pos.y + tile - inset);
    dl->AddRectFilled(pa, pb, IM_COL32(26, 28, 32, 255), round - 2.0f * scale);

    ThumbRect rect;
    const ThumbnailService::State st = thumbnailFor(a, rect);
    if (st == ThumbnailService::State::Ready) {
        const float w = pb.x - pa.x, h = pb.y - pa.y;
        float iw = w, ih = h;
        if (rect.aspect >= 1.0f) ih = w / rect.aspect;
        else iw = h * rect.aspect;
        const ImVec2 c((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
        dl->AddImageRounded(ImTextureRef(rect.texture), ImVec2(c.x - iw * 0.5f, c.y - ih * 0.5f), ImVec2(c.x + iw * 0.5f, c.y + ih * 0.5f),
                            rect.uv0, rect.uv1, IM_COL32_WHITE, round - 2.0f * scale);
    } else if (st == ThumbnailService::State::Failed) {
        DrawModelGlyph(dl, pa, pb, IM_COL32(120, 126, 136, 255));
    } else {
        const float t = static_cast<float>(ImGui::GetTime()) * 5.0f;
        const ImVec2 c((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
        dl->PathArcTo(c, 10.0f * scale, t, t + 4.4f, 18);
        dl->PathStroke(IM_COL32(180, 186, 196, 220), 0, 2.0f * scale);
    }

    // License pill (bottom-left of the preview) and downloads.
    {
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * 0.78f;
        const std::string lic = a.license.size() > 14 ? a.license.substr(0, 14) : a.license;
        const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, lic.c_str());
        const ImVec2 mn(pa.x + 4.0f * scale, pb.y - ts.y - 8.0f * scale), mx(mn.x + ts.x + 10.0f * scale, pb.y - 4.0f * scale);
        dl->AddRectFilled(mn, mx, a.nonCommercial ? IM_COL32(150, 90, 20, 230) : IM_COL32(12, 13, 15, 210), 4.0f * scale);
        dl->AddText(font, fs, ImVec2(mn.x + 5.0f * scale, mn.y + 2.0f * scale), IM_COL32(225, 229, 235, 255), lic.c_str());
    }

    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float tw = tile - 10.0f * scale;
    const float tx = pos.x + 5.0f * scale;
    const float ty = pos.y + tile + 3.0f * scale;
    const float lineH = ImGui::GetTextLineHeight();
    const char* s = a.name.c_str();
    const char* end = s + a.name.size();
    const char* l1 = font->CalcWordWrapPosition(fs, s, end, tw);
    if (l1 == s) l1 = s + 1;
    auto centered = [&](const std::string& t, float y, ImU32 col) {
        const std::string fitted = ellipsize(font, fs, t, tw);
        const float w = font->CalcTextSizeA(fs, 1e9f, 0.0f, fitted.c_str()).x;
        dl->AddText(font, fs, ImVec2(tx + (tw - w) * 0.5f, y), col, fitted.c_str());
    };
    centered(std::string(s, l1), ty, ImGui::GetColorU32(ImGuiCol_Text));
    if (l1 != end) centered(std::string(l1, end), ty + lineH, ImGui::GetColorU32(ImGuiCol_Text));
    centered(a.author, ty + lineH * 2.0f + 2.0f * scale, IM_COL32(128, 135, 148, 255));
    if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(a.name.c_str());
        ImGui::TextDisabled("by %s  |  %s", a.author.c_str(), a.license.c_str());
        ImGui::TextDisabled("Double-click to download and import");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

void Workshop::drawDetail(float scale) {
    if (m_selected < 0 || static_cast<size_t>(m_selected) >= m_results.size()) {
        ImGui::TextDisabled("Select an asset to see its details.\nDouble-click an asset to download and import it right away.");
        return;
    }
    const WorkshopAsset& a = m_results[static_cast<size_t>(m_selected)];
    WorkshopProvider* provider = nullptr;
    for (auto& p : m_providers)
        if (a.provider == p->name()) provider = p.get();

    const float side = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side * 0.8f);
    dl->AddRectFilled(p0, p1, IM_COL32(26, 28, 32, 255), 6.0f * scale);
    ThumbRect rect;
    if (thumbnailFor(a, rect) == ThumbnailService::State::Ready) {
        float iw = side, ih = side * 0.8f;
        if (rect.aspect >= iw / ih) ih = iw / rect.aspect;
        else iw = ih * rect.aspect;
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        dl->AddImage(ImTextureRef(rect.texture), ImVec2(c.x - iw * 0.5f, c.y - ih * 0.5f), ImVec2(c.x + iw * 0.5f, c.y + ih * 0.5f), rect.uv0, rect.uv1);
    }
    ImGui::Dummy(ImVec2(side, side * 0.8f));

    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(a.name.c_str());
    ImGui::TextDisabled("by %s", a.author.c_str());
    ImGui::Spacing();
    ImGui::Text("License");
    ImGui::SameLine();
    ImGui::TextColored(a.nonCommercial ? ImVec4(0.95f, 0.7f, 0.3f, 1) : ImVec4(0.45f, 0.85f, 0.55f, 1), "%s", a.license.c_str());
    if (a.nonCommercial) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "Non-commercial license: do not use in a shipped game.");
    if (!a.category.empty()) ImGui::TextDisabled("Category: %s", a.category.c_str());
    if (a.downloads) ImGui::TextDisabled("%llu %s", a.downloads, a.provider == "Sketchfab" ? "views" : "downloads");
    if (!a.tags.empty()) {
        std::string tags;
        for (size_t i = 0; i < a.tags.size() && i < 10; ++i) tags += (i ? ", " : "") + a.tags[i];
        ImGui::TextDisabled("%s", tags.c_str());
    }
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Open page in browser")) OpenUrl(a.pageUrl);
    ImGui::Separator();

    if (a.variants.size() > 1) {
        ImGui::SetNextItemWidth(side * 0.6f);
        m_variant = std::clamp(m_variant, 0, static_cast<int>(a.variants.size()) - 1);
        if (ImGui::BeginCombo("Quality", a.variants[static_cast<size_t>(m_variant)].c_str())) {
            for (size_t i = 0; i < a.variants.size(); ++i)
                if (ImGui::Selectable(a.variants[i].c_str(), static_cast<int>(i) == m_variant)) m_variant = static_cast<int>(i);
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Higher resolutions are much larger downloads.");
    }
    if (a.kind == WorkshopKind::Model) ImGui::Checkbox("Place in scene when done", &m_placeAfter);

    const bool needToken = provider && provider->needsToken() && m_token[0] == '\0';
    ImGui::BeginDisabled(needToken);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(EditorTheme::accent()));
    if (ImGui::Button("Download & import", ImVec2(side, 0))) {
        const std::string variant = a.variants.empty() ? std::string() : a.variants[static_cast<size_t>(std::clamp(m_variant, 0, static_cast<int>(a.variants.size()) - 1))];
        startDownload(a, variant, m_placeAfter);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (needToken) {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "Downloads from %s need your API token.", a.provider.c_str());
        if (ImGui::SmallButton("Enter token")) m_showSettings = true;
    }
}

void Workshop::drawDownloads(float scale) {
    ImGui::TextDisabled("Downloads");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear finished")) {
        for (auto it = m_downloads.begin(); it != m_downloads.end();) {
            const auto st = (*it)->stage;
            it = (st == Download::Stage::Done || st == Download::Stage::Failed) ? m_downloads.erase(it) : std::next(it);
        }
    }
    if (m_downloads.empty()) {
        ImGui::TextDisabled("Nothing yet.");
        return;
    }
    for (auto it = m_downloads.rbegin(); it != m_downloads.rend(); ++it) {
        Download& d = **it;
        ImGui::PushID(static_cast<int>(d.id));
        const bool active = d.stage != Download::Stage::Done && d.stage != Download::Stage::Failed;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  (%s, %s)", d.asset.name.c_str(), d.asset.provider.c_str(), d.variant.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
        if (d.stage == Download::Stage::Downloading) {
            std::uint64_t rec = 0, tot = 0;
            for (HttpClient::Id h : d.http) {
                HttpProgress p;
                if (m_http.progress(h, p)) {
                    rec += p.received;
                    tot += p.total;
                }
            }
            const float frac = d.files.empty() ? 0.0f : (static_cast<float>(d.filesDone) + (tot ? static_cast<float>(rec) / static_cast<float>(tot) : 0.0f)) / static_cast<float>(d.files.size());
            char label[64];
            std::snprintf(label, sizeof label, "%zu/%zu files  %s", d.filesDone, d.files.size(), FormatBytes(rec).c_str());
            ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(220.0f * scale, 0), label);
        } else if (active) {
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(220.0f * scale, 0), d.message.c_str());
        } else {
            ImGui::TextColored(d.stage == Download::Stage::Done ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", d.message.c_str());
        }
        if (active) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                d.cancelled = true;
                for (HttpClient::Id h : d.http) m_http.cancel(h);
                if (d.stage == Download::Stage::Planning) finish(d, false, "cancelled");
            }
        } else if (d.stage == Download::Stage::Done) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show folder")) RevealInFileManager(d.dir);
        }
        if (!d.skipped.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu entries skipped)", d.skipped.size());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                for (size_t i = 0; i < d.skipped.size() && i < 12; ++i) ImGui::TextUnformatted(d.skipped[i].c_str());
                ImGui::EndTooltip();
            }
        }
        ImGui::PopID();
    }
}

} // namespace foh
