#pragma once

#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "foh/ecs/access.h"
#include "foh/ecs/buffer_access.h"
#include "foh/ecs/commands.h"
#include "foh/ecs/events.h"
#include "foh/ecs/query.h"
#include "foh/ecs/world.h"

namespace foh {

// Read-only access to the resource T.
template <class T>
class Res {
public:
    explicit Res(const T* value) : m_value(value) {}
    const T& operator*() const { return *m_value; }
    const T* operator->() const { return m_value; }
    const T* get() const { return m_value; }

private:
    const T* m_value;
};

// State owned by ONE system: default constructed when the system is registered, kept between
// frames, destroyed with the system. Not part of the world (not saved, not visible to other
// systems), and any type works (no POD requirement), which makes it the place for caches and
// scratch memory that must not be reallocated every frame. Declares no access.
template <class T>
class Local {
public:
    explicit Local(T* value) : m_value(value) {}
    T& operator*() const { return *m_value; }
    T* operator->() const { return m_value; }
    T* get() const { return m_value; }

private:
    T* m_value;
};

// Exclusive access to the resource T: no other system touching T runs at the
// same time.
template <class T>
class ResMut {
public:
    explicit ResMut(T* value) : m_value(value) {}
    T& operator*() const { return *m_value; }
    T* operator->() const { return m_value; }
    T* get() const { return m_value; }

private:
    T* m_value;
};

// First parameter of an observer: what fired it. `value` points at the component
// (null for tags). It is valid for the duration of the call only.
//   onAdd:    the component was just added (the value is still default/zero if it
//             was added without a value; onSet fires once it is assigned)
//   onSet:    a value was assigned through set/insert (NOT by writing through a
//             Query reference: in-place writes are not observable)
//   onRemove: the component is about to go away; value is its last value
template <class T>
struct Trigger {
    Entity entity;
    const T* value;
};

// Handed to SystemParam<P>::init when a system is registered (main thread).
struct SystemInit {
    World& world;
    AccessSet& access;
};

// A system parameter type P describes, via a specialization of SystemParam<P>:
//   State   per-system storage (created once, destroyed with the system)
//   init    build State, declare the access set (registration time)
//   prepare resolve pointers for this frame; main thread, before systems run.
//           Returns the name of a missing dependency, or {} if all is well.
//   fetch   make the value passed to the system (worker thread)
//   finish  publish buffered side effects; main thread, in registration order
// To support a new parameter kind, add a specialization; nothing else changes.
template <class P>
struct SystemParam; // intentionally undefined: unsupported parameter type

template <class... Terms>
struct SystemParam<Query<Terms...>> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    using State = detail::QueryHandle;
    static State init(SystemInit& in) {
        std::array<detail::TermDesc, sizeof...(Terms)> terms{};
        detail::TermDesc* out = terms.data();
        (detail::addTerm<Terms>(in.world, out, &in.access), ...);
        return detail::createQuery(in.world, terms.data(), static_cast<u32>(out - terms.data()));
    }
    static std::string_view prepare(State&, World&) { return {}; }
    static Query<Terms...> fetch(State& s, const RunContext& ctx) { return Query<Terms...>(*s, ctx); }
    static void finish(State&, World&) {}
};

template <class T>
struct SystemParam<Local<T>> {
    static constexpr bool kDeferred = false;
    struct State {
        T value{};
    };
    static State init(SystemInit&) { return {}; }
    static std::string_view prepare(State&, World&) { return {}; }
    static Local<T> fetch(State& s, const RunContext&) { return Local<T>(&s.value); }
    static void finish(State&, World&) {}
};

template <class T>
struct SystemParam<Res<T>> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    struct State {
        const T* ptr = nullptr;
    };
    static State init(SystemInit& in) {
        in.access.read(in.world.component<T>());
        return {};
    }
    static std::string_view prepare(State& s, World& w) {
        s.ptr = static_cast<const World&>(w).resource<T>();
        return s.ptr ? std::string_view{} : detail::typeName<T>();
    }
    static Res<T> fetch(State& s, const RunContext&) { return Res<T>(s.ptr); }
    static void finish(State&, World&) {}
};

template <class T>
struct SystemParam<ResMut<T>> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    struct State {
        T* ptr = nullptr;
    };
    static State init(SystemInit& in) {
        in.access.write(in.world.component<T>());
        return {};
    }
    static std::string_view prepare(State& s, World& w) {
        s.ptr = w.resource<T>();
        return s.ptr ? std::string_view{} : detail::typeName<T>();
    }
    static ResMut<T> fetch(State& s, const RunContext&) { return ResMut<T>(s.ptr); }
    static void finish(State&, World&) {}
};

template <class T>
struct SystemParam<Buffers<T>> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    struct State {};
    static State init(SystemInit& in) {
        in.access.read(in.world.component<detail::BufferAccessTag<T>>());
        return {};
    }
    static std::string_view prepare(State&, World&) { return {}; }
    static Buffers<T> fetch(State&, const RunContext& ctx) {
        return Buffers<T>(static_cast<const World&>(*ctx.world).buffers());
    }
    static void finish(State&, World&) {}
};

template <class T>
struct SystemParam<BuffersMut<T>> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    struct State {};
    static State init(SystemInit& in) {
        in.access.write(in.world.component<detail::BufferAccessTag<T>>());
        return {};
    }
    static std::string_view prepare(State&, World&) { return {}; }
    static BuffersMut<T> fetch(State&, const RunContext& ctx) { return BuffersMut<T>(ctx.world->buffers()); }
    static void finish(State&, World&) {}
};

template <>
struct SystemParam<Commands> {
    static constexpr bool kDeferred = true;  // true: finish() applies side effects other systems may need
    using State = CommandQueue;
    static State init(SystemInit& in) { return CommandQueue(in.world); }
    static std::string_view prepare(State& s, World&) {
        s.clear();
        s.prepare();
        return {};
    }
    static Commands fetch(State& s, const RunContext&) { return Commands(s); }
    static void finish(State& s, World&) { s.apply(); }
};

template <class T>
struct SystemParam<EventWriter<T>> {
    static constexpr bool kDeferred = true;  // true: finish() applies side effects other systems may need
    struct State {
        EventChannel* channel;
        std::vector<T> buffer;
    };
    static State init(SystemInit& in) { return {&eventChannelFor<T>(in.world), {}}; }
    static std::string_view prepare(State& s, World&) {
        s.buffer.clear();
        return {};
    }
    static EventWriter<T> fetch(State& s, const RunContext&) { return EventWriter<T>(s.buffer); }
    static void finish(State& s, World&) {
        if (!s.buffer.empty()) s.channel->append(s.buffer.data(), static_cast<u32>(s.buffer.size()));
        s.buffer.clear();
    }
};

template <class T>
struct SystemParam<EventReader<T>> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    struct State {
        const EventChannel* channel;
        u64 cursor = 0;
    };
    static State init(SystemInit& in) { return {&eventChannelFor<T>(in.world), 0}; }
    static std::string_view prepare(State&, World&) { return {}; }
    static EventReader<T> fetch(State& s, const RunContext&) { return EventReader<T>(*s.channel, s.cursor); }
    static void finish(State&, World&) {}
};

template <class T>
struct SystemParam<Trigger<T>> {
    static constexpr bool kDeferred = false;
    struct State {};
    static State init(SystemInit&) { return {}; }
    static std::string_view prepare(State&, World&) { return {}; }
    static Trigger<T> fetch(State&, const RunContext& ctx) {
        return {ctx.triggerEntity, static_cast<const T*>(ctx.triggerValue)};
    }
    static void finish(State&, World&) {}
};

// Exclusive system: gets the whole World and runs alone on the main thread,
// outside the parallel phase. Use it for rare, structural, whole-world work.
template <>
struct SystemParam<World&> {
    static constexpr bool kDeferred = false; // true: finish() applies side effects other systems may need
    struct State {};
    static State init(SystemInit& in) {
        in.access.exclusive = true;
        return {};
    }
    static std::string_view prepare(State&, World&) { return {}; }
    static World& fetch(State&, const RunContext& ctx) { return *ctx.world; }
    static void finish(State&, World&) {}
};

// ---- type erasure: what the scheduler stores per system ---------------------

// Function pointers into the module that registered the system. The scheduler
// removes (and destroys) every system of a module before that module unloads.
struct SystemVTable {
    void (*run)(void* state, const RunContext& ctx);
    std::string_view (*prepare)(void* state, World& world);
    void (*finish)(void* state, World& world);
    void (*destroy)(void* state);
    bool deferred;  // some parameter has side effects that must be applied before dependents run
};

namespace detail {

template <class... Ps>
struct SystemModel {
    using Fn = void (*)(Ps...);
    using Indices = std::index_sequence_for<Ps...>;

    SystemModel(Fn f, SystemInit& in) : fn(f), states(SystemParam<Ps>::init(in)...) {}

    static void run(void* self, const RunContext& ctx) {
        auto& m = *static_cast<SystemModel*>(self);
        [&]<size_t... I>(std::index_sequence<I...>) {
            m.fn(SystemParam<Ps>::fetch(std::get<I>(m.states), ctx)...);
        }(Indices{});
    }
    static std::string_view prepare(void* self, World& world) {
        auto& m = *static_cast<SystemModel*>(self);
        std::string_view missing;
        // Every parameter prepares even after a failure, so queues/buffers are reset.
        [&]<size_t... I>(std::index_sequence<I...>) {
            ((missing = pick(missing, SystemParam<Ps>::prepare(std::get<I>(m.states), world))), ...);
        }(Indices{});
        return missing;
    }
    static void finish(void* self, World& world) {
        auto& m = *static_cast<SystemModel*>(self);
        [&]<size_t... I>(std::index_sequence<I...>) {
            (SystemParam<Ps>::finish(std::get<I>(m.states), world), ...);
        }(Indices{});
    }
    static void destroy(void* self) { delete static_cast<SystemModel*>(self); }

    static SystemVTable vtable() { return {&run, &prepare, &finish, &destroy, (SystemParam<Ps>::kDeferred || ...)}; }

    Fn fn;
    std::tuple<typename SystemParam<Ps>::State...> states;

private:
    static std::string_view pick(std::string_view first, std::string_view next) {
        return first.empty() ? next : first;
    }
};

} // namespace detail

} // namespace foh
