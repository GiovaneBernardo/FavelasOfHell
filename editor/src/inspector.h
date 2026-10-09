#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "foh/ecs.h"
#include "foh/ecs/inspect.h"
#include "foh/math.h"

namespace foh {

struct SceneRef {
    const char* name;
    World* world;
};

// What the editor has selected: an entity inside one of the scenes.
struct Selection {
    size_t scene = 0;
    Entity entity = kNullEntity;
    bool valid(std::span<const SceneRef> scenes) const {
        return scene < scenes.size() && !entity.isNull() && scenes[scene].world->isAlive(entity);
    }
};

// Two windows over the type-erased ECS inspection API (foh/ecs/inspect.h):
//   Entities   list of one scene's entities, filtered like a Query: components an
//              entity must have, components it must not have, plus a text search
//   Inspector  every component of the selected entity, edited in place through its
//              reflected schema
//
// Editing writes straight into component storage, which is safe because the UI runs
// on the main thread outside App::update. Components without FOH_REFLECT are listed
// but cannot be edited field by field.
//
// Thread-safety: main thread only.
class AssetPicker;

class EntityInspector {
public:
    // Reference fields (AssetId) get a picker button instead of two raw numbers. Optional.
    void setAssetPicker(AssetPicker* picker) { m_picker = picker; }

    void drawEntities(std::span<const SceneRef> scenes, Selection& selection, bool* open);
    void drawInspector(std::span<const SceneRef> scenes, Selection& selection, bool* open);

private:
    void refresh(const SceneRef& scene);
    bool drawFilterChips(std::vector<std::string>& names, bool with);
    void drawComponent(World& world, Entity entity, ComponentId id, const Schema& schema, std::span<const SceneRef> scenes,
                       Selection& selection);

    // Entities window state.
    size_t m_scene = 0;
    std::vector<std::string> m_with, m_without;
    char m_search[64] = {};
    std::vector<ComponentInfoView> m_components; // of the listed scene's world
    std::vector<Entity> m_entities;              // after all filters
    // What the list shows: a flattened tree (only the expanded branches) while no filter is active, else the
    // matches as a flat list. Flat rows keep the list clipper usable with 100k entities.
    struct Row {
        Entity entity;
        u32 depth = 0;
        u32 children = 0;
    };
    std::vector<Row> m_rows;
    std::unordered_set<u64> m_expanded;
    Entity m_lastSelected{};
    Entity m_revealTarget{}; // selected from elsewhere (viewport pick): open its ancestors and scroll to it
    AssetPicker* m_picker = nullptr;
    bool m_scrollToReveal = false;
    bool m_pickedHere = false; // this window set the selection this frame: no need to reveal it
    void buildTreeRows(const World& world);
    size_t m_matchedByComponents = 0;
    size_t m_total = 0;
    double m_sinceRefresh = 1e9;
    bool m_dirty = true;

    // Quaternion fields are edited as Euler angles; keep the angles being dragged so
    // they do not flip representation every frame.
    unsigned m_eulerId = 0;
    Vec3 m_euler{0.0f};
    std::vector<ComponentId> m_scratchIds;
    std::vector<std::byte> m_editBefore; // component bytes before the widgets ran (edit detection)
};

} // namespace foh
