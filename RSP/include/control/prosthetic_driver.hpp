#pragma once
#include <vector>
#include <string>
#include <utility>
#include <iostream>
#include "hardware/observability.hpp"
#include "prosthetic.hpp"
#include "core/robot.hpp"
#include "qpc/physics_kernel.hpp"

namespace rvdlrsp::control
{

    class ProstheticRobotDriver
    {
    private:
        ProstheticLimb *limb_;
        core::Robot *robot_;
        qpc::PhysicsKernel *kernel_;
        std::vector<std::string> bone_names_;

    public:
        ProstheticRobotDriver(ProstheticLimb &limb, core::Robot &robot,
                              qpc::PhysicsKernel &kernel,
                              std::vector<std::string> bone_names)
            : limb_(&limb), robot_(&robot), kernel_(&kernel),
              bone_names_(std::move(bone_names))
        {
            RVDLRSP_INFO("prosthetic") << "Prosthetic robot driver online ("
                                        << bone_names_.size() << " bones).";
        }

        ProstheticLimb &limb() { return *limb_; }
        const std::vector<std::string> &bone_names() const { return bone_names_; }

        void drive()
        {
            limb_->step();
            const auto &angles = limb_->angles();
            const auto &joints = limb_->joints();
            for (size_t i = 0; i < bone_names_.size() && i < angles.size(); ++i)
            {
                core::Bone *bone = robot_->skeleton().find(bone_names_[i]);
                if (!bone)
                    continue;
                if (bone->body_index >= kernel_->body_count())
                    continue;

                qpc::RigidBody &body = kernel_->body(bone->body_index);

                qpc::Quat parent_orient;
                if (bone->parent && bone->parent->body_index < kernel_->body_count())
                    parent_orient = kernel_->body(bone->parent->body_index).orientation;

                qpc::Vec3 axis = (i < joints.size()) ? joints[i].axis : qpc::Vec3{1.0, 0.0, 0.0};

                body.orientation = (parent_orient * qpc::Quat::axis_angle(axis, angles[i]))
                                       .normalized();
            }
        }
    };

    inline std::vector<std::string> make_prosthetic_arm_bones()
    {
        return {"upper_arm", "elbow", "forearm", "wrist", "prosthetic_hand"};
    }

    class ProstheticPhysicsEnvironment : public IEnvironment
    {
    private:
        ProstheticLimb limb_;
        ProstheticRobotDriver driver_;
        qpc::PhysicsKernel &kernel_;
        std::vector<double> target_;
        int step_ = 0;

    public:
        ProstheticPhysicsEnvironment(std::vector<ProstheticJoint> joints,
                                     core::Robot &robot,
                                     qpc::PhysicsKernel &kernel,
                                     std::vector<std::string> bone_names)
            : limb_(std::move(joints)),
              driver_(limb_, robot, kernel, std::move(bone_names)),
              kernel_(kernel)
        {
            target_.assign(limb_.joint_count(), 0.0);
            RVDLRSP_INFO("prosthetic") << "Prosthetic physics environment online.";
        }

        std::vector<double> reset() override
        {
            step_ = 0;
            for (size_t i = 0; i < target_.size(); ++i)
                target_[i] = limb_.joints()[i].rest_angle;
            limb_.set_target(target_);
            return observe();
        }

        StepResult step(const std::vector<double> &action) override
        {
            if (action.size() == target_.size())
                limb_.set_target(action);

            driver_.drive();
            kernel_.step();

            ++step_;

            double reward = 0.0;
            const auto &a = limb_.angles();
            for (size_t i = 0; i < a.size(); ++i)
            {
                double err = target_[i] - a[i];
                reward -= err * err;
            }

            StepResult r;
            r.observation = observe();
            r.reward = reward;
            r.done = (step_ >= 100);
            return r;
        }

        size_t observation_dim() const override { return limb_.joint_count() * 2; }
        size_t action_dim() const override { return limb_.joint_count(); }

        ProstheticLimb &limb() { return limb_; }
        ProstheticRobotDriver &driver() { return driver_; }

    private:
        std::vector<double> observe()
        {
            std::vector<double> obs;
            obs.reserve(limb_.joint_count() * 2);
            const auto &a = limb_.angles();
            const auto &t = limb_.target_angles();
            for (size_t i = 0; i < limb_.joint_count(); ++i)
            {
                obs.push_back(a[i]);
                obs.push_back(t[i]);
            }
            return obs;
        }
    };

    inline double train_prosthetic_physics(int episodes = 50)
    {
        core::Robot robot = core::make_prosthetic_arm();
        qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
        robot.build(kernel);

        ProstheticPhysicsEnvironment env(
            make_prosthetic_arm_joints(), robot, kernel, make_prosthetic_arm_bones());

        QuantumRLAgent agent(env.observation_dim(), env.action_dim(), 0.01);
        agent.set_backend(kernel.backend());

        RLConfig cfg;
        cfg.episodes = episodes;
        cfg.max_steps = 100;
        return RLPipeline::train(env, agent, cfg);
    }

}