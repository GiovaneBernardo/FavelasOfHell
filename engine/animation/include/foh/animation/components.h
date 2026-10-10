#pragma once

// Animation components (ADR 0009). Plain data, reflected, no pointers into modules: they survive a game module
// reload and are saved with the world. Bulk per-character state lives in engine buffers (BufferHandle).
//
// What a character entity looks like after instantiating an imported skinned prefab:
//   root:   Transform, Animator, SkinPalette, LocomotionClips, LocomotionControl, LocomotionState
//   child:  Transform, MeshRenderer, SkinnedMesh      (one per skinned mesh)
// Gameplay writes LocomotionControl (what the character should do) and never touches clips or poses.

#include "foh/animation/inertialization.h"
#include "foh/asset_id.h"
#include "foh/ecs/buffer.h"
#include "foh/ecs/reflect.h"
#include "foh/math.h"
#include "foh/types.h"

namespace foh {

// Binds a character to a skeleton and owns the history needed for smooth transitions.
struct Animator {
    AssetId skeleton;
    BufferHandle<JointHistory> history; // one entry per joint; engine-owned, rebuilt when the layout does not match
    f32 playbackSpeed = 1.0f;
    f32 transitionHalfLife = 0.1f;     // seconds; the offset of an inertialized transition halves this often
    f32 inertialElapsed = 1.0e9f;      // seconds since the last transition (huge: none running)
    u32 jointCount = 0;                // joints `history` was built for
    bool historyValid = false;         // `history` holds a previous frame
    bool enabled = true;
};

// The clips of a character's locomotion. Which role a clip plays is measured from the clip itself (its root motion
// gives the direction and speed it was authored for), so the slots only need to be filled, not labelled precisely:
// any move clip may sit in any "move" slot. Unfilled slots are skipped.
struct LocomotionClips {
    AssetId idleClip;
    // Idle breaks: while the character stands still, every so often one of these plays (shifting weight, looking around)
    // and the idle resumes. Any number of the four slots may be filled.
    AssetId idleBreak0Clip, idleBreak1Clip, idleBreak2Clip, idleBreak3Clip;
    AssetId walkClip, runClip, sprintClip;
    AssetId walkBackClip, runBackClip;
    AssetId walkLeftClip, walkRightClip, runLeftClip, runRightClip;
    AssetId turnLeftClip, turnRightClip, turnLeft90Clip, turnRight90Clip;
    AssetId jumpClip;
    // Crouched stance (LocomotionControl::crouch): its own idle and move clips, blended like the standing ones. A
    // character without a crouch idle or crouch moves ignores the request (gameplay should still slow it down).
    AssetId crouchIdleClip;
    AssetId crouchWalkClip, crouchWalkBackClip, crouchWalkLeftClip, crouchWalkRightClip;
};

// What gameplay wants. Written by game code, AI or the editor, read by the locomotion system.
struct LocomotionControl {
    Vec3 velocity{0.0f}; // desired velocity in world space, meters per second (only X and Z are used)
    Vec3 facing{0.0f};   // direction to face in world space; zero: face the direction of movement
    bool jump = false;   // request the jump clip; cleared by the system when it starts
    bool crouch = false; // crouched stance: the crouch clips when they exist, else a procedural crouch (hips down, legs IK)
    // Ground contact as gameplay sees it. false while the character is in the air (a jump, or a fall off a ledge): the
    // animation plays the air pose and, when this turns true again, the landing. Defaults to true (no gameplay physics).
    bool grounded = true;
    f32 expectedAirTime = 0.7f; // seconds the current jump is expected to last: the air pose is stretched over it
    // The animation system moves the entity itself (Transform += smoothed velocity). Turn off when a character
    // controller / physics moves it: the system then only reads the entity's actual velocity from its movement.
    bool kinematic = true;
    f32 velocityHalfLife = 0.12f; // seconds; smoothing of the desired velocity (acceleration feel)
    f32 turnHalfLife = 0.12f;     // seconds; smoothing of the body heading
    f32 turnInPlaceDegrees = 70.0f; // standing still, facing further off than this plays a turn clip
    // Preview: while previewClip is set the character plays exactly that clip (in place) and nothing else, for looking
    // at any clip on any character. Set it in the inspector; clear it to go back to locomotion.
    AssetId previewClip;
    f32 previewTime = 0.0f;
    f32 previewSpeed = 1.0f;
    bool previewPlay = true;
};

// Jump: take-off (the jump clip up to the moment the feet leave the ground), air (the clip's airborne part, stretched to
// the real air time) and landing (the rest of the clip). Preview: LocomotionControl::previewClip.
enum class LocomotionMode : u32 { Move = 0, Turn = 1, OneShot = 2, Jump = 3, Preview = 4, IdleBreak = 5 };
enum class JumpPhase : u32 { TakeOff = 0, Air = 1, Land = 2 };

// Runtime state of the locomotion system. Reflected so a reload or a save keeps the gait phase.
struct LocomotionState {
    Vec3 velocity{0.0f};  // smoothed world velocity
    f32 yawVelocity = 0.0f; // radians per second, the spring state of the heading
    f32 phase = 0.0f;       // gait cycle, 0..1 (0: left foot down)
    f32 idleTime = 0.0f;    // seconds on the idle clip's timeline
    u32 mode = 0;           // LocomotionMode
    u32 stance = 0;         // 0 standing, 1 crouched (the move set in use)
    f32 modeTime = 0.0f;    // seconds since the current mode started
    AssetId oneShot;        // clip of the current Turn/OneShot
    f32 oneShotYaw = 0.0f;  // heading when it started
    f32 oneShotDuration = 0.0f;
    // Jump bookkeeping
    u32 jumpPhase = 0;      // JumpPhase
    f32 jumpClipTime = 0.0f; // where in the jump clip the pose is
    f32 airElapsed = 0.0f;
    f32 crouch01 = 0.0f;    // procedural crouch amount, smoothed (0 standing .. 1 crouched)
    f32 stillTime = 0.0f;   // seconds standing still in the idle; an idle break starts when it passes nextBreakAt
    f32 nextBreakAt = 0.0f;
    u32 random = 0x9E3779B9u;
    bool leftGround = false; // gameplay has reported grounded == false during this jump
    bool takeOffNow = false; // OUTPUT, true for one update when the feet leave the ground: gameplay applies the impulse
    bool initialized = false;
};

} // namespace foh

FOH_REFLECT(foh::Animator, skeleton, history, playbackSpeed, transitionHalfLife, inertialElapsed, jointCount, historyValid, enabled);
FOH_REFLECT(foh::LocomotionClips, idleClip, idleBreak0Clip, idleBreak1Clip, idleBreak2Clip, idleBreak3Clip, walkClip, runClip, sprintClip, walkBackClip, runBackClip, walkLeftClip, walkRightClip,
            runLeftClip, runRightClip, turnLeftClip, turnRightClip, turnLeft90Clip, turnRight90Clip, jumpClip, crouchIdleClip, crouchWalkClip, crouchWalkBackClip,
            crouchWalkLeftClip, crouchWalkRightClip);
FOH_REFLECT(foh::LocomotionControl, velocity, facing, jump, crouch, grounded, expectedAirTime, kinematic, velocityHalfLife, turnHalfLife,
            turnInPlaceDegrees, previewClip, previewTime, previewSpeed, previewPlay);
FOH_REFLECT(foh::LocomotionState, velocity, yawVelocity, phase, idleTime, mode, modeTime, stance, oneShot, oneShotYaw, oneShotDuration,
            jumpPhase, jumpClipTime, airElapsed, crouch01, stillTime, nextBreakAt, random, leftGround, takeOffNow, initialized);
