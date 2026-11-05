#include "rvdlRSP.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace rvdlrsp;

// ─── 网格生成 ────────────────────────────────────────────
static render::Mesh make_sphere(float radius, int stacks, int slices) {
    render::Mesh m;
    m.name = "sphere";
    for (int i = 0; i <= stacks; ++i) {
        double phi = M_PI * i / stacks;
        double y = std::cos(phi), r = std::sin(phi);
        for (int j = 0; j <= slices; ++j) {
            double theta = 2.0 * M_PI * j / slices;
            double x = r * std::cos(theta), z = r * std::sin(theta);
            render::Vertex v;
            v.position = {x * radius, y * radius, z * radius};
            v.normal = {x, y, z};
            v.r = static_cast<float>(0.5 + 0.5 * y);
            v.g = static_cast<float>(0.4 + 0.4 * (1.0 - y));
            v.b = 0.9f;
            m.vertices.push_back(v);
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            uint32_t a = i * (slices + 1) + j, b = a + slices + 1;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return m;
}

static render::Mesh make_cube(float scale) {
    render::Mesh m;
    m.name = "cube";
    float h = 0.5f * scale;
    render::Vertex v[8] = {
        {{-h,-h,-h},{0,-1,0},0.7f,0.7f,0.7f}, {{h,-h,-h},{0,-1,0},0.7f,0.7f,0.7f},
        {{h, h,-h},{0, 1,0},0.7f,0.7f,0.7f}, {{-h, h,-h},{0, 1,0},0.7f,0.7f,0.7f},
        {{-h,-h, h},{0,-1,0},0.7f,0.7f,0.7f}, {{h,-h, h},{0,-1,0},0.7f,0.7f,0.7f},
        {{h, h, h},{0, 1,0},0.7f,0.7f,0.7f},  {{-h, h, h},{0, 1,0},0.7f,0.7f,0.7f}};
    for (auto &x : v) m.vertices.push_back(x);
    uint32_t idx[36] = {0,1,2, 0,2,3, 5,4,7, 5,7,6, 4,0,3, 4,3,7,
                        1,5,6, 1,6,2, 3,2,6, 3,6,7, 4,5,1, 4,1,0};
    for (auto i : idx) m.indices.push_back(i);
    return m;
}

int main() {
    // ─── 渲染引擎 ────────────────────────────────────────
    render::Renderer renderer;
    if (!renderer.init("rvdlRSP — QCDRC Teleop + Physics + Render", 1280, 720,
                       "shaders/mesh.vert.spv", "shaders/mesh.frag.spv")) {
        std::cerr << "[rvdlRSP] Failed to init renderer.\n";
        return 1;
    }

    // ─── 物理内核 ────────────────────────────────────────
    const double fixed_dt = 1.0 / 60.0;
    qpc::PhysicsKernel kernel(false, fixed_dt);
    kernel.set_gravity({0, -9.81, 0});
    kernel.set_solver_iterations(4);

    // 地面
    qpc::RigidBody ground;
    ground.set_static(true);
    ground.position = {0, -2.5, 0};
    qpc::Collider gc;
    gc.type = qpc::ShapeType::AABB;
    gc.half_extents = {12, 0.5, 12};
    kernel.add_body(ground, gc);

    // 机器人刚体（受遥操作驱动）
    qpc::RigidBody robot;
    robot.set_mass(2.0);
    robot.position = {0, 0.5, 0};
    robot.restitution = 0.2;
    qpc::Collider rc;
    rc.type = qpc::ShapeType::Sphere;
    rc.radius = 0.5;
    size_t robot_id = kernel.add_body(robot, rc);

    // ─── QCDRC 遥操作 ────────────────────────────────────
    qcdrc::Teleop teleop;
    qcdrc::TeleopDriver::Config drive_cfg;

    // ─── 场景 ───────────────────────────────────────────
    render::Scene scene;
    scene.meshes.push_back(make_sphere(0.5f, 16, 24));  // mesh 0：机器人（球）
    scene.meshes.push_back(make_cube(1.0f));             // mesh 1：地面
    scene.meshes.push_back(make_sphere(0.15f, 8, 12));   // mesh 2：目标点

    render::SceneInstance ground_inst;
    ground_inst.mesh_id = 1;
    ground_inst.position = {0, -2.5, 0};
    ground_inst.scale = {24, 1.0, 24};
    scene.instances.push_back(ground_inst);

    render::SceneInstance robot_inst;
    robot_inst.mesh_id = 0;
    robot_inst.position = robot.position;
    scene.instances.push_back(robot_inst);

    render::SceneInstance target_inst;
    target_inst.mesh_id = 2;
    target_inst.position = {0, 0.5, 0};
    scene.instances.push_back(target_inst);

    scene.camera.position = {0, 6, 14};
    scene.camera.target = {0, 1.0, 0};
    scene.light.direction = {0.3, -1.0, 0.5};
    scene.light.intensity = 1.2f;

    renderer.submit_scene(scene);

    // ─── 主循环：遥操作 → 物理 → 渲染 闭环 ─────────────
    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    double accumulator = 0.0;
    double sim_time = 0.0;

    std::cout << "[rvdlRSP] QCDRC teleop loop started.\n";
    while (!renderer.should_close()) {
        auto now = clock::now();
        double frame_dt = std::chrono::duration<double>(now - prev).count();
        prev = now;
        if (frame_dt > 0.25) frame_dt = 0.25;

        // 遥操作一步：capture → estimate → map → ik
        std::vector<double> joint_angles = teleop.teleop_step();
        qpc::Vec3 target = qcdrc::TeleopDriver::joint_to_target(joint_angles, drive_cfg);

        // 驱动机器人刚体（PD 控制）
        kernel.body(robot_id).apply_force(
            qcdrc::TeleopDriver::compute_force(kernel.body(robot_id), target, drive_cfg));

        // 固定步长物理
        accumulator += frame_dt;
        while (accumulator >= fixed_dt) {
            kernel.step();
            accumulator -= fixed_dt;
            sim_time += fixed_dt;
        }

        // 同步到渲染
        scene.instances[1].position = kernel.body(robot_id).position;
        scene.instances[1].orientation = kernel.body(robot_id).orientation;
        scene.instances[2].position = target;

        renderer.submit_scene(scene);
        renderer.render_frame();
    }

    renderer.shutdown();
    std::cout << "[rvdlRSP] Teleop loop exited after " << std::fixed << std::setprecision(1)
              << sim_time << "s simulated.\n";
    return 0;
}
