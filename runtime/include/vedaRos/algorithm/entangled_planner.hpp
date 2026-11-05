#pragma once
#include <memory>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>
#include "../../qhal/IQuantumBackend.hpp"
#include "../../../src/QObject.hpp"

namespace vedaros::algorithm
{
    // ⚠ BellState 必须写成 **::rvdl::BellState**（全局命名空间）。
    //
    //   本文件在 namespace vedaros::algorithm 里，而 vedaRos 还有一个
    //   **同名**的嵌套命名空间 vedaros::rvdl（core/rvdl_lang.hpp，vedaRos 的
    //   rvdl 语言构件）—— 它**遮蔽**了全局的 ::rvdl。
    //
    //   不加限定时 `rvdl::BellState` 解析到 vedaros::rvdl，而那里没有这个
    //   类型 → 编译报 "namespace \"vedaros::rvdl\" has no member \"BellState\""，
    //   整个 rvdlRSP 子项目编不出来。
    //
    //   这里的 BellState 是量子对象（runtime/src/QObject.hpp 的 ::rvdl::BellState），
    //   不是 vedaRos 的语言构件 —— 所以限定到全局是对的。

    // 用纠缠的 BellState 表示"意识态"，驱动运动规划与操作
    class EntangledPlanner
    {
    private:
        qhal::IQuantumBackend *backend_;

        // 长度惩罚：R_len = clip(log2(clip(n,1,T)/T), -1, 0)
        // 用于防止「奖励捷径」（重复冗长决策反而得分更高）。
        static double length_penalty(const std::vector<int> &decisions, size_t T = 80)
        {
            double n = static_cast<double>(decisions.size());
            double clipped = std::max(1.0, std::min(n, static_cast<double>(T)));
            double val = std::log2(clipped / static_cast<double>(T));
            return std::max(-1.0, std::min(0.0, val));
        }

    public:
        explicit EntangledPlanner(qhal::IQuantumBackend *backend) : backend_(backend)
        {
            std::cout << "[vedaRos.plan] Fully-entangled consciousness planner online.\n";
        }

        // 生成一条"意识纠缠"的运动路径：每个决策点对应一个 BellState
        std::vector<std::shared_ptr<::rvdl::BellState>> entangle_consciousness(size_t num_decision_points)
        {
            std::vector<std::shared_ptr<::rvdl::BellState>> consciousness;
            consciousness.reserve(num_decision_points);
            for (size_t i = 0; i < num_decision_points; ++i)
            {
                consciousness.push_back(std::make_shared<::rvdl::BellState>(backend_));
            }
            return consciousness;
        }

        // 依据意识态测量结果，规划运动目标序列（向后兼容的贪心单采样）
        std::vector<int> plan_motion(
            const std::vector<std::shared_ptr<::rvdl::BellState>> &consciousness)
        {
            std::vector<int> goals;
            goals.reserve(consciousness.size());
            for (auto &bell : consciousness)
            {
                auto bits = bell->measure();
                int decision = (bits.empty() ? 0 : bits[0]);
                goals.push_back(decision);
            }
            std::cout << "[vedaRos.plan] Motion planned from " << goals.size()
                      << " entangled decisions.\n";
            return goals;
        }

        // ── K 次重放重采样 + 组相对优势决策 ─────────────
        // 每个决策点重放电路 K 次得到 K 条候选意识轨迹（独立采样自同一
        // 分布 P(θ)），用组内标准化优势 A = R - mean_k(R) 选出最优，
        // 替代「取 bits[0]」的贪心单采样。
        //
        // reward_fn：语义/精确匹配奖励（对应论文 C 的 R_acc），可注入；
        //            缺省为 0（仅靠长度惩罚），实际应用时应提供。
        std::vector<int> plan_motion_grpo(
            const std::vector<std::shared_ptr<::rvdl::BellState>> &consciousness,
            size_t K = 8,
            std::function<double(const std::vector<int> &)> reward_fn = nullptr)
        {
            // K 次重放重采样（新公式 14）
            std::vector<std::vector<int>> candidates(K);
            for (auto &bell : consciousness)
            {
                for (size_t k = 0; k < K; ++k)
                {
                    bell->reset_to_ground_state();   // 重放 BellState 纠缠
                    auto bits = bell->measure();
                    candidates[k].push_back(bits.empty() ? 0 : bits[0]);
                }
            }

            // 奖励 + 长度惩罚（R = R_acc + R_len）
            std::vector<double> R(K);
            double mean = 0.0;
            for (size_t k = 0; k < K; ++k)
            {
                double acc = reward_fn ? reward_fn(candidates[k]) : 0.0;
                R[k] = acc + length_penalty(candidates[k]);
                mean += R[k];
            }
            mean /= K;

            // 组相对优势：A^(k) = R^(k) - mean(R)，取最大优势
            size_t best = 0;
            double best_adv = R[0] - mean;
            for (size_t k = 1; k < K; ++k)
            {
                double adv = R[k] - mean;
                if (adv > best_adv)
                {
                    best_adv = adv;
                    best = k;
                }
            }

            std::cout << "[vedaRos.plan] GRPO motion planned from " << K
                      << " resampled trajectories (best advantage " << best_adv << ").\n";
            return candidates[best];
        }
    };
}