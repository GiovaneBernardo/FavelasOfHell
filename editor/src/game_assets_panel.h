#pragma once

// The Game Assets window: every vehicle, prop and character definition in the project (the files the Asset Forge makes),
// with one-click spawn into the game and the choice of the vehicle the B key spawns.
//
// Thread-safety: main thread only.

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "foh/game_asset.h"

namespace foh {

struct GameAssetsHost {
    std::filesystem::path assetRoot;
    std::function<void(const std::string& relPath)> spawn;            // in front of the Game camera
    std::function<void(const std::string& relPath)> edit;             // open in the Asset Forge
    std::function<void(const std::string& relPath)> selectForSpawnKey; // the B key
};

class GameAssetsPanel {
public:
    void init(GameAssetsHost host) { m_host = std::move(host); }
    void refresh() { m_dirty = true; }
    void draw(bool* open);
    void setSpawnKeyVehicle(const std::string& relPath) { m_spawnKey = relPath; }

private:
    struct Item {
        std::string rel;
        std::string name;
        GameAssetKind kind = GameAssetKind::None;
    };
    void scan();

    GameAssetsHost m_host;
    std::vector<Item> m_items;
    bool m_dirty = true;
    std::chrono::steady_clock::time_point m_lastScan{};
    std::string m_spawnKey;
};

} // namespace foh
