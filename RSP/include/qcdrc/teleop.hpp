#pragma once
#include <vector>
#include <string>
#include <iostream>
#include "hardware/observability.hpp"
#include "camera.hpp"
#include "mocap.hpp"
#include "ik.hpp"

namespace rvdlrsp::qcdrc
{

    // 遥操作目标关节角（机器人侧）
    struct RobotJointTarget
    {
        std::string name;
        double angle_rad = 0.0;
    };

    class Teleop
    {
    private:
        RgbCamera camera_;
        MotionCapture mocap_;
        std::vector<RobotJointTarget> joint_targets_;

    public:
        Teleop()
        {
            RVDLRSP_INFO("qcdrc") << "QCDRC real-time teleop online.";
        }

        // 采样一帧 RGB
        RgbFrame capture() { return camera_.capture(); }

        // 从相机帧估计全身骨架
        const Skeleton &estimate_pose(const RgbFrame &frame)
        {
            return mocap_.estimate(frame);
        }

        // 动作映射：人体骨架 → 机器人关节目标
        // 从人体骨架 3D 关节位置用向量几何提取真实关节角：肩抬升角、肘弯曲角、腕倾角，对应义肢
        // shoulder/elbow/wrist/finger_thumb/finger_index 五关节。
        void map_motion(const Skeleton &skel)
        {
            joint_targets_.clear();

            const Joint3D *neck = find_joint(skel, "neck");
            const Joint3D *pelvis = find_joint(skel, "pelvis");
            const Joint3D *shoulder = find_joint(skel, "r_shoulder");
            const Joint3D *elbow = find_joint(skel, "r_elbow");
            const Joint3D *wrist = find_joint(skel, "r_wrist");

            // 右臂缺失时回退左臂
            if (!shoulder || !elbow || !wrist)
            {
                shoulder = find_joint(skel, "l_shoulder");
                elbow = find_joint(skel, "l_elbow");
                wrist = find_joint(skel, "l_wrist");
            }

            double shoulder_angle = 0.0;
            double elbow_angle = 0.0;
            double wrist_angle = 0.0;

            if (shoulder && elbow && wrist)
            {
                // 肘弯曲角：伸直 = 0，弯曲增大
                elbow_angle = elbow_flexion(*shoulder, *elbow, *wrist);
                // 肩抬升角：上臂相对躯干轴，T-pose = 0
                if (neck && pelvis)
                    shoulder_angle = shoulder_lift(*shoulder, *elbow, *neck, *pelvis);
                // 腕倾角：前臂相对竖直轴的倾角（骨架无手关节，以姿态近似）
                double fx = wrist->x - elbow->x;
                double fy = wrist->y - elbow->y;
                double fz = wrist->z - elbow->z;
                wrist_angle = std::atan2(std::sqrt(fx * fx + fz * fz), fy);
            }

            joint_targets_.push_back({"shoulder", shoulder_angle});
            joint_targets_.push_back({"elbow", elbow_angle});
            joint_targets_.push_back({"wrist", wrist_angle});
            joint_targets_.push_back({"finger_thumb", 0.0});  // 骨架无手指，放松
            joint_targets_.push_back({"finger_index", 0.0});
        }

        // IK 求解：关节目标 → 关节角向量（map_motion 已完成真实几何提取）
        std::vector<double> solve_ik(const std::vector<RobotJointTarget> &targets)
        {
            std::vector<double> angles;
            angles.reserve(targets.size());
            for (const auto &t : targets)
                angles.push_back(t.angle_rad);
            return angles;
        }

        // 真实数值 IK：从末端目标位置反解 2 连杆臂（上臂 l1 / 前臂 l2）关节角。
        // 返回 {shoulder_angle, elbow_relative_angle}；未收敛返回空。
        static std::vector<double> solve_arm_ik(double tx, double ty, double l1, double l2)
        {
            auto r = PlanarIKSolver::solve({l1, l2}, tx, ty);
            if (!r.converged || r.angles.size() < 2)
                return {};
            double shoulder = r.angles[0];
            double elbow_rel = r.angles[1] - r.angles[0]; // 绝对角 → 相对角
            return {shoulder, elbow_rel};
        }

        // 完整遥操作一步：capture → estimate → map → ik
        std::vector<double> teleop_step()
        {
            RgbFrame frame = capture();
            const Skeleton &skel = estimate_pose(frame);
            map_motion(skel);
            return solve_ik(joint_targets_);
        }
        
        const std::vector<RobotJointTarget> &joint_targets() const { return joint_targets_; }
    };
}
