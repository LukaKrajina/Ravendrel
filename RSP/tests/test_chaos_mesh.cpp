// chaos 网格体 + 凸包单元测试
#include "test_framework.hpp"
#include "core/chaos_mesh.hpp"
#include "render/mesh_generator.hpp"

using namespace rvdlrsp::core;
using namespace rvdlrsp;

QTEST(chaos_mesh_build_from_cube) {
    render::Mesh cube = render::make_cube(1.0f);
    ChaosMesh cm;
    QCHECK(cm.build(cube, 2.0, {0, 5, 0}));
    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    size_t idx = cm.add_to(kernel);
    QCHECK(kernel.body_count() == 1);
    QCHECK(kernel.body(idx).mass == 2.0);
}

QTEST(chaos_mesh_build_from_sphere) {
    render::Mesh sphere = render::make_sphere(1.0f, 8, 12);
    ChaosMesh cm;
    QCHECK(cm.build(sphere, 1.0, {0, 0, 0}));
}

QTEST(convex_hull_sphere_points) {
    std::vector<qpc::Vec3> pts;
    for (int i = 0; i < 20; ++i)
        pts.push_back({std::cos(i * 0.5), std::sin(i * 0.5), std::cos(i * 0.3)});
    qpc::ConvexHull h = qpc::build_convex_hull(pts);
    QCHECK(h.vertices.size() >= 4);
    QCHECK(!h.face_normals.empty());
}

QTEST(convex_hull_cube) {
    render::Mesh cube = render::make_cube(1.0f);
    std::vector<qpc::Vec3> pts;
    for (const auto &v : cube.vertices)
        pts.push_back(v.position);
    qpc::ConvexHull h = qpc::build_convex_hull(pts);
    QCHECK(h.vertices.size() >= 4);
    QCHECK(h.face_normals.size() >= 4);
}
