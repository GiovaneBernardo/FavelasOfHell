#pragma once

// Bevy-style ECS on top of flecs (see docs/adr/0001-ecs-on-flecs.md).
//
//   flecs      storage, archetypes, queries, (later) relationships/observers/meta
//   this layer typed Query/Res/Commands/Event parameters, plugins, access-set
//              based parallel scheduler, hot-reload ownership
//
// flecs headers never appear in public headers: everything here is plain C++20
// over a few exported engine functions, so game modules compile fast and never
// depend on the backend's ABI.
//
// Hot reload contract (see AGENTS.md):
//   * systems are plain function pointers, tagged with the registering module
//     (App::setActiveOwner); the host removes them before unloading the DLL
//   * components/resources/events are POD-like and identified by type name, so
//     the data in the World survives a reload and the new DLL re-binds to it
//   * all cross-system effects are buffered per system (Commands, EventWriter)

#include "gtabr/ecs/access.h"
#include "gtabr/ecs/app.h"
#include "gtabr/ecs/commands.h"
#include "gtabr/ecs/events.h"
#include "gtabr/ecs/query.h"
#include "gtabr/ecs/system.h"
#include "gtabr/ecs/types.h"
#include "gtabr/ecs/world.h"
