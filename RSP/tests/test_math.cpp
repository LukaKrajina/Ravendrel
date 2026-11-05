// 数学库单元测试
#include "test_framework.hpp"
#include "qpc/math.hpp"

using namespace rvdlrsp::qpc;

QTEST(vec3_basic) {
    Vec3 a{1, 2, 3}, b{4, 5, 6};
    Vec3 s = a + b;
    QCHECK_NEAR(s.x, 5, 1e-9);
    QCHECK_NEAR(s.y, 7, 1e-9);
    QCHECK_NEAR(s.z, 9, 1e-9);

    Vec3 d = b - a;
    QCHECK_NEAR(d.x, 3, 1e-9);

    Vec3 m = a * 2.0;
    QCHECK_NEAR(m.y, 4, 1e-9);
}

QTEST(vec3_dot_cross) {
    Vec3 a{1, 0, 0}, b{0, 1, 0};
    QCHECK_NEAR(a.dot(b), 0, 1e-9);
    Vec3 c = a.cross(b);
    QCHECK_NEAR(c.z, 1, 1e-9);
}

QTEST(vec3_normalize) {
    Vec3 v{3, 0, 0};
    QCHECK_NEAR(v.normalized().length(), 1.0, 1e-9);
    QCHECK_NEAR(v.normalized().x, 1.0, 1e-9);
}

QTEST(quat_identity_rotate) {
    Quat q; // 单位四元数
    Vec3 v{1, 2, 3};
    Vec3 r = q.rotate(v);
    QCHECK_NEAR(r.x, 1, 1e-9);
    QCHECK_NEAR(r.y, 2, 1e-9);
    QCHECK_NEAR(r.z, 3, 1e-9);
}

QTEST(quat_axis_angle_90) {
    Quat q = Quat::axis_angle({0, 1, 0}, 3.141592653589793 / 2.0); // 绕 Y 转 90°
    Vec3 v{1, 0, 0};
    Vec3 r = q.rotate(v);
    // (1,0,0) 绕 Y 转 90° → (0,0,-1)
    QCHECK_NEAR(r.z, -1, 1e-6);
    QCHECK_NEAR(r.x, 0, 1e-6);
}
