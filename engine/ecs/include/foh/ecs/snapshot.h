#pragma once

#include <span>
#include <vector>

#include "foh/ecs/world.h"

namespace foh {

struct SnapshotOptions {
    // Also save/restore resources (including engine ones such as Time).
    bool includeResources = true;
};

struct SnapshotReport {
    bool ok = false;           // false: the data was malformed; the world was NOT modified
    u32 entities = 0;
    u32 components = 0;        // component instances restored
    u32 resources = 0;
    u32 migratedComponents = 0; // restored through a layout conversion
    u32 skippedComponents = 0;  // saved with a component this build does not know (module not loaded?) or cannot convert
};

// Serializes every spawned entity (components, parent, relationships) and the
// resources into a self-describing binary blob. The blob carries the schema of
// each component it contains, so it can be loaded after the types changed: fields
// are matched by name (or former name), numbers are converted, new fields get
// zero. Components saved without FOH_REFLECT are stored as opaque bytes and only
// load back if their size is unchanged.
//
// Main thread only, outside the parallel phase. Buffers are saved by value.
FOH_API std::vector<std::byte> saveSnapshot(const World& world, const SnapshotOptions& options = {});

// Replaces the world's spawned entities with the snapshot's. Entity ids are NOT
// preserved (fresh ids are created); every Entity field and parent link is remapped
// to the new ids. Malformed or truncated data is rejected before anything is
// changed (the report says ok == false). Components must be registered (by loading
// their module) before the call; unknown ones are skipped and counted.
FOH_API SnapshotReport loadSnapshot(World& world, std::span<const std::byte> data,
                                    const SnapshotOptions& options = {});

} // namespace foh
