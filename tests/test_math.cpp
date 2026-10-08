#include <doctest/doctest.h>

#include "gtabr/math.h"

using namespace gtabr;

TEST_CASE("Math aliases wrap glm") {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    CHECK(glm::dot(a, b) == doctest::Approx(32.0f));

    const Mat4 identity{1.0f};
    CHECK((identity * Vec4(a, 1.0f)).x == doctest::Approx(1.0f));
}

TEST_CASE("Projection uses [0,1] clip depth") {
    const Mat4 p = glm::perspective(math::kPi / 2.0f, 1.0f, 0.1f, 100.0f);
    const Vec4 nearPoint = p * Vec4(0.0f, 0.0f, -0.1f, 1.0f);
    CHECK(nearPoint.z / nearPoint.w == doctest::Approx(0.0f).epsilon(1e-4));
}
