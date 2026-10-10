#include "asset_inspector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>

#include <imgui.h>
#include <stb_image.h>

#include "file_icons.h"
#include "foh/utf8_path.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* kindName(AssetKind k) {
    switch (k) {
        case AssetKind::Mesh: return "Mesh";
        case AssetKind::Material: return "Material";
        case AssetKind::Texture: return "Texture";
        case AssetKind::Prefab: return "Prefab";
        case AssetKind::PrefabVariant: return "Prefab variant";
        case AssetKind::Skeleton: return "Skeleton";
        case AssetKind::AnimationClip: return "Animation clip";
        default: return "Asset";
    }
}

// Reads only the header of an image file.
bool imageHeader(const fs::path& file, int& w, int& h, int& channels) {
#if defined(_WIN32)
    FILE* f = _wfopen(file.c_str(), L"rb");
#else
    FILE* f = std::fopen(file.string().c_str(), "rb");
#endif
    if (!f) return false;
    const bool ok = stbi_info_from_file(f, &w, &h, &channels) != 0;
    std::fclose(f);
    return ok;
}

std::string safeFileName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "Material" : out;
}

} // namespace

void AssetInspector::init(AssetDb& db, AssetPicker& picker, AssetInspectorHost host) {
    m_db = &db;
    m_picker = &picker;
    m_host = std::move(host);
}

// ---- selection -------------------------------------------------------------------------------------------

void AssetInspector::select(Target target, const std::string& path, AssetId asset) {
    flush();
    m_target = target;
    m_path = path;
    m_asset = asset;
    m_message.clear();
    m_matLoaded = false;
    m_users.clear();
    m_imageW = m_imageH = m_imageChannels = 0;
    m_imageBytes = 0;

    // The image header behind a texture or an image source: size and file size, without decoding it.
    const SourceRecord* rec = nullptr;
    if (target == Target::Source) rec = m_db->findSource(path);
    else if (const AssetInfo* info = m_db->find(asset)) rec = m_db->findSource(info->source);
    if (rec && rec->format == "image") {
        const fs::path file = m_db->sourceRoot() / PathFromUtf8(rec->path);
        imageHeader(file, m_imageW, m_imageH, m_imageChannels);
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        m_imageBytes = ec ? 0 : static_cast<unsigned long long>(size);
    }
    if (target == Target::Asset) m_users = m_db->dependents(asset);
}

void AssetInspector::inspectPath(const std::string& rel) {
    if (rel.empty()) return select(Target::None, {}, {});
    const std::string ext = lowerAscii(PathToUtf8(PathFromUtf8(rel).extension()));
    if (ext == ".fmat" || ext == ".fvariant") {
        const AssetId id = m_db->findAuthored(rel);
        if (id.isValid()) return inspectAsset(id);
    }
    select(m_db->findSource(rel) ? Target::Source : Target::None, rel, {});
}

void AssetInspector::inspectAsset(AssetId id) {
    if (m_target == Target::Asset && m_asset == id) return;
    select(m_db->find(id) ? Target::Asset : Target::None, {}, id);
}

void AssetInspector::flush() {
    if (!m_matDirty) return;
    m_matDirty = false;
    m_sinceFlush = 0.0f;
    if (!m_matLoaded || !m_matEditable) return;
    const AssetInfo* info = m_db->find(m_asset);
    if (!info) return;
    if (!m_db->saveMaterial(info->key, m_mat).isValid()) {
        m_message = "Could not write '" + info->key + "'.";
        return;
    }
    m_picker->touch(m_asset);
    if (m_host.changed) m_host.changed(m_asset);
}

// ---- drawing ---------------------------------------------------------------------------------------------

void AssetInspector::draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Inspector", open)) {
        ImGui::End();
        return;
    }
    if (!m_message.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_message.c_str());
    switch (m_target) {
        case Target::None:
            ImGui::TextDisabled("Nothing selected.\nClick a file in the Content Browser, or press 'i' on an asset field.");
            if (!m_path.empty()) ImGui::TextDisabled("No asset information for %s.", m_path.c_str());
            break;
        case Target::Source: drawSource(); break;
        case Target::Asset: drawAsset(); break;
    }
    // Edits are written when the drag ends, and at least every 0.2 s while it goes on, so the viewport follows live.
    if (m_matDirty) {
        m_sinceFlush += ImGui::GetIO().DeltaTime;
        if (!ImGui::IsAnyItemActive() || m_sinceFlush > 0.2f) flush();
    }
    ImGui::End();
}

void AssetInspector::drawLink(AssetId id) {
    const AssetInfo* info = m_db->find(id);
    if (!info) {
        ImGui::TextDisabled("(missing)");
        return;
    }
    const std::string label = std::string(kindName(info->kind)) + ": " + (info->name.empty() ? info->key : info->name);
    if (ImGui::Selectable(label.c_str())) inspectAsset(id);
}

void AssetInspector::drawUsedBy(AssetId) {
    if (m_users.empty()) return;
    ImGui::SeparatorText("Used by");
    ImGui::PushID("users");
    const size_t shown = std::min<size_t>(m_users.size(), 40);
    for (size_t i = 0; i < shown; ++i) {
        ImGui::PushID(static_cast<int>(i));
        drawLink(m_users[i]);
        ImGui::PopID();
    }
    if (m_users.size() > shown) ImGui::TextDisabled("... and %zu more", m_users.size() - shown);
    ImGui::PopID();
}

void AssetInspector::drawSource() {
    const SourceRecord* rec = m_db->findSource(m_path);
    if (!rec) {
        ImGui::TextDisabled("%s is not imported.", m_path.c_str());
        return;
    }
    ImGui::TextUnformatted(PathToUtf8(PathFromUtf8(rec->path).filename()).c_str());
    ImGui::TextDisabled("%s", rec->path.c_str());
    ImGui::Text("Format: %s", rec->format.c_str());
    if (m_imageW) ImGui::Text("Image: %d x %d, %d channel(s), %s on disk", m_imageW, m_imageH, m_imageChannels, FormatBytes(m_imageBytes).c_str());

    AssetId prefab;
    u32 counts[5] = {};
    for (const SubAssetRecord& s : rec->subAssets) {
        if (s.tombstone) continue;
        if (s.kind == AssetKind::Prefab) prefab = s.guid;
        switch (s.kind) {
            case AssetKind::Mesh: ++counts[0]; break;
            case AssetKind::Material: ++counts[1]; break;
            case AssetKind::Texture: ++counts[2]; break;
            case AssetKind::Prefab: ++counts[3]; break;
            default: ++counts[4]; break;
        }
    }
    ImGui::Text("%u mesh(es), %u material(s), %u texture(s)", counts[0], counts[1], counts[2]);
    u32 clipCount = 0;
    for (const SubAssetRecord& s : rec->subAssets) clipCount += (!s.tombstone && s.kind == AssetKind::AnimationClip) ? 1 : 0;
    if (clipCount) ImGui::Text("%u animation clip(s)", clipCount);

    if (prefab.isValid() && m_host.editPrefab) {
        if (ImGui::Button("Edit prefab...")) m_host.editPrefab(rec->path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fix scale, rotation, parts and materials of this model once, for every placement");
        ImGui::SameLine();
    }
    if (m_host.reimport && ImGui::Button("Reimport")) m_host.reimport(rec->path);

    if (rec->format != "image" && m_host.applyImportSettings) {
        if (m_settingsFor != rec->path) {
            m_settingsFor = rec->path;
            m_settings = rec->settings;
            std::snprintf(m_segments, sizeof(m_segments), "%s", m_settings.clipSegments.c_str());
        }
        ImGui::SeparatorText("Animation import");
        ImGui::Checkbox("Animation only (clips, no mesh or skeleton)", &m_settings.animationOnly);
        ImGui::TextDisabled("Clip segments (cut a long take into clips):");
        ImGui::InputTextMultiline("##segments", m_segments, sizeof(m_segments), ImVec2(-1.0f, ImGui::GetTextLineHeight() * 5.0f));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("name=start-end[:loop]; one per line or separated by ';'. Seconds on the take.\n"
                              "Example:  idle=14.0-22.5:loop; fidget=40-46.2\n"
                              ":loop moves the end to where the pose matches the start.");
        m_settings.clipSegments = m_segments;
        if (ImGui::Button("Apply and reimport")) m_host.applyImportSettings(rec->path, m_settings);
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            m_settings = rec->settings;
            std::snprintf(m_segments, sizeof(m_segments), "%s", m_settings.clipSegments.c_str());
        }
    }

    if (rec->format == "image") {
        for (const SubAssetRecord& s : rec->subAssets) {
            if (s.tombstone || s.kind != AssetKind::Texture) continue;
            const float size = ImGui::GetContentRegionAvail().x;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float side = std::min(size, ImGui::GetFontSize() * 18.0f);
            ImGui::Dummy(ImVec2(side, side));
            m_picker->thumb(ImGui::GetWindowDrawList(), s.guid, p, ImVec2(p.x + side, p.y + side));
            break;
        }
    }

    ImGui::SeparatorText("Contents");
    static const struct {
        AssetKind kind;
        const char* title;
    } kGroups[] = {{AssetKind::Prefab, "Prefab"}, {AssetKind::Material, "Materials"}, {AssetKind::Texture, "Textures"}, {AssetKind::Mesh, "Meshes"},
                 {AssetKind::AnimationClip, "Animation clips"}};
    for (const auto& g : kGroups) {
        u32 n = 0;
        for (const SubAssetRecord& s : rec->subAssets) n += (!s.tombstone && s.kind == g.kind) ? 1 : 0;
        if (n == 0) continue;
        if (!ImGui::TreeNodeEx(g.title, g.kind == AssetKind::Material ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s (%u)", g.title, n)) continue;
        int i = 0;
        for (const SubAssetRecord& s : rec->subAssets) {
            if (s.tombstone || s.kind != g.kind) continue;
            ImGui::PushID(i++);
            if (ImGui::Selectable(s.name.empty() ? s.key.c_str() : s.name.c_str())) inspectAsset(s.guid);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void AssetInspector::drawAsset() {
    const AssetInfo* info = m_db->find(m_asset);
    if (!info) {
        ImGui::TextDisabled("This asset no longer exists.");
        return;
    }
    ImGui::TextUnformatted(info->name.empty() ? info->key.c_str() : info->name.c_str());
    ImGui::TextDisabled("%s", kindName(info->kind));
    const SourceRecord* rec = m_db->findSource(info->source);
    if (rec) {
        ImGui::TextDisabled("from %s", rec->path.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("open source")) inspectPath(rec->path);
    } else {
        ImGui::TextDisabled("%s", info->key.c_str());
    }

    switch (info->kind) {
        case AssetKind::Material: drawMaterial(*info); break;
        case AssetKind::Texture: drawTexture(*info); break;
        case AssetKind::PrefabVariant: {
            PrefabVariantAsset v;
            if (m_db->load(m_asset, v)) {
                ImGui::Text("%zu edit(s)", v.overrides.size());
                ImGui::TextUnformatted("Based on");
                ImGui::SameLine();
                drawLink(v.base);
            }
            if (m_host.editPrefab && ImGui::Button("Edit in the prefab editor...")) m_host.editPrefab(info->key);
            break;
        }
        case AssetKind::Prefab:
            if (rec && m_host.editPrefab && ImGui::Button("Edit prefab...")) m_host.editPrefab(rec->path);
            break;
        default: break;
    }
    drawUsedBy(m_asset);
}

void AssetInspector::drawTexture(const AssetInfo&) {
    if (m_imageW) ImGui::Text("%d x %d, %d channel(s), source %s", m_imageW, m_imageH, m_imageChannels, FormatBytes(m_imageBytes).c_str());
    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 20.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    m_picker->thumb(ImGui::GetWindowDrawList(), m_asset, p, ImVec2(p.x + side, p.y + side));
}

void AssetInspector::makeEditableCopy(const AssetInfo& info) {
    const SourceRecord* rec = m_db->findSource(info.source);
    fs::path dir = rec ? PathFromUtf8(rec->path).parent_path() : fs::path();
    const std::string base = safeFileName(m_mat.name.empty() ? info.name : m_mat.name);
    std::string rel;
    for (int n = 0; n < 1000; ++n) {
        const fs::path candidate = dir / PathFromUtf8(base + (n ? "_" + std::to_string(n) : std::string()) + ".fmat");
        rel = PathToUtf8(candidate);
        std::replace(rel.begin(), rel.end(), '\\', '/');
        std::error_code ec;
        if (!m_db->findAuthored(rel).isValid() && !fs::exists(m_db->sourceRoot() / candidate, ec)) break;
    }
    const AssetId id = m_db->saveMaterial(rel, m_mat);
    if (!id.isValid()) {
        m_message = "Could not write '" + rel + "'.";
        return;
    }
    if (m_host.refreshBrowser) m_host.refreshBrowser();
    if (m_host.changed) m_host.changed(id);
    inspectAsset(id);
}

void AssetInspector::drawMaterial(const AssetInfo& info) {
    if (!m_matLoaded) {
        m_matLoaded = m_db->load(m_asset, m_mat);
        m_matEditable = !info.source.isValid(); // a model's own materials are rewritten by its next import
    }
    if (!m_matLoaded) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "Cannot read the material file.");
        return;
    }

    const float side = ImGui::GetFontSize() * 7.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    m_picker->thumb(ImGui::GetWindowDrawList(), m_asset, p, ImVec2(p.x + side, p.y + side));

    if (!m_matEditable) {
        ImGui::TextWrapped("This material belongs to the imported model: its next import would overwrite any change.");
        if (ImGui::Button("Make editable copy")) {
            makeEditableCopy(info);
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Creates a .fmat next to the model. Pick it in the prefab editor to use it on the model.");
    }

    bool c = false;
    ImGui::BeginDisabled(!m_matEditable);
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.0f);
    char name[128];
    std::snprintf(name, sizeof name, "%s", m_mat.name.c_str());
    if (ImGui::InputText("Name", name, sizeof name)) {
        m_mat.name = name;
        c = true;
    }
    c |= ImGui::ColorEdit4("Base color", &m_mat.baseColor.x);
    c |= ImGui::SliderFloat("Metallic", &m_mat.metallic, 0.0f, 1.0f);
    c |= ImGui::SliderFloat("Roughness", &m_mat.roughness, 0.0f, 1.0f);
    c |= ImGui::ColorEdit3("Emissive", &m_mat.emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    int mode = static_cast<int>(m_mat.alphaMode);
    if (ImGui::Combo("Alpha", &mode, "Opaque\0Mask (cutout)\0Blend\0")) {
        m_mat.alphaMode = static_cast<AlphaMode>(mode);
        c = true;
    }
    if (m_mat.alphaMode != AlphaMode::Opaque) c |= ImGui::SliderFloat("Alpha cutoff", &m_mat.alphaCutoff, 0.0f, 1.0f);
    c |= ImGui::Checkbox("Double sided", &m_mat.doubleSided);
    c |= ImGui::DragFloat("Normal strength", &m_mat.normalScale, 0.01f, 0.0f, 4.0f);
    c |= ImGui::SliderFloat("Occlusion strength", &m_mat.occlusionStrength, 0.0f, 1.0f);
    ImGui::PopItemWidth();

    ImGui::SeparatorText("Textures");
    struct Slot {
        const char* label;
        TextureRef* ref;
    };
    const Slot slots[] = {{"Base color", &m_mat.baseColorTexture},
                          {"Normal", &m_mat.normalTexture},
                          {"Metallic (B) / Roughness (G)", &m_mat.metallicRoughnessTexture},
                          {"Occlusion (R)", &m_mat.occlusionTexture},
                          {"Emissive", &m_mat.emissiveTexture}};
    int i = 0;
    for (const Slot& s : slots) {
        ImGui::PushID(i++);
        ImGui::TextUnformatted(s.label);
        c |= m_picker->field("##tex", s.ref->texture, AssetKind::Texture);
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    // A texture slot that was just assigned must be streamed in: the material reload (host.changed) does it,
    // because the saved material now depends on it.
    if (c && m_matEditable) m_matDirty = true;
}

} // namespace foh
