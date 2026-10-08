#pragma once

// Type-erased read access to what a World holds, for tools (the editor inspector).
// Gameplay code uses Query<...> instead.
//
// All functions: main thread, outside the parallel phase (i.e. not from a system).

#include <span>
#include <vector>

#include "foh/ecs/schema.h"
#include "foh/ecs/world.h"

namespace foh {

struct ComponentInfoView {
    ComponentId id;
    const Schema* schema; // never null; schema->reflected tells if fields can be shown
};

// Every component type registered in the world, sorted by name.
FOH_API void listComponents(const World& world, std::vector<ComponentInfoView>& out);

// Spawned entities that have ALL of `with` and NONE of `without`, in storage order.
// This is the type-erased twin of Query<With<A>, With<B>, Without<C>>.
FOH_API void findEntities(const World& world, std::span<const ComponentId> with,
                          std::span<const ComponentId> without, std::vector<Entity>& out);

// The registered components `entity` currently has, sorted by name.
FOH_API void componentsOf(const World& world, Entity entity, std::vector<ComponentId>& out);

} // namespace foh
