#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include "qpc/math.hpp"
#include "qpc/rigid_body.hpp"
#include "teleop.hpp"

namespace rvdlrsp::qcdrc
{

    class TeleopDriver
    {
    public:
        struct Config
        {
            double kp = 12.0;         // 比例增益
            double kd = 2.5;          // 阻尼
            double drive_scale = 3.0; // 关节角 → 目标位置缩放
            double max_target = 6.0;  // 目标位置范围
        };

        // 关节角 → 目标位置
        // 简化映射：pelvis（索引 0）驱动 X，head（索引 3）驱动 Z（见 mocap 骨架顺序）
        static qpc::Vec3 joint_to_target(const std::vector<double> &angles, const Config &cfg)
        {
            Config c = cfg;
            double tx = 0.0, tz = 0.0;
            if (!angles.empty())
                tx = std::tan(angles[0]) * c.drive_scale;
            if (angles.size() > 3)
                tz = std::tan(angles[3]) * c.drive_scale;

            tx = std::max(-c.max_target, std::min(c.max_target, tx));
            tz = std::max(-c.max_target, std::min(c.max_target, tz));
            return {tx, 0.5, tz}; // 保持机器人在地面上方
        }

        // PD 控制：由当前位置与目标位置计算驱动力
        static qpc::Vec3 compute_force(const qpc::RigidBody &body, const qpc::Vec3 &target,
                                       const Config &cfg)
        {
            qpc::Vec3 error = target - body.position;
            return error * cfg.kp - body.linear_velocity * cfg.kd;
        }

        // 无配置重载
        static qpc::Vec3 joint_to_target(const std::vector<double> &angles)
        {
            return joint_to_target(angles, Config{});
        }
        static qpc::Vec3 compute_force(const qpc::RigidBody &body, const qpc::Vec3 &target)
        {
            return compute_force(body, target, Config{});
        }
    };
}
