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

    // ─────────────────────────────────────────────────────────────
    // 义肢 → Robot 刚体驱动
    //
    // 把 ProstheticLimb 的关节角映射到 Robot 的刚体朝向，实现
    // 「脑意识控制 + 量子 RL 辅助」驱动的物理义肢。
    //
    // 映射：bone_names_[i] 对应 limb 的第 i 个关节。drive() 每步推进
    // 义肢控制，并把关节角应用为对应骨骼刚体的「父朝向 × 绕关节轴
    // 旋转(关节角)」的复合朝向，沿骨架链前向传播。
    // ─────────────────────────────────────────────────────────────
    class ProstheticRobotDriver
    {
    private:
        ProstheticLimb *limb_;
        core::Robot *robot_;
        qpc::PhysicsKernel *kernel_;
        std::vector<std::string> bone_names_; // 与 limb 关节顺序对应的骨骼名

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

        // 每步：推进义肢控制，把关节角映射为 Robot 刚体朝向
        void drive()
        {
            limb_->step();
            const auto &angles = limb_->angles();
            const auto &joints = limb_->joints();
            for (size_t i = 0; i < bone_names_.size() && i < angles.size(); ++i)
            {
                core::Bone *bone = robot_->skeleton().find(bone_names_[i]);
                if (!bone)
                    continue;                       // 防御：骨骼不存在
                if (bone->body_index >= kernel_->body_count())
                    continue;                       // 防御：刚体索引越界

                qpc::RigidBody &body = kernel_->body(bone->body_index);

                // 父朝向（世界空间）
                qpc::Quat parent_orient;
                if (bone->parent && bone->parent->body_index < kernel_->body_count())
                    parent_orient = kernel_->body(bone->parent->body_index).orientation;

                // 关节轴（局部），绕局部轴旋转 = 局部旋转
                qpc::Vec3 axis = (i < joints.size()) ? joints[i].axis : qpc::Vec3{1.0, 0.0, 0.0};

                // 世界朝向 = 父朝向 × 绕局部轴旋转(关节角)
                body.orientation = (parent_orient * qpc::Quat::axis_angle(axis, angles[i]))
                                       .normalized();
            }
        }
    };

    // ─── 义肢驱动工厂：装配「接受腔→上臂→肘→前臂→腕→假手」──────
    // 返回与 make_prosthetic_arm() 的零件名对应的骨骼名顺序，
    // 与 make_prosthetic_arm_joints() 的 5 个关节一一对应。
    inline std::vector<std::string> make_prosthetic_arm_bones()
    {
        return {"upper_arm", "elbow", "forearm", "wrist", "prosthetic_hand"};
    }

    // ─────────────────────────────────────────────────────────────
    // 义肢物理环境：接入 RLPipeline 做端到端量子强化学习
    //
    // 实现 IEnvironment，把「脑意识意图 + 量子 RL 补偿」映射到
    // Robot 刚体关节驱动，并在物理内核上步进：
    //   观测 = [关节角, 目标角]（展开），动作 = [目标关节角]，
    //   奖励 = 负跟踪误差平方和。
    // 可直接传入 RLPipeline::train(env, agent, cfg) 训练。
    // ─────────────────────────────────────────────────────────────
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
            // 动作 = 目标关节角（脑意识意图 / 策略输出）
            if (action.size() == target_.size())
                limb_.set_target(action);

            // 义肢驱动刚体（含关节角推进）+ 物理步进
            driver_.drive();
            kernel_.step();

            ++step_;

            // 奖励 = 负跟踪误差平方和
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

    // ─── 端到端训练便利函数：义肢物理环境 + RLPipeline ─────────
    // 创建义肢 Robot + 物理内核 + 义肢环境，注入量子后端，
    // 用 RLPipeline 训练量子 RL 智能体。
    inline double train_prosthetic_physics(int episodes = 50)
    {
        core::Robot robot = core::make_prosthetic_arm();
        qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
        robot.build(kernel);

        ProstheticPhysicsEnvironment env(
            make_prosthetic_arm_joints(), robot, kernel, make_prosthetic_arm_bones());

        QuantumRLAgent agent(env.observation_dim(), env.action_dim(), 0.01);
        agent.set_backend(kernel.backend()); // 注入量子后端（量子探索）

        RLConfig cfg;
        cfg.episodes = episodes;
        cfg.max_steps = 100;
        return RLPipeline::train(env, agent, cfg);
    }

}
