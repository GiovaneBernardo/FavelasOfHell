# 0015: Game assets, the Asset Forge, and vehicles

- Status: **Proposed** (project lead to review); implemented in the engine, the editor and the game module.
- Related: [ADR 0004](0004-model-and-texture-import.md) (models, prefab variants), [ADR 0009](0009-skeletal-animation.md) (skeletons, Mixamo clips), [ADR 0011](0011-player-camera-input.md) (player, `PrefabSlot`), README "Physics"

## Problem

A model from Sketchfab is meshes and materials. A car needs wheels, suspension, an engine; a character needs a skeleton the animation library understands; a prop needs a collision shape. That knowledge belongs to a human, differs per model, and must survive reimports. Until now there was no place to put it, and no vehicles.

## Decisions

### Game asset definitions are small text files next to the model

`engine/assets` (`foh/game_asset.h`): `.fvehicle`, `.fprop`, `.fchar` hold `key value` lines and `[wheel]` sections, next to the model (`Car.gltf` -> `Car.fvehicle`; the prefab editor's `Car.fvariant` already follows that convention). Text so they diff and merge like the `.meta` files, and so game code can read them without the asset database. Unknown keys are ignored and missing ones keep defaults, so files survive added fields in both directions. One visitor per type serves reading and writing, so the two cannot disagree. They name the model by source path (not GUID), like `PrefabRequest`.

### Game code asks for them by path

`GameAssetRequest { path }` (`foh/scene/game_asset_request.h`), resolved by the scene plugin like `PrefabRequest`: it sets the entity's `PrefabSlot` to the model, adds the definition as a plain component (`VehicleSetup`, `PropSetup`, `CharacterSetup`; trivially copyable and reflected, so saved with the world and safe across module reloads) and, for a vehicle, finds the wheel mesh entities in the instantiated model and records their rest transforms (`WheelNode`). The request waits until the model is imported. A wheel may name up to three meshes (tire + rim) joined with `|`. `SlotModelPart` now records its `owner`, so "hide the player's own body" no longer hides every model made through a slot (a driven car's meshes included).

### The Asset Forge (editor)

One window, three tabs, all starting from the same step, the model fix (rotate in 90 degree steps, scale to a real length/height, centre and put the origin on the ground), saved as the same `.fvariant` the prefab editor writes. It shows the model in the Prefab Preview view and draws its handles over it (ImGui draw list + ImGuizmo to move them).

- **Vehicle**: *Find the wheels* groups meshes named like wheels/tires/rims whose shape is a disc, turns the model when the axles point along Z, puts the origin between the wheels on the ground, then proposes mass, collider, centre of mass, brakes, seat and camera from the dimensions. Presets (hatchback .. van) fill engine, gearbox, springs, grip and steering. Every number the simulation reads is editable. "Save and test drive" spawns it in the game.
- **Character**: a skinned model is checked against the 25 humanoid joints the animation library speaks (Mixamo names). An unskinned one is rigged automatically (below).
- **Prop**: box / sphere / capsule fitted to the bounds, body type, mass, surface, breakable.

The Game Assets window lists every definition with Spawn / Edit and the choice of the B-key vehicle. The content browser shows them with their own icons and "Make game asset" on models.

### Auto-rig (`engine/assets/humanoid_rig.*`)

The artist places 25 joints (proposed from the bounds by scaling the Mixamo template). The rigger lifts hanging arms to a T-pose (the clips are authored against one), weights every vertex to the bones by distance to the bone segments scaled by a per-bone thickness, diffuses the weights along the mesh edges (vertices at one position are one point, so UV seams never split a weight), keeps the strongest four, and builds the skeleton from the landmarks with **Mixamo's own rest rotations** (copied from Y Bot). It writes a glTF binary that goes through the normal importer, so the result is an ordinary skinned prefab and every imported Mixamo clip animates it without retargeting; the model's real materials are put back with a prefab variant. Fingers are not rigged (the hand moves as one piece). Runs on a low-priority job.

### Vehicle dynamics (`game/src/vehicle_dynamics.*`, hot reloadable)

A rigid body on spring-damper wheels, pure math over plain data (unit tested). Per sub-step (<= 10 ms): wheels sample the ground below the hub (terrain height or the top of a solid box); the spring has the static weight as preload, so the vehicle sits at the ride height that was modelled; tires turn the contact velocity into a friction force (stiff while gripping, saturating on a friction ellipse: holds on slopes, slides when overdriven, drifts with the handbrake); an engine with torque curve and rev limiter drives the wheels through an automatic gearbox that shifts by ground speed; brakes and the handbrake act on wheel spin; traction control cuts the torque of a spinning wheel; Ackermann steering reduced with speed; drag and downforce. The body (circles along the collider box) is pushed out of buildings, tree trunks, `StaticBox` entities and cliffs with an impulse at the height of the centre of mass, so a crash shoves and turns the car instead of tipping it onto its nose.

### Driving (`game/src/vehicle.*`)

F gets in the nearest vehicle / out (the player is hidden while driving: there is no seated pose yet), W S accelerate / brake / reverse, A D steer, Space handbrake, R rights an overturned car, mouse looks around a chase camera that swings back behind the car. B spawns the vehicle selected in the editor (the placeholder sedan by default). Props get their collider here too (an invisible `StaticBox` child), because colliders are game components.

## Why not Jolt vehicles (yet)

README "Physics" says Jolt, and its wheeled vehicle is good. The terrain is an infinite streamed heightfield and the buildings are footprints queried through `WorldSurface`; neither is in a Jolt world, and the player already collides against exactly those through hand-written queries. A raycast-style vehicle against the same queries drives on all of it today, lives in the hot-reloadable module (tune handling while the game runs) and needs no physics world synchronisation. This does not replace Jolt for rigid bodies, ragdolls or dynamic props; when the physics module gains terrain and building shapes the solver's contact queries move behind it and the tire/engine model can stay. This is a deviation from "vehicles through Jolt" in the README and needs the lead's decision.

## Not done

- No seated driver pose or door animation (the driver is hidden); no AI drivers or traffic; no damage, lights, horn, audio or exhaust smoke (`slip` and `rpm` are exposed per wheel for them); no trailers, bikes (2 wheels) or tracked vehicles; no manual gearbox.
- Dynamic props are static colliders until rigid bodies exist; sphere/capsule colliders are approximated by boxes for the player and vehicles.
- Auto-rig: humanoids only, T/A-pose, no fingers, no facial rig, no automatic landmark detection beyond the bounds (the joints are placed by hand), weights are distance-based (no heat diffusion), no pose retargeting for other skeletons (a skinned model with non-Mixamo bone names is reported, not converted).
- The Forge finds wheels by name and shape; a model whose wheels are merged into the body mesh needs "Wheels from size" and no wheel mesh spins.
- Vulkan, Linux: nothing platform-specific was added.
