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

    struct Transform
    {
        std::string parent_frame;
        std::string child_frame;
        std::array<double, 3> translation{0.0, 0.0, 0.0};
        std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
    };


    inline std::array<double, 4> quat_mul(const std::array<double, 4> &a,
                                          const std::array<double, 4> &b)
    {
        return {
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]
        };
    }

    inline std::array<double, 3> quat_rotate(const std::array<double, 4> &q,
                                             const std::array<double, 3> &v)
    {
        std::array<double, 3> qv{q[0], q[1], q[2]};
        double w = q[3];

        std::array<double, 3> t{
            2.0 * (qv[1] * v[2] - qv[2] * v[1]),
            2.0 * (qv[2] * v[0] - qv[0] * v[2]),
            2.0 * (qv[0] * v[1] - qv[1] * v[0])};

        return {
            v[0] + w * t[0] + (qv[1] * t[2] - qv[2] * t[1]),
            v[1] + w * t[1] + (qv[2] * t[0] - qv[0] * t[2]),
            v[2] + w * t[2] + (qv[0] * t[1] - qv[1] * t[0])};
    }

    class TfTree
    {
    private:
        std::unordered_map<std::string, Transform> edges_;

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

        Transform compose_to_root(const std::string &child) const
        {
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

            Transform acc;
            acc.child_frame = child;
            acc.parent_frame = "world";
            acc.translation = {0.0, 0.0, 0.0};
            acc.rotation = {0.0, 0.0, 0.0, 1.0};

            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                auto rot_p = quat_rotate(acc.rotation, it->translation);
                for (int i = 0; i < 3; ++i)
                    acc.translation[i] += rot_p[i];
                acc.rotation = quat_mul(acc.rotation, it->rotation);
            }
            return acc;
        }

        size_t frame_count() const { return edges_.size(); }
    };
}