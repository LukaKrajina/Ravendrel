#pragma once
#include <vector>
#include <cmath>
#include <iostream>
#include "hardware/observability.hpp"
#include "rl_pipeline.hpp"
#include "../qpc/physics_kernel.hpp"

namespace rvdlrsp::control {

    class PhysicsEnvironment : public IEnvironment {
    private:
        qpc::PhysicsKernel kernel_;
        size_t robot_id_ = 0;
        size_t target_id_ = 0;
        qpc::Vec3 target_{3.0, 0.5, 2.0};
        int step_ = 0;

    public:
        PhysicsEnvironment() : kernel_(false, 1.0 / 60.0) {
            kernel_.set_gravity({0, -9.81, 0});
            kernel_.set_solver_iterations(4);

            // 地面
            qpc::RigidBody ground;
            ground.set_static(true);
            ground.position = {0, -2.5, 0};
            qpc::Collider gc;
            gc.type = qpc::ShapeType::AABB;
            gc.half_extents = {12, 0.5, 12};
            kernel_.add_body(ground, gc);

            // 机器人球
            qpc::RigidBody robot;
            robot.set_mass(2.0);
            robot.position = {0, 0.5, 0};
            robot.restitution = 0.2;
            qpc::Collider rc;
            rc.type = qpc::ShapeType::Sphere;
            rc.radius = 0.5;
            robot_id_ = kernel_.add_body(robot, rc);

            // 目标球（静态）
            qpc::RigidBody target_body;
            target_body.set_static(true);
            target_body.position = target_;
            qpc::Collider tc;
            tc.type = qpc::ShapeType::Sphere;
            tc.radius = 0.2;
            target_id_ = kernel_.add_body(target_body, tc);

            RVDLRSP_INFO("rl") << "Physics environment online (target "
                                << target_.x << "," << target_.z << ").";
        }

        std::vector<double> reset() override {
            step_ = 0;
            kernel_.body(robot_id_).position = {0, 0.5, 0};
            kernel_.body(robot_id_).linear_velocity = {0, 0, 0};
            return observe();
        }

        StepResult step(const std::vector<double> &action) override {
            // 动作 → 控制力（X/Z 方向）
            double fx = action.empty() ? 0.0 : action[0] * 20.0;
            double fz = action.size() > 1 ? action[1] * 20.0 : 0.0;
            kernel_.body(robot_id_).apply_force({fx, 0.0, fz});

            // 物理步进 10 个子步
            for (int i = 0; i < 10; ++i) kernel_.step();

            ++step_;
            StepResult r;
            r.observation = observe();
            r.reward = -distance();
            r.done = (step_ >= 120) || (distance() < 0.2);
            return r;
        }

        size_t observation_dim() const override { return 3; }
        size_t action_dim() const override { return 2; }

        const qpc::PhysicsKernel &kernel() const { return kernel_; }
        qpc::PhysicsKernel &kernel() { return kernel_; }
        qhal::IQuantumBackend *backend() { return kernel_.backend(); }

    private:
        double distance() const {
            const qpc::Vec3 &p = kernel_.body(robot_id_).position;
            double dx = p.x - target_.x;
            double dz = p.z - target_.z;
            return std::sqrt(dx * dx + dz * dz);
        }

        std::vector<double> observe() const {
            const qpc::Vec3 &p = kernel_.body(robot_id_).position;
            return {p.x - target_.x, p.z - target_.z, distance()};
        }
    };
}