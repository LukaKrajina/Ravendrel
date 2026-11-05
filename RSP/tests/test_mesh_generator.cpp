// 程序化几何体单元测试（圆柱 / 锥体 / 圆环 / 平面）
#include "test_framework.hpp"
#include "render/mesh_generator.hpp"

using namespace rvdlrsp::render;

QTEST(make_cylinder_valid) {
    Mesh m = make_cylinder(0.5f, 1.0f, 16);
    QCHECK(!m.vertices.empty());
    QCHECK(m.indices.size() % 3 == 0);
}

QTEST(make_cone_valid) {
    Mesh m = make_cone(0.5f, 1.0f, 16);
    QCHECK(!m.vertices.empty());
    QCHECK(m.indices.size() % 3 == 0);
}

QTEST(make_torus_valid) {
    Mesh m = make_torus(1.0f, 0.3f);
    QCHECK(!m.vertices.empty());
    QCHECK(m.indices.size() % 3 == 0);
}

QTEST(make_plane_valid) {
    Mesh m = make_plane(2.0f);
    QCHECK(m.vertices.size() == 4);
    QCHECK(m.indices.size() == 6);
}