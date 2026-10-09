# 0009: Skeletal animation: skinned meshes, clips, blending, GPU skinning

- Status: **Proposed** (needs the project lead's review). The parts marked "built" are implemented; the rest is the plan.
- Related: [ADR 0004](0004-model-and-texture-import.md) (import pipeline, which already reserved `Skeleton`/`AnimationClip`), [ADR 0003](0003-renderer-render-graph-viewports.md), [ADR 0001](0001-ecs-on-flecs.md); AGENTS.md rules 2, 3, 5, 8
- References: Bollinger, "Inertialization: High Performance Animation Transitions in Gears of War" (GDC 2018); Holden, "Spring-It-On: The Game Developer's Spring-Roll-Call" and "Dead Blending / Inertialization" (theorangeduck.com); Clavet, "Motion Matching and The Road to Next-Gen Animation" (GDC 2016, for what we are *not* doing yet); Kovar & Gleicher, "Flexible Automatic Motion Blending with Registration Curves" (2003, why blends need phase alignment); Ubisoft's crowd talks (vertex-animation / GPU crowds); Mixamo FBX specifics inspected from the lead's own files (below).

## Why animation looks bad in most clones, and what we do about it

The "uncanny" look has a short list of causes. Each one gets a concrete answer in the design instead of "polish later":

| Symptom | Cause | Answer |
|---|---|---|
| Feet slide on the ground | Entity speed differs from the speed the clip was authored at | Locomotion is **speed driven**: playback rate = actual speed / clip speed (stride matching), blend weights come from speed and direction, and the clip's own root motion is kept as data (`RootMotion`) |
| Walk -> run pops or "ice skates" | Linear cross-fade of clips whose feet are in different phases | Locomotion clips are blended **phase synchronised**: one shared cycle phase, every clip sampled at `phase + footDownOffset` (offset detected from foot height at import) |
| Pop or floaty mush at state changes | Cross-fade needs both poses evaluated for a while and blends a moving pose into a different moving pose | **Inertialization** (Bollinger 2018): switch to the new animation instantly and decay the *difference* (position + rotation + velocity) with a critically damped spring. Cheaper than a cross-fade (one pose is evaluated, not two) and keeps the momentum of the old motion |
| Twitching on rotations | Component-wise quaternion lerp, double cover (`q` and `-q`), un-normalised results | All blends use hemisphere-corrected normalised lerp (nlerp) for small angles and slerp when asked; offsets live in log space (rotation vectors) |
| Character body turns like a rigid box | Entity yaw snaps to the desired direction | Facing is a **critically damped spring** toward the desired heading; turn-in-place clips are chosen by the remaining angle (the Mixamo `turn 90 / turn` clips) and their root rotation drives the entity |
| Shuffling at the start/stop | Instant velocity changes | Desired velocity is also spring-smoothed (acceleration limited) before it reaches the blend space |
| Clip compression "shimmer" | Quantisation without error control | Key reduction with a **per-joint error bound** (angle in milliradians, position in millimetres) and smallest-three 16-bit quaternions |
| Characters "float" on slopes and stairs (this game: Vidigal is steep) | No ground adaptation | **Planned** (phase 5): two-bone foot IK + pelvis offset from ray casts |

## Decisions

### 1. Skeleton, clip and skinned mesh are assets (built)

Following ADR 0004: new sub-asset kinds, each with its own GUID.

- **`Skeleton`** (`fskel`, FourCC `SKEL`): joint names, parent indices (parents always precede children, so one forward pass computes the pose), rest pose (local TRS), inverse bind matrices per joint. Names are normalised on import (a `mixamorig:` style namespace prefix is stripped) and hashed (`nameHash`) so clips bind by name.
- **`AnimationClip`** (`fclip`, FourCC `ACLP`): duration, loop flag, per-joint tracks addressed by name hash (a clip from another file animates any skeleton that has the same joint names: this is the Mixamo workflow, where every clip is its own FBX), foot-contact phases for sync, root motion curve (below). **It does not depend on a skeleton asset.**
- **Skinned meshes**: the same `Mesh` asset with two more vertex streams, `Joints0` (4 x u8) and `Weights0` (4 x unorm8, normalised so they sum to exactly 255), and a `SKIN` chunk naming the skeleton. Vertex joint indices are indices into the **skeleton** (the importer remaps from the source skin's joint list), so a mesh needs no per-skin palette: the pose evaluator produces `jointModel * inverseBind` per skeleton joint and every mesh bound to that skeleton reads it. Maximum 255 joints per skeleton (Mixamo: 65).
- **Prefab**: bones are *not* entities. A skinned import produces one entity per skinned mesh under the root, plus the skeleton reference (`PrefabNode::skeleton`), and instantiating adds `Animator` + the pose state once on the root. 1000 characters x 65 joints would be 65000 entities otherwise.

### 2. Pose evaluation runs on the CPU job system; skinning runs on the GPU

The question was whether the whole animation should be GPU driven for crowds. The recommendation is **split by cost, not by principle**:

- *Skinning* (the per-vertex part, the expensive and embarrassingly parallel one) is **compute pre-skinning on the GPU** (built). One dispatch per viewport scene skins every visible character into a transient vertex buffer in the normal `MeshVertex` layout; the existing depth, shadow, G-buffer and picking pipelines then draw that buffer unchanged. Pre-skinning (instead of skinning inside every vertex shader) pays for the work once instead of 1 + 4 (cascades) + picking times, and the result is also what hardware ray tracing needs later (BLAS refit input).
- *Pose evaluation* (clip sampling, blending, inertialization, state machine, root motion; ~65 joints per character) stays on the **CPU job system**, one job per character, SoA-friendly flat arrays, no allocation. Characters that matter need gameplay-driven state (the player, nearby NPCs), and a state machine on the GPU buys nothing for 65 joints. At ~65 joints the cost is a few microseconds per character: 1000 evaluated characters are a fraction of a millisecond spread over the workers.
- *Crowd tier* (**planned**, phase 4): for far/background NPCs the clips are baked into GPU-friendly uniformly sampled tracks, and a compute shader samples + blends two clips per character straight into the palette (no CPU work and no per-character upload except a tiny state record). That is the "GPU driven animation" the lead asked about; it is built on the same skinning pass, so it is an addition, not a rewrite. We deliberately do not start there: it cannot do inertialization, IK, or gameplay layers, and it needs the data-oriented clip format below to exist first.

### 3. Runtime model (built unless marked)

`engine/animation` (new module, depends on core, jobs, ecs, assets):

- `Pose`: flat arrays of local TRS per joint. Pure functions: `sampleClip`, `blendPoses`, `additive`, `toModelSpace`, `buildPalette`. No hidden state, unit tested.
- `Animator` component (POD, reflected): the skeleton, the locomotion profile (clip ids), and the smoothed locomotion parameters. Large state (current/previous pose, inertialization offsets and velocities) lives in an engine `BufferHandle`, so it is freed with the entity and survives game module reloads.
- `LocomotionControl` component (what gameplay writes): desired velocity (world space, m/s), desired facing, flags (`jump`...). Gameplay never touches clips.
- Systems, in this order: `locomotionControl` (smoothing, state and clip selection, phase advance) -> `evaluatePose` (parallel per character) -> `applyRootMotion` -> (existing) transform propagation -> extraction.
- Locomotion = **radial blend space**: rings by speed (idle, walk, run, optionally sprint), clips by angle within the ring (forward, strafe left/right, backward when present). Two adjacent clips per ring, two adjacent rings: at most four clips evaluated per character. Missing directions are handled by letting the character *turn* instead of strafing (NPCs) unless the profile asks for strafing (player aiming).
- One-shots (turn in place, jump, ...) are entered via inertialization, so there is no "blend time" tuning per pair of states.

### 4. Root motion and "motion vectors"

The lead asked for "motion vectors for character movement". Interpreted as **root motion**: the translation and yaw the animation itself performs, kept as explicit data instead of staying baked in the hips (inspected Mixamo files: `walking.fbx` moves the hips 1.65 m in 1.03 s, `running.fbx` 2.94 m in 0.70 s; the strafes move along X; the turn clips rotate; all of it is baked into `mixamorig:Hips`).

- At import the hips' planar translation and yaw are split into a **root motion curve** (dx, dz, dyaw per sample). Looping clips store the *linear* part (average velocity and yaw rate), so the natural pelvis sway stays in the pose; one-shot clips store the exact curve. The pose that remains is "in place", so the entity can be driven either way:
  - *controller driven* (default for players and NPCs: gameplay decides the movement, animation is stride-matched to it),
  - *animation driven* (cutscenes, vault/climb, melee: the entity moves by the clip's root motion delta).
- The same data gives clip speed for stride matching and the expected displacement for turn clips (so the entity ends facing the target).
- **Screen-space motion vectors** (per-pixel velocity for TAA / motion blur) are not part of this phase: the renderer has no velocity target yet. The skinning pass keeps the previous frame's palette addressable so the velocity pass can be added without touching animation; that is flagged as a renderer task, not done here.

### 5. Import (built for glTF and FBX)

`parse -> ImportedScene -> cook` as in ADR 0004. `ImportedScene` gains skeletons, skinned meshes (joint/weight arrays per vertex) and clips. Parsers produce *densely sampled* local TRS per joint (FBX: `ufbx_bake_anim`, which resolves Euler orders, pre/post rotations and curve interpolation; glTF: the samplers are resampled, step/linear/cubic-spline). The cook step does everything format independent: name normalisation, root motion split, loop detection, foot phase detection, key reduction and quantisation. Mixamo specifics handled: 100x unit scale on the armature, `mixamorig:` prefix, clips downloaded without skin (skeleton only), 30 Hz dense keys with a duplicated closing frame on loops (the loop duration is the time of the *last* key; the pose at `t = duration` equals the pose at 0).

### 6. Hot reload and state

Clip/skeleton/mesh assets reload like every other sub-asset (ADR 0006 loader). Animation state is in reflected components plus engine-owned buffers; the pose buffers are derived data and rebuilt from the components if their layout changes (one-frame pop at worst, no crash).

## Phases

1. **Assets + import** (skeleton, clip, skin streams; glTF + FBX; tests) - built
2. **Runtime** (pose math, sampling, blend space, inertialization, root motion, systems; tests) - built
3. **GPU skinning** (store skin data, compute pass, draws; D3D12 validation clean) - built
4. **Crowd tier** (GPU clip sampling for distant characters; LOD of pose evaluation rate) - planned
5. **Foot IK, look-at, aim/upper-body layer, additive hit reactions** - planned
6. **Motion vectors** (renderer velocity target) - planned, renderer side

## Consequences

- A character costs: one entity, one `Animator`, ~8 KB of pose state, a 3 KB palette upload per frame, one compute group per 64 vertices.
- Clips are shared and tiny (Mixamo walk: 65 tracks, mostly constant; with key reduction a few KB).
- Skinned instances are drawn one draw call per character per submesh (each has its own skinned vertices); batching them (indirect draws) is part of the crowd tier.
- Retargeting is by joint name only. A different skeleton topology (non-Mixamo) needs a retarget asset; not designed here.
