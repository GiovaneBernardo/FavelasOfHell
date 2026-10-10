# 0011: Player, camera and input

- Status: **Proposed** (project lead to review); implemented in the editor and the runtime.
- Related: [ADR 0009](0009-skeletal-animation.md) (the character is animated by the locomotion system), [ADR 0002](0002-windowing-and-input.md)

## Decisions

- **Input** reaches game code as the resource `GameInput { InputState state; bool captured; }` (`foh/input.h`), refreshed by the host before every world update. `captured` is false (and the state empty) when the editor UI has the focus, so typing in a text field never moves the player. The editor captures the mouse when the Game viewport is clicked and releases it on Escape; the runtime is always captured (Escape quits). Game code never touches the window.
- **Models are requested by source path.** A game module cannot reach the asset database and asset GUIDs differ per machine (imports are not committed), so a game entity carries `PrefabRequest { sourcePath }` (`foh/scene/prefab_request.h`). Every frame the scene plugin instantiates the prefab INTO that entity once the source is imported (nodes become children, `Animator`/`SkinPalette`/`Locomotion*` are added, locomotion clips are assigned from the imported clips by their Mixamo names) and removes the request. The game starts and plays (invisibly) before the import finishes.
- **The player** (`game/src/player.*`): WASD relative to the camera, Left Shift walks (default is the 4.2 m/s run), Space jumps (jump clip + takeoff after `jumpTakeoffDelay`, gravity), C crouches (hold; `Player::crouchToggle`), V switches camera. The engine's locomotion system moves the entity horizontally; the game then resolves collision against `StaticBox` entities (circle against AABBs, steps up to 0.5 m, standing on ledges) and owns the vertical axis. This is a stand-in until the physics module exists.
- **Camera**: third person is GTA 5 style, orbiting a shoulder pivot (55 cm to the right, 1.55 m up, 3.3 m back) that trails the player a little so running does not jitter; first person sits at eye height and hides the player's body with the new `Hidden` component (the renderer skips hidden entities and characters whose root is hidden). The body faces the movement direction in third person and the camera heading in first person (strafe clips).
- **Crouch animation**: `LocomotionClips` gained `crouchIdleClip` and `crouchWalk{,Back,Left,Right}Clip` and `LocomotionControl::crouch`; the stance is a second blend space, entered with an inertialized transition. Without crouch clips the request is ignored by the animation (the game still lowers the camera and slows the player).

## Not done

- No camera collision (third person can clip into walls), no aim/sprint modes, no ledge grabs or vaulting, no sound.
- Collision is AABB only (rotated boxes use their bounds).
- Download Mixamo "Crouch Idle" and "Crouched Walking" (plus the sneaking left/right ones) next to the other clips so the crouch animates; no jump airborne-phase clips either, the single jump clip is played from the takeoff.
