#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <memory>

#include "foh/ecs/system.h"
#include "foh/ecs/world.h"

namespace foh {

class App;
class JobSystem;

// A plugin registers components, resources, events and systems in build().
// Plugin objects are NOT retained by the App: a plugin from a hot-reloadable
// module must not outlive its module.
class Plugin {
public:
    virtual ~Plugin() = default;
    virtual void build(App& app) = 0;
};

struct AppConfig {
    static constexpr u32 kAutoWorkers = 0xFFFFFFFFu;
    // Worker threads of the pool the App creates (0 = everything runs on the
    // calling thread, which also makes runs fully sequential for debugging).
    u32 workerThreads = kAutoWorkers;
    // Use an existing pool instead (not owned; must outlive the App).
    JobSystem* jobs = nullptr;
    // Safety valve: an observer that keeps re-triggering itself would never return.
    // After this many calls in one frame (counted from update() to update()) further
    // calls are skipped and an error is logged.
    u32 maxObserverCalls = 1u << 20;
};

// Fluent handle returned by App::addSystem for optional ordering. Valid until
// the next addSystem/removeSystemsOwnedBy call.
//
// Without constraints, systems that conflict (see AccessSet) run in registration
// order and non-conflicting ones may run concurrently. after()/before() add
// explicit edges within the same stage and refer to the other system by its
// function; a reference to a system that is not registered is ignored with a
// warning (e.g. its module is not loaded).
class FOH_API SystemRef {
public:
    SystemRef(App& app, u64 id) : m_app(&app), m_id(id) {}

    SystemRef& name(const char* name);
    template <class... Ps>
    SystemRef& after(void (*other)(Ps...)) { return order(reinterpret_cast<std::uintptr_t>(other), true); }
    template <class... Ps>
    SystemRef& before(void (*other)(Ps...)) { return order(reinterpret_cast<std::uintptr_t>(other), false); }

    // System sets: a set is any empty struct used as a label, e.g. `struct Physics {};`.
    // inSet joins the set; afterSet/beforeSet order against every member of the set
    // in this stage, so ordering holds however many systems join it, from any module.
    template <class Set>
    SystemRef& inSet() { return link(detail::fnv1a(detail::typeName<Set>()), Link::Member); }
    template <class Set>
    SystemRef& afterSet() { return link(detail::fnv1a(detail::typeName<Set>()), Link::After); }
    template <class Set>
    SystemRef& beforeSet() { return link(detail::fnv1a(detail::typeName<Set>()), Link::Before); }

private:
    enum class Link { Member, After, Before };
    SystemRef& order(std::uintptr_t otherKey, bool otherRunsFirst);
    SystemRef& link(u64 setKey, Link kind);
    App* m_app;
    u64 m_id;
};

// Owns the World and the schedule, and runs a frame.
//
// Frame: for each stage (Startup once, then PreUpdate, Update, PostUpdate) the
// systems are split into segments at exclusive (`World&`) systems. Inside a
// segment, systems run as a dependency graph on the job system: a system starts
// when everything it conflicts with (or was explicitly ordered after) is done.
// Structural changes and events are buffered per system and applied on the main
// thread, in registration order, when the segment ends, so results do not depend
// on thread timing.
//
// Thread-safety: all methods must be called from the main thread, outside
// update(), except where noted.
class FOH_API App {
public:
    explicit App(AppConfig config = {});
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    World& world() { return m_world; }
    const World& world() const { return m_world; }
    const Time& time() const { return m_time; }
    JobSystem& jobs();

    void addPlugin(Plugin& plugin) { plugin.build(*this); }

    // Registration helpers for plugin build(). Components must be registered
    // before the first update; they are cheap to register twice.
    template <class T>
    App& registerComponent() {
        m_world.component<T>();
        return *this;
    }
    template <class T>
    App& registerEvent() {
        eventChannelFor<T>(m_world);
        return *this;
    }
    template <class T>
    App& insertResource(const T& value) {
        m_world.insertResource(value);
        return *this;
    }

    // `fn` is a plain function whose parameters are system parameters (Query,
    // Res, ResMut, Commands, EventReader, EventWriter) or a single `World&`.
    template <class... Ps>
    SystemRef addSystem(Stage stage, void (*fn)(Ps...)) {
        AccessSet access;
        SystemInit init{m_world, access};
        auto* model = new detail::SystemModel<Ps...>(fn, init);
        return registerSystem(stage, model, detail::SystemModel<Ps...>::vtable(), std::move(access),
                              reinterpret_cast<std::uintptr_t>(fn));
    }

    // Observers run immediately, on the main thread, when a component is added to,
    // set on, or removed from an entity (including when the entity is despawned).
    // The function takes a Trigger<T> first, then any of Query, Res, ResMut, Commands
    // and EventWriter/Reader (not World&). Changes recorded through Commands are
    // applied right after the observer returns. Like systems, observers are
    // owner-tagged and removed with removeSystemsOwnedBy().
    //
    // They fire for changes made through World, Commands and exclusive systems. They do
    // NOT fire for in-place writes through a Query reference, which the engine
    // cannot see; use onSet with Commands/World::insert when others must react.
    template <class T, class... Ps>
    App& onAdd(void (*fn)(Trigger<T>, Ps...)) { return addObserver(ObserverEvent::Add, fn); }
    template <class T, class... Ps>
    App& onSet(void (*fn)(Trigger<T>, Ps...)) { return addObserver(ObserverEvent::Set, fn); }
    template <class T, class... Ps>
    App& onRemove(void (*fn)(Trigger<T>, Ps...)) { return addObserver(ObserverEvent::Remove, fn); }

    // Systems registered while an owner is active are tagged with it.
    void setActiveOwner(OwnerId owner) { m_activeOwner = owner; }
    void removeSystemsOwnedBy(OwnerId owner);

    void update(f32 deltaSeconds);
    size_t systemCount() const;
    // Wall time of every system's last run, in registration order per stage (a diagnostic: the editor's --profile).
    struct SystemTiming {
        std::string name;
        double ms;
    };
    std::vector<SystemTiming> systemTimings() const;

    struct Impl; // opaque: the scheduler's state (src/app.cpp)

private:
    friend class SystemRef;

    enum class ObserverEvent : u8 { Add, Set, Remove };

    template <class T, class... Ps>
    App& addObserver(ObserverEvent event, void (*fn)(Trigger<T>, Ps...)) {
        static_assert(!(std::is_same_v<Ps, World&> || ...), "an observer cannot take World&: record changes with Commands");
        AccessSet access; // not used for scheduling: observers run when the change happens
        SystemInit init{m_world, access};
        auto* model = new detail::SystemModel<Trigger<T>, Ps...>(fn, init);
        registerObserver(event, m_world.component<T>(), std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T)), model,
                         detail::SystemModel<Trigger<T>, Ps...>::vtable(), reinterpret_cast<std::uintptr_t>(fn));
        return *this;
    }
    void registerObserver(ObserverEvent event, ComponentId component, u32 componentSize, void* state,
                          const SystemVTable& vtable, std::uintptr_t key);

    SystemRef registerSystem(Stage stage, void* state, const SystemVTable& vtable, AccessSet&& access,
                             std::uintptr_t key);
    void setSystemName(u64 id, const char* name);
    void addOrdering(u64 id, std::uintptr_t otherKey, bool otherRunsFirst);
    void addSetLink(u64 id, u64 setKey, int kind);

    // Declaration order is destruction order in reverse: the schedule (systems
    // and their queries) must die before the World, and the pool last-but-one.
    World m_world;
    Time m_time;
    OwnerId m_activeOwner = kEngineOwner;
    std::unique_ptr<JobSystem> m_ownedJobs;
    JobSystem* m_jobs = nullptr;
    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
