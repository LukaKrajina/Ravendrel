#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <iostream>
#include "hardware/observability.hpp"
#include "camera.hpp"

namespace rvdlrsp::qcdrc
{

    struct Joint3D
    {
        std::string name;
        double x = 0, y = 0, z = 0;
    };

    struct Skeleton
    {
        std::vector<Joint3D> joints;
    };

    class MotionCapture
    {
    private:
        Skeleton skeleton_;
        std::vector<double> base_x_, base_y_, base_z_;

        static Skeleton make_tpose()
        {
            Skeleton s;
            s.joints = {
                {"pelvis", 0.0, 0.9, 0.0},
                {"spine", 0.0, 1.1, 0.0},
                {"neck", 0.0, 1.4, 0.0},
                {"head", 0.0, 1.6, 0.0},
                {"l_shoulder", -0.3, 1.35, 0.0},
                {"l_elbow", -0.5, 1.15, 0.0},
                {"l_wrist", -0.7, 0.95, 0.0},
                {"r_shoulder", 0.3, 1.35, 0.0},
                {"r_elbow", 0.5, 1.15, 0.0},
                {"r_wrist", 0.7, 0.95, 0.0},
                {"l_hip", -0.15, 0.9, 0.0},
                {"l_knee", -0.15, 0.5, 0.0},
                {"l_ankle", -0.15, 0.1, 0.0},
                {"r_hip", 0.15, 0.9, 0.0},
                {"r_knee", 0.15, 0.5, 0.0},
                {"r_ankle", 0.15, 0.1, 0.0},
            };
            return s;
        }

    public:
        MotionCapture() : skeleton_(make_tpose())
        {
            for (const auto &j : skeleton_.joints)
            {
                base_x_.push_back(j.x);
                base_y_.push_back(j.y);
                base_z_.push_back(j.z);
            }
            RVDLRSP_INFO("qcdrc") << "Motion capture (full-body) online.";
        }

        const Skeleton &estimate(const struct RgbFrame &frame)
        {
            size_t seed = static_cast<size_t>(frame.timestamp_us);
            for (size_t i = 0; i < skeleton_.joints.size(); ++i)
            {
                double s = std::sin(static_cast<double>(seed + i) * 0.6180339887);
                double jitter = s * 0.02;
                skeleton_.joints[i].x = base_x_[i] + jitter;
                skeleton_.joints[i].y = base_y_[i] + jitter * 0.5;
                skeleton_.joints[i].z = base_z_[i];
            }
            return skeleton_;
        }

        void set_skeleton(const Skeleton &s) { skeleton_ = s; }
        const Skeleton &skeleton() const { return skeleton_; }
    };
}