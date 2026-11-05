#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cmath>
#include <stdexcept>
#include <iostream>

namespace vedaros::algorithm
{

    // 三维坐标变换（平移 + 单位四元数旋转）
    struct Transform
    {
        std::string parent_frame;
        std::string child_frame;
        std::array<double, 3> translation{0.0, 0.0, 0.0};
        std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0}; // x,y,z,w
    };

    // ── SE(3) 复合所需的四元数运算 ─────────────────────
    // 实现了SO(3)×R^3 复合

    // Hamilton 乘积
    // q = a ⊗ b（x,y,z,w 顺序）
    inline std::array<double, 4> quat_mul(const std::array<double, 4> &a,
                                          const std::array<double, 4> &b)
    {
        return {
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], // x
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0], // y
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], // z
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]  // w
        };
    }

    // 单位四元数旋转向量 v（Rodrigues 式）v' = q ⊗ v ⊗ q^{-1}
    inline std::array<double, 3> quat_rotate(const std::array<double, 4> &q,
                                             const std::array<double, 3> &v)
    {
        // qv = (x, y, z)，w = q[3]
        // v' = v + w·t + qv × t，t = 2·(qv × v)
        std::array<double, 3> qv{q[0], q[1], q[2]};
        double w = q[3];

        // t = 2 * cross(qv, v)
        std::array<double, 3> t{
            2.0 * (qv[1] * v[2] - qv[2] * v[1]),
            2.0 * (qv[2] * v[0] - qv[0] * v[2]),
            2.0 * (qv[0] * v[1] - qv[1] * v[0])};

        // v' = v + w*t + cross(qv, t)
        return {
            v[0] + w * t[0] + (qv[1] * t[2] - qv[2] * t[1]),
            v[1] + w * t[1] + (qv[2] * t[0] - qv[0] * t[2]),
            v[2] + w * t[2] + (qv[0] * t[1] - qv[1] * t[0])};
    }

    // 坐标变换树：维护帧间关系并提供查找
    class TfTree
    {
    private:
        std::unordered_map<std::string, Transform> edges_; // child -> transform

    public:
        void set_transform(const Transform &t)
        {
            edges_[t.child_frame] = t;
            std::cout << "[vedaRos.tf] Set transform '" << t.parent_frame
                      << "' -> '" << t.child_frame << "'.\n";
        }

        const Transform &lookup(const std::string &child) const
        {
            auto it = edges_.find(child);
            if (it == edges_.end())
                throw std::runtime_error("[vedaRos.tf] Unknown frame '" + child + "'.");
            return it->second;
        }

        // 沿父链复合，得到 world -> child 的完整 SE(3) 变换。
        // 正确的 SE(3) 复合：
        //   T_world→child = T_world→parent ⊗ T_parent→child
        //   R = R_wp ⊗ R_pc（四元数乘法）
        //   p = p_wp + R_wp · p_pc（旋转后的平移）
        Transform compose_to_root(const std::string &child) const
        {
            // 收集父链（child -> parent -> ... -> world），顺序为「近 → 远」
            std::vector<Transform> chain;
            std::string cur = child;
            while (true)
            {
                auto it = edges_.find(cur);
                if (it == edges_.end())
                    break;
                chain.push_back(it->second);
                cur = it->second.parent_frame;
                if (cur == "world")
                    break;
            }

            // 从 world 向下复合（反向遍历，最远帧先乘）
            Transform acc;
            acc.child_frame = child;
            acc.parent_frame = "world";
            acc.translation = {0.0, 0.0, 0.0};
            acc.rotation = {0.0, 0.0, 0.0, 1.0}; // 单位四元数

            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                // 平移：p_acc += R_acc · p_t（旋转后的平移）
                auto rot_p = quat_rotate(acc.rotation, it->translation);
                for (int i = 0; i < 3; ++i)
                    acc.translation[i] += rot_p[i];
                // 旋转：R_acc = R_acc ⊗ R_t
                acc.rotation = quat_mul(acc.rotation, it->rotation);
            }
            return acc;
        }

        size_t frame_count() const { return edges_.size(); }
    };
}
