#pragma once

// The Asset Inspector: what the Inspector window is for entities, for assets. It follows the content browser's
// selection (and can be pointed at an asset from a picker field with its "i" button).
//
//   material (.fmat)       every parameter and texture slot is editable; changes are written to the file and
//                          reloaded wherever the material is in use, live
//   material of a model    the same fields, read-only (the model's import owns them), with "Make editable
//                          copy" that authors a .fmat next to the model, to pick in the prefab editor
//   texture                preview, size, where it comes from, who uses it
//   model / source         its sub-assets (click to inspect), edit prefab, reimport
//   prefab variant         what it is based on and how many edits it holds
//
// Thread-safety: main thread only.

#include <functional>
#include <string>
#include <vector>

#include "asset_picker.h"
#include "foh/asset_db.h"

namespace foh {

struct AssetInspectorHost {
    // The content of an asset was rewritten: reload it wherever it is in use.
    std::function<void(AssetId)> changed;
    // Opens the prefab editor on a model or variant (path relative to the asset root).
    std::function<void(const std::string& relPath)> editPrefab;
    // Starts a forced reimport of a source (path relative to the asset root).
    std::function<void(const std::string& relPath)> reimport;
    std::function<void()> refreshBrowser;
};

class AssetInspector {
public:
    void init(AssetDb& db, AssetPicker& picker, AssetInspectorHost host);

    // A content browser selection: a source file, a ".fmat" or a ".fvariant" (path relative to the asset root).
    void inspectPath(const std::string& relPath);
    void inspectAsset(AssetId id);

    // Writes a pending material edit (also called when the target changes).
    void flush();
    void draw(bool* open);

private:
    enum class Target : u8 { None, Source, Asset };

    void select(Target target, const std::string& path, AssetId asset);
    void drawSource();
    void drawAsset();
    void drawMaterial(const AssetInfo& info);
    void drawTexture(const AssetInfo& info);
    void drawUsedBy(AssetId id);
    void drawLink(AssetId id);
    void makeEditableCopy(const AssetInfo& info);

    AssetDb* m_db = nullptr;
    AssetPicker* m_picker = nullptr;
    AssetInspectorHost m_host;

    Target m_target = Target::None;
    std::string m_path;
    AssetId m_asset;
    std::vector<AssetId> m_users; // assets that reference m_asset (computed once per selection)

    // The material being edited (a working copy of the file).
    MaterialAsset m_mat;
    bool m_matLoaded = false;
    bool m_matEditable = false;
    bool m_matDirty = false;
    float m_sinceFlush = 0.0f;

    // Header of the source image of a texture / image source.
    int m_imageW = 0, m_imageH = 0, m_imageChannels = 0;
    unsigned long long m_imageBytes = 0;
    std::string m_message;
};

} // namespace foh
