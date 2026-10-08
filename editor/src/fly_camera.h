#pragma once

#include <algorithm>

#include "foh/math.h"
#include "foh/window.h"

namespace foh {

// Free-fly editor camera: RMB + mouse look, WASD move, Q/E down/up, wheel = speed.
// Editor state, not an ECS entity: it never reaches the game world.
struct FlyCamera {
    Vec3 position{0.0f, 16.0f, 34.0f};
    f32 yaw = 0.0f;     // radians around +Y; 0 looks down -Z
    f32 pitch = -0.38f;  // radians; negative looks down
    f32 speed = 8.0f;   // m/s
    f32 fovY = 1.0471976f;
    f32 nearZ = 0.1f;
    f32 farZ = 4000.0f;

    Quat rotation() const {
        return glm::angleAxis(yaw, Vec3(0, 1, 0)) * glm::angleAxis(pitch, Vec3(1, 0, 0));
    }
    Mat4 world() const { return glm::translate(Mat4(1.0f), position) * glm::mat4_cast(rotation()); }

    // `active`: this camera currently owns the input (viewport hovered/focused and RMB held).
    void update(const InputState& in, f32 dt, bool active, bool hovered) {
        if (hovered && in.wheelY != 0.0f) speed = std::clamp(speed * (in.wheelY > 0 ? 1.25f : 0.8f), 0.5f, 400.0f);
        if (!active) return;
        constexpr f32 kSensitivity = 0.0025f;
        yaw -= in.mouseDeltaX * kSensitivity;
        pitch = std::clamp(pitch - in.mouseDeltaY * kSensitivity, -1.55f, 1.55f);

        const Quat q = rotation();
        const Vec3 forward = q * Vec3(0, 0, -1);
        const Vec3 right = q * Vec3(1, 0, 0);
        Vec3 move(0.0f);
        if (in.isDown(Key::W)) move += forward;
        if (in.isDown(Key::S)) move -= forward;
        if (in.isDown(Key::D)) move += right;
        if (in.isDown(Key::A)) move -= right;
        if (in.isDown(Key::E)) move += Vec3(0, 1, 0);
        if (in.isDown(Key::Q)) move -= Vec3(0, 1, 0);
        if (glm::dot(move, move) > 0.0f) {
            const f32 boost = in.isDown(Key::LShift) ? 4.0f : 1.0f;
            position += glm::normalize(move) * speed * boost * dt;
        }
    }
};

} // namespace foh
