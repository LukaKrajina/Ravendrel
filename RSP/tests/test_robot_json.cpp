// 机器人 JSON 装配单元测试
#include "test_framework.hpp"
#include "core/robot.hpp"

using namespace rvdlrsp::core;
using namespace rvdlrsp;

QTEST(robot_from_json_parts) {
    const char *json = R"({
        "parts": [
            {"name": "base", "parent": "", "joint": "BallSocket", "pos": [0,0,0],
             "collider": "AABB", "radius": 0.2, "half_height": 0.1, "mass": 2.0, "shape": "Box"},
            {"name": "arm", "parent": "base", "joint": "Hinge", "pos": [0,0.2,0],
             "collider": "Cylinder", "radius": 0.1, "half_height": 0.15, "mass": 1.0, "shape": "Cylinder"}
        ]
    })";
    Robot r = robot_from_json(json);

    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    r.build(kernel);

    QCHECK(r.meshes().size() == 2);
    QCHECK(r.instances().size() == 2);
    QCHECK(kernel.body_count() == 2);
    QCHECK(kernel.joints().size() == 1);
}

QTEST(humanoid_robot_build) {
    Robot r = make_humanoid_robot();
    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    r.build(kernel);
    QCHECK(r.meshes().size() == 16);
    QCHECK(kernel.body_count() == 16);
    QCHECK(kernel.joints().size() == 15);
}
