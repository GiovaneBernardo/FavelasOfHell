#pragma once

#include <cstddef>

#include "foh/types.h"

namespace foh {

// Opaque handle. Layout matches the storage backend's 64-bit id (32-bit index,
// 16-bit generation) but nothing outside the ecs module relies on that.
struct Entity {
    u64 id = 0;

    constexpr u32 index() const { return static_cast<u32>(id); }
    constexpr u32 generation() const { return static_cast<u32>(id >> 32) & 0xFFFFu; }
    constexpr bool isNull() const { return id == 0; }
    constexpr explicit operator bool() const { return id != 0; }
    friend constexpr bool operator==(Entity, Entity) = default;
};
static_assert(sizeof(Entity) == sizeof(u64), "queries reinterpret backend id arrays as Entity arrays");

inline constexpr Entity kNullEntity{};

// Identifies a component or resource type inside ONE World. Never persist it or
// share it across worlds: resolve by name (see ComponentDesc) instead.
using ComponentId = u64;

// Who registered something. The host removes everything owned by kGameOwner
// before it unloads the game DLL.
using OwnerId = u32;
inline constexpr OwnerId kEngineOwner = 0;
inline constexpr OwnerId kGameOwner = 1;

// Fixed frame schedule. Startup systems run once, on the first update after
// they were added, and are then removed.
enum class Stage : u8 { Startup, PreUpdate, Update, PostUpdate, Count };

// Frame timing, stored in the World as a resource (Res<Time>).
struct Time {
    f64 elapsed = 0.0;
    f32 delta = 0.0f;
    u64 frame = 0;
};

} // namespace foh
