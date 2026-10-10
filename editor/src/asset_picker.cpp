#include "asset_picker.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "content_browser.h" // kAssetDragPayload
#include "file_icons.h"
#include "foh/utf8_path.h"
#include "thumb_loaders.h"

namespace foh {

namespace {

u64 hashBytes(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
template <class T>
u64 hashValue(const T& v, u64 h) {
    return hashBytes(&v, sizeof v, h);
}

bool containsNoCase(const std::string& hay, const char* needle) {
    if (!needle[0]) return true;
    const size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) == std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == n) return true;
    }
    return false;
}

const char* kindLabel(AssetKind k) {
    switch (k) {
        case AssetKind::Mesh: return "Mesh";
        case AssetKind::Material: return "Material";
        case AssetKind::Texture: return "Texture";
        case AssetKind::Prefab: return "Prefab";
        case AssetKind::PrefabVariant: return "Variant";
        case AssetKind::Skeleton: return "Skeleton";
        case AssetKind::AnimationClip: return "Animation";
        default: return "Asset";
    }
}

constexpr AssetKind kPickable[] = {AssetKind::Prefab, AssetKind::Mesh, AssetKind::Material, AssetKind::Texture, AssetKind::AnimationClip, AssetKind::Skeleton};

} // namespace

void AssetPicker::init(AssetDb& db, ThumbnailService& thumbs, AssetPickerHost host) {
    m_db = &db;
    m_thumbs = &thumbs;
    m_host = std::move(host);
}

AssetId AssetPicker::resolveDrop(const std::string& rel, AssetKind kind) const {
    const std::string ext = PathToUtf8(PathFromUtf8(rel).extension());
    if (ext == ".fmat") return kind == AssetKind::None || kind == AssetKind::Material ? m_db->findAuthored(rel) : AssetId{};
    if (ext == ".fvariant") return kind == AssetKind::None || kind == AssetKind::Prefab ? m_db->findAuthored(rel) : AssetId{};
    const SourceRecord* rec = m_db->findSource(rel);
    if (!rec) return {};
    const AssetKind want = kind == AssetKind::None ? AssetKind::Prefab : kind;
    for (const SubAssetRecord& s : rec->subAssets)
        if (!s.tombstone && s.kind == want) return s.guid;
    return {};
}

void AssetPicker::gather(AssetKind kind) {
    m_items.clear();
    for (const AssetKind k : kPickable) {
        if (kind != AssetKind::None && k != kind) continue;
        for (const AssetId id : m_db->assetsOfKind(k)) {
            const AssetInfo* info = m_db->find(id);
            if (!info) continue;
            Item it;
            it.id = id;
            it.kind = k;
            it.name = info->name.empty() ? info->key : info->name;
            if (const SourceRecord* rec = m_db->findSource(info->source)) it.where = rec->path;
            else it.where = info->key;
            m_items.push_back(std::move(it));
        }
        // A prefab variant is placeable like a prefab.
        if (k == AssetKind::Prefab && (kind == AssetKind::None || kind == AssetKind::Prefab)) {
            for (const AssetId id : m_db->assetsOfKind(AssetKind::PrefabVariant)) {
                const AssetInfo* info = m_db->find(id);
                if (!info) continue;
                m_items.push_back({id, info->name.empty() ? info->key : info->name, info->key, AssetKind::PrefabVariant});
            }
        }
    }
    std::stable_sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.name < b.name;
    });
}

bool AssetPicker::previewState(AssetId asset, ThumbRect& rect) {
    const AssetInfo* info = m_db->find(asset);
    if (!info) return false;
    const auto rev = m_revision.find(asset);
    const u32 revision = rev == m_revision.end() ? 0 : rev->second;
    ThumbnailService::State st = ThumbnailService::State::Unknown;

    if (info->kind == AssetKind::Texture) {
        const SourceRecord* rec = m_db->findSource(info->source);
        // Only standalone images have a decodable source file; a texture embedded in a model has none.
        if (!rec || rec->format != "image") return false;
        u64 key = hashBytes(rec->path.data(), rec->path.size(), 0x1111);
        key = hashValue(rec->contentHash, key);
        st = m_thumbs->lookup(key, rect);
        if (st == ThumbnailService::State::Unknown) {
            m_thumbs->request(key, [path = m_db->sourceRoot() / PathFromUtf8(rec->path)](ThumbPixels& out) { return LoadImageThumbnail(path, out); });
            st = ThumbnailService::State::Pending;
        }
    } else if (info->kind == AssetKind::Material) {
        u64 key = hashValue(asset, 0x3333);
        key = hashValue(revision, key);
        st = m_thumbs->lookup(key, rect);
        if (st == ThumbnailService::State::Unknown) {
            MaterialThumbSource src;
            MaterialAsset mat;
            if (m_db->load(asset, mat)) {
                src.baseColor = Vec3(mat.baseColor);
                src.roughness = mat.roughness;
                src.metallic = mat.metallic;
                if (mat.baseColorTexture.isValid()) {
                    if (const AssetInfo* ti = m_db->find(mat.baseColorTexture.texture))
                        if (const SourceRecord* sr = m_db->findSource(ti->source))
                            if (sr->format == "image") src.albedoImage = m_db->sourceRoot() / PathFromUtf8(sr->path);
                }
                if (mat.metallicRoughnessTexture.isValid()) { // the factors alone would be misleading next to a packed map
                    src.roughness = 0.55f;
                    src.metallic = 0.0f;
                }
            }
            m_thumbs->request(key, [src = std::move(src)](ThumbPixels& out) { return RenderMaterialThumbnail(src, out); });
            st = ThumbnailService::State::Pending;
        }
    } else if (info->kind == AssetKind::Prefab) {
        const SourceRecord* rec = m_db->findSource(info->source);
        if (!rec) return false;
        u64 key = hashBytes(rec->path.data(), rec->path.size(), 0x2222);
        key = hashValue(rec->contentHash, key);
        key = hashValue(rec->importerVersion, key);
        key = hashValue(asset, key);
        st = m_thumbs->lookup(key, rect);
        if (st == ThumbnailService::State::Unknown) {
            ModelThumbSource src;
            src.prefabFile = info->file;
            for (const SubAssetRecord& s : rec->subAssets) {
                if (s.tombstone || (s.kind != AssetKind::Mesh && s.kind != AssetKind::Material)) continue;
                if (const AssetInfo* ai = m_db->find(s.guid)) src.files.emplace(s.guid, ai->file);
            }
            m_thumbs->request(key, [src = std::move(src)](ThumbPixels& out) { return RenderModelThumbnail(src, out); });
            st = ThumbnailService::State::Pending;
        }
    }
    return st == ThumbnailService::State::Ready;
}

void AssetPicker::thumb(ImDrawList* dl, AssetId asset, ImVec2 a, ImVec2 b) {
    const AssetInfo* info = asset.isValid() ? m_db->find(asset) : nullptr;
    dl->AddRectFilled(a, b, IM_COL32(24, 26, 30, 255), 3.0f);
    if (!info) return;
    ThumbRect rect;
    if (previewState(asset, rect)) {
        const float w = b.x - a.x, h = b.y - a.y;
        float iw = w, ih = h;
        if (rect.aspect >= 1.0f) ih = w / rect.aspect;
        else iw = h * rect.aspect;
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        dl->AddImage(ImTextureRef(rect.texture), ImVec2(c.x - iw * 0.5f, c.y - ih * 0.5f), ImVec2(c.x + iw * 0.5f, c.y + ih * 0.5f), rect.uv0, rect.uv1);
        return;
    }
    // No preview (yet): a typed glyph.
    const float pad = (b.x - a.x) * 0.12f;
    const ImVec2 ia(a.x + pad, a.y + pad), ib(b.x - pad, b.y - pad);
    switch (info->kind) {
        case AssetKind::Texture: DrawFileIcon(dl, ia, ib, IM_COL32(110, 170, 230, 255), "IMG"); break;
        case AssetKind::Material:
            dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), (b.x - a.x) * 0.32f, IM_COL32(190, 140, 90, 255));
            break;
        default: DrawModelGlyph(dl, ia, ib, IM_COL32(160, 190, 150, 255)); break;
    }
}

void AssetPicker::assign(AssetId& value, AssetId next, bool& changed) {
    if (value == next) return;
    value = next;
    changed = true;
    if (next.isValid() && m_host.assigned) m_host.assigned(next);
}

bool AssetPicker::field(const char* id, AssetId& value, AssetKind kind) {
    bool changed = false;
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    const float rowH = h * 1.5f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool hasValue = value.isValid();
    const float side = (hasValue ? (m_host.inspect ? 2 : 1) : 0) * (h + spacing);
    const float w = std::max(ImGui::GetContentRegionAvail().x - side, h * 4.0f);

    const AssetInfo* info = hasValue ? m_db->find(value) : nullptr;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool open = ImGui::Button("##pick", ImVec2(w, rowH));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = (rowH - h) * 0.25f + 2.0f;
    if (hasValue) thumb(dl, value, ImVec2(p0.x + pad, p0.y + pad), ImVec2(p0.x + rowH - pad, p0.y + rowH - pad));
    const std::string label = !hasValue ? "None" : info ? (info->name.empty() ? info->key : info->name) : "(missing asset)";
    const float tx = p0.x + (hasValue ? rowH : 0.0f) + pad + 2.0f;
    const ImU32 col = ImGui::GetColorU32(hasValue && info ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    dl->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p0.x + w - 2.0f, p0.y + rowH), true);
    dl->AddText(ImVec2(tx, p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f - (info ? ImGui::GetTextLineHeight() * 0.5f : 0.0f)), col, label.c_str());
    if (info) {
        const char* k = kindLabel(info->kind);
        dl->AddText(ImVec2(tx, p0.y + rowH * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), k);
    }
    dl->PopClipRect();
    if (hovered && info) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(label.c_str());
        if (const SourceRecord* rec = m_db->findSource(info->source)) ImGui::TextDisabled("%s", rec->path.c_str());
        else ImGui::TextDisabled("%s", info->key.c_str());
        ImGui::TextDisabled("click to pick another, drop an asset here to assign it");
        ImGui::EndTooltip();
    }

    // Drop from the content browser.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const AssetId dropped = resolveDrop(static_cast<const char*>(payload->Data), kind);
            if (dropped.isValid()) assign(value, dropped, changed);
        }
        ImGui::EndDragDropTarget();
    }

    if (hasValue) {
        if (m_host.inspect) {
            ImGui::SameLine();
            if (ImGui::Button("i", ImVec2(h, rowH))) m_host.inspect(value);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show in the asset inspector");
        }
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(h, rowH))) assign(value, AssetId{}, changed);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear");
    }

    if (open) {
        gather(kind);
        m_search[0] = 0;
        m_kindTab = 0;
        m_focusSearch = true;
        ImGui::OpenPopup("##assetpick");
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, ImGui::GetFontSize() * 32.0f), ImGuiCond_Appearing);
    drawPopup(value, kind, changed);
    ImGui::PopID();
    return changed;
}

void AssetPicker::drawPopup(AssetId& value, AssetKind kind, bool& changed) {
    if (!ImGui::BeginPopup("##assetpick")) return;
    ImGui::TextDisabled("Pick %s", kind == AssetKind::None ? "an asset" : kindLabel(kind));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::InputTextWithHint("##search", "search name or file", m_search, sizeof m_search);

    AssetKind only = kind;
    if (kind == AssetKind::None) {
        static const char* kTabs[] = {"All", "Prefab", "Mesh", "Material", "Texture"};
        static const AssetKind kTabKinds[] = {AssetKind::None, AssetKind::Prefab, AssetKind::Mesh, AssetKind::Material, AssetKind::Texture};
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Selectable(kTabs[i], m_kindTab == i, 0, ImVec2(ImGui::CalcTextSize(kTabs[i]).x + 8.0f, 0))) m_kindTab = i;
        }
        only = kTabKinds[m_kindTab];
    }

    std::vector<const Item*> shown;
    shown.reserve(m_items.size());
    for (const Item& it : m_items) {
        if (only != AssetKind::None && it.kind != only && !(only == AssetKind::Prefab && it.kind == AssetKind::PrefabVariant)) continue;
        if (m_search[0] && !containsNoCase(it.name, m_search) && !containsNoCase(it.where, m_search)) continue;
        shown.push_back(&it);
    }

    const float rowH = ImGui::GetFrameHeight() * 1.9f;
    ImGui::Separator();
    // Fixed size: popups auto-resize to their content, so a child sized relative to the remaining
    // window space (0 / negative) shrinks a little every frame until the list is gone.
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginChild("##items", ImVec2(fs * 27.0f, fs * 24.0f))) {
        if (ImGui::Selectable("None", !value.isValid(), 0, ImVec2(0, ImGui::GetFrameHeight()))) {
            assign(value, AssetId{}, changed);
            ImGui::CloseCurrentPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()), rowH + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Item& it = *shown[static_cast<size_t>(i)];
                ImGui::PushID(i);
                const bool current = it.id == value;
                if (ImGui::Selectable("##row", current, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH))) {
                    assign(value, it.id, changed);
                    ImGui::CloseCurrentPopup();
                }
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                const float pad = 3.0f;
                thumb(dl, it.id, ImVec2(a.x + pad, a.y + pad), ImVec2(a.x + rowH - pad, b.y - pad));
                const float tx = a.x + rowH + 6.0f;
                dl->PushClipRect(a, b, true);
                dl->AddText(ImVec2(tx, a.y + rowH * 0.5f - ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_Text), it.name.c_str());
                dl->AddText(ImVec2(tx, a.y + rowH * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), it.where.c_str());
                const char* k = kindLabel(it.kind);
                const float kw = ImGui::CalcTextSize(k).x;
                dl->AddText(ImVec2(b.x - kw - 6.0f, a.y + 2.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), k);
                dl->PopClipRect();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%zu of %zu", shown.size(), m_items.size());
    ImGui::EndPopup();
}

} // namespace foh
