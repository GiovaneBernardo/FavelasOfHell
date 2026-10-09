#pragma once

// Asset reference widgets shared by the component inspector and the asset inspector:
//
//   field()   a button showing the current asset (thumbnail + name); clicking opens a popup that lists every
//             asset of the wanted kind (searchable, with previews); dropping an asset from the content
//             browser assigns it too
//   thumb()   paints an asset's preview into a rectangle
//
// Previews come from the thumbnail service: textures from their source image, materials as a lit sphere,
// prefabs rasterized. Everything else gets a typed glyph.
//
// Thread-safety: main thread only.

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "foh/asset_db.h"
#include "thumbnails.h"

namespace foh {

struct AssetPickerHost {
    // After a field assigned a different asset: the editor streams it to the renderer.
    std::function<void(AssetId)> assigned;
    // "Inspect" button on a field: shows the asset in the asset inspector.
    std::function<void(AssetId)> inspect;
};

class AssetPicker {
public:
    void init(AssetDb& db, ThumbnailService& thumbs, AssetPickerHost host);

    // `kind` None offers meshes, materials, textures and prefabs. `id` must be unique among the fields drawn in
    // the current ID scope. Returns true when `value` changed this frame.
    bool field(const char* id, AssetId& value, AssetKind kind);

    // Paints the preview of `asset` (or a typed placeholder) into [a, b].
    void thumb(ImDrawList* dl, AssetId asset, ImVec2 a, ImVec2 b);

    // Call when the content of an asset was rewritten (its preview is stale).
    void touch(AssetId asset) { ++m_revision[asset]; }

    // The asset that a content browser drag (a path relative to the asset root) stands for, as `kind`.
    AssetId resolveDrop(const std::string& relPath, AssetKind kind) const;

private:
    struct Item {
        AssetId id;
        std::string name;
        std::string where; // source file (or authored file) it lives in
        AssetKind kind = AssetKind::None;
    };

    void gather(AssetKind kind);
    void drawPopup(AssetId& value, AssetKind kind, bool& changed);
    bool previewState(AssetId asset, ThumbRect& rect);
    void assign(AssetId& value, AssetId next, bool& changed);

    AssetDb* m_db = nullptr;
    ThumbnailService* m_thumbs = nullptr;
    AssetPickerHost m_host;
    std::unordered_map<AssetId, u32, AssetIdHash> m_revision;

    // Popup state: one popup is open at a time.
    std::vector<Item> m_items;
    char m_search[96] = {};
    int m_kindTab = 0; // when the field accepts several kinds
    bool m_focusSearch = false;
};

} // namespace foh
