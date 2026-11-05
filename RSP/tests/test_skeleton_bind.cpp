// 骨架系统单元测试（层级 / 世界变换 / 绑定姿态）
#include "test_framework.hpp"
#include "core/skeleton.hpp"

using namespace rvdlrsp::core;

QTEST(skeleton_build_hierarchy) {
    Skeleton sk;
    Bone *root = sk.add_bone("root", nullptr, {0, 0, 0});
    Bone *child = sk.add_bone("child", root, {0, 1, 0});
    QCHECK(sk.root() == root);
    QCHECK(root->children.size() == 1);
    QCHECK(root->children[0] == child);
    QCHECK(child->parent_index == 0);
    QCHECK(sk.size() == 2);
}

QTEST(skeleton_world_transform) {
    Skeleton sk;
    sk.add_bone("root", nullptr, {0, 0, 0});
    sk.add_bone("child", sk.root(), {0, 1, 0});
    sk.update_world_transforms();
    Bone *child = sk.find("child");
    QCHECK_NEAR(child->world_pos.y, 1.0, 1e-6);
}

QTEST(skeleton_bind_pose) {
    Skeleton sk;
    sk.add_bone("root", nullptr, {0, 0, 0});
    sk.add_bone("child", sk.root(), {0, 1, 0});
    sk.compute_bind_pose();
    Bone *child = sk.find("child");
    QCHECK_NEAR(child->bind_pos.y, 1.0, 1e-6);
    // 世界绑定矩阵平移 y = 1；逆绑定矩阵平移 y = -1
    QCHECK_NEAR(child->bind_world.m[13], 1.0, 1e-4);
    QCHECK_NEAR(child->inverse_bind.m[13], -1.0, 1e-4);
}
