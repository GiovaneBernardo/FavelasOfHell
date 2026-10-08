#pragma once

// Engine math types. glm is the implementation, but the rest of the code base
// uses these aliases only, so the backing library can change in one place.
//
// Conventions: right-handed, Y-up, column-major matrices, clip-space depth in
// [0, 1]. World positions in the big open world use the D* (double) types.

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "foh/types.h"

namespace foh {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

using DVec2 = glm::dvec2;
using DVec3 = glm::dvec3;
using DVec4 = glm::dvec4;
using DMat4 = glm::dmat4;

using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using UVec2 = glm::uvec2;
using UVec3 = glm::uvec3;

namespace math {
inline constexpr f32 kPi = glm::pi<f32>();
inline constexpr Vec3 kUp{0.0f, 1.0f, 0.0f};
} // namespace math

} // namespace foh
