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

        RgbFrame capture() { return camera_.capture(); }

        const Skeleton &estimate_pose(const RgbFrame &frame)
        {
            return mocap_.estimate(frame);
        }

        void map_motion(const Skeleton &skel)
        {
            joint_targets_.clear();

            const Joint3D *neck = find_joint(skel, "neck");
            const Joint3D *pelvis = find_joint(skel, "pelvis");
            const Joint3D *shoulder = find_joint(skel, "r_shoulder");
            const Joint3D *elbow = find_joint(skel, "r_elbow");
            const Joint3D *wrist = find_joint(skel, "r_wrist");

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
                elbow_angle = elbow_flexion(*shoulder, *elbow, *wrist);
                if (neck && pelvis)
                    shoulder_angle = shoulder_lift(*shoulder, *elbow, *neck, *pelvis);
                double fx = wrist->x - elbow->x;
                double fy = wrist->y - elbow->y;
                double fz = wrist->z - elbow->z;
                wrist_angle = std::atan2(std::sqrt(fx * fx + fz * fz), fy);
            }

            joint_targets_.push_back({"shoulder", shoulder_angle});
            joint_targets_.push_back({"elbow", elbow_angle});
            joint_targets_.push_back({"wrist", wrist_angle});
            joint_targets_.push_back({"finger_thumb", 0.0});
            joint_targets_.push_back({"finger_index", 0.0});
        }

        std::vector<double> solve_ik(const std::vector<RobotJointTarget> &targets)
        {
            std::vector<double> angles;
            angles.reserve(targets.size());
            for (const auto &t : targets)
                angles.push_back(t.angle_rad);
            return angles;
        }

        static std::vector<double> solve_arm_ik(double tx, double ty, double l1, double l2)
        {
            auto r = PlanarIKSolver::solve({l1, l2}, tx, ty);
            if (!r.converged || r.angles.size() < 2)
                return {};
            double shoulder = r.angles[0];
            double elbow_rel = r.angles[1] - r.angles[0];
            return {shoulder, elbow_rel};
        }

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