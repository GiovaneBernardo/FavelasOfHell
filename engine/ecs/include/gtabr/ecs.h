#pragma once

// Bevy-inspired ECS. THIS IS A PLACEHOLDER: only entities, plugins, ordered
// systems and owner-tagged system removal exist so far. Components/archetypes,
// queries, resources, events and commands come next (see README, "ECS").
//
// Hot reload contract: plain function pointers for systems (no std::function),
// and every system is tagged with the module that registered it so the host
// can remove exactly that module's systems before unloading its DLL.

#include <string>
#include <vector>

#include "gtabr/platform.h"
#include "gtabr/types.h"

namespace gtabr {

struct Entity {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool operator==(const Entity&) const = default;
};

class GTABR_API World {
public:
    Entity spawn();
    bool despawn(Entity entity);
    bool isAlive(Entity entity) const;
    size_t entityCount() const { return m_aliveCount; }

private:
    std::vector<u32> m_generations;
    std::vector<u32> m_freeList;
    size_t m_aliveCount = 0;
};

struct Time {
    f64 elapsed = 0.0;
    f32 delta = 0.0f;
    u64 frame = 0;
};

struct SystemContext {
    World& world;
    const Time& time;
};

using SystemFn = void (*)(SystemContext&);
using OwnerId = u32;
inline constexpr OwnerId kEngineOwner = 0;
inline constexpr OwnerId kGameOwner = 1;

enum class Stage : u8 { PreUpdate, Update, PostUpdate, Count };

class App;

// A plugin registers systems (and later components/resources/events) on build().
// Plugin objects are NOT retained by the App: a plugin from a hot-reloadable
// module must not outlive its module.
class Plugin {
public:
    virtual ~Plugin() = default;
    virtual void build(App& app) = 0;
};

class GTABR_API App {
public:
    World& world() { return m_world; }
    const Time& time() const { return m_time; }

    void addPlugin(Plugin& plugin) { plugin.build(*this); }
    void addSystem(Stage stage, SystemFn fn, const char* name);

    // Systems registered while an owner is active are tagged with it.
    void setActiveOwner(OwnerId owner) { m_activeOwner = owner; }
    void removeSystemsOwnedBy(OwnerId owner);

    void update(f32 deltaSeconds);
    size_t systemCount() const;

private:
    struct SystemEntry {
        SystemFn fn;
        std::string name;
        OwnerId owner;
    };
    World m_world;
    Time m_time;
    OwnerId m_activeOwner = kEngineOwner;
    std::vector<SystemEntry> m_systems[static_cast<size_t>(Stage::Count)];
};

} // namespace gtabr
