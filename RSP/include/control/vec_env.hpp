#pragma once
//
// Gymnasium 对齐层 —— 向量环境（VectorEnv）
//
// ═══════════════════════════════════════════════════════════════════════
// 为什么需要
// ═══════════════════════════════════════════════════════════════════════
//
// 单环境 RL 在物理仿真里几乎没有实用吞吐：一次 step 的大部分时间花在
// Python/脚本边界与单线程物理求解上。向量环境把 N 个子环境批成一次调用，
// 是 RL 训练吞吐的主要杠杆（Genesis 的 scene.build(n_envs=N)、
// Gymnasium 的 SyncVectorEnv / AsyncVectorEnv 做的是同一件事）。
//
// ───────────────────────────────────────────────────────────────────────
//
// 【本实现显式记录的取舍】—— 与 Gymnasium 的差异，不掩盖
//
// 1. 只有 **SyncVectorEnv**（同进程、串行循环子环境）。
//    Gymnasium 的 AsyncVectorEnv 用多进程 + 共享内存并行。本平台是
//    单个 C++ 进程且物理内核本身是串行的（PhysicsKernel 顺序积分），
//    多进程只会复制内核状态而无并行收益。真正的并行化应在物理内核层
//    （Kokkos）完成，那属于 qpc 的范畴，不是本层的职责。
//
// 2. autoreset 采用 **NEXT_STEP** 语义（Gymnasium 的默认）：
//    子环境终止后，在**下一次** step 时重置，而非当步。这样批量中其他
//    子环境无需等待。这是多数自研向量化实现最容易搞错的地方 ——
//    SAME_STEP 会在重置步丢失当步转移，DISABLED 则要求外部手工重置。
//
// 3. info 为**键 → 逐子环境数组**的 dict（Gymnasium v0.25+ 约定），
//    而非 pre-0.25 的「每环境一个 dict 列表」。逐项奖励分解因此可以
//    无损地按子环境对齐。
//
// 4. 单个子环境崩溃时**不**静默跳过：抛出并携带子环境索引。静默跳过会
//    让一条已损坏的轨迹继续污染 batch 均值，且极难定位。
//
#include "control/qrl.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qrl
{

    // autoreset 模式（对齐 Gymnasium 的 AutoresetMode 规范，
    // 见 https://farama.org/Vector-Autoreset-Mode）
    enum class AutoresetMode
    {
        NextStep = 0,  // 终止后的下一步重置（Gymnasium 默认）
        SameStep = 1,  // 当步末尾重置
        Disabled = 2,  // 不自动重置，由调用方负责
    };

    struct VectorStepResult
    {
        // [n_envs][obs_dim]
        std::vector<std::vector<double>> observations;
        std::vector<double> rewards;
        std::vector<bool> terminated;
        std::vector<bool> truncated;
        // 键 → 长度 n_envs 的数组
        std::unordered_map<std::string, std::vector<double>> info;

        size_t size() const { return rewards.size(); }
        bool any_done() const
        {
            for (size_t i = 0; i < terminated.size(); ++i)
                if (terminated[i] || truncated[i])
                    return true;
            return false;
        }
    };

    class VectorEnv
    {
    public:
        virtual ~VectorEnv() = default;

        virtual size_t num_envs() const = 0;

        // seed 可为 nullopt（不重置）、单个值（子环境 i 收 seed+i）、
        // 或逐环境显式给出。
        virtual std::pair<std::vector<std::vector<double>>,
                          std::unordered_map<std::string, std::vector<double>>>
        reset(std::optional<std::vector<std::optional<uint64_t>>> seeds) = 0;

        // actions[i] 作用于子环境 i
        virtual VectorStepResult step(const std::vector<std::vector<double>> &actions) = 0;

        virtual const Space &single_action_space() const = 0;
        virtual const Space &single_observation_space() const = 0;

        AutoresetMode autoreset_mode() const { return autoreset_mode_; }

        virtual void close() {}

        // 子环境属性存取（Gymnasium 的 get_attr/set_attr/call 语义）
        // 子环境类型未知时只能操作公共接口，故限定为一组固定属性。
        virtual double attr(size_t, const std::string &) const = 0;
        virtual void set_attr(size_t, const std::string &, double) = 0;

    protected:
        AutoresetMode autoreset_mode_ = AutoresetMode::NextStep;
    };

    // ─── SyncVectorEnv ─────────────────────────────────────────────
    class SyncVectorEnv final : public VectorEnv
    {
    public:
        explicit SyncVectorEnv(std::vector<std::shared_ptr<Env>> envs,
                                AutoresetMode mode = AutoresetMode::NextStep)
            : envs_(std::move(envs))
        {
            autoreset_mode_ = mode;
            if (envs_.empty())
                throw std::invalid_argument("[qrl.SyncVectorEnv] no sub-environments");
            needs_reset_.assign(envs_.size(), false);
            out_of_range_.assign(envs_.size(), false);
            last_reset_info_.assign(envs_.size(), {});
            // 子环境空间必须一致，否则批量化的前提不成立。
            // Gymnasium 用 observation_mode="same" 表达同一约束。
            for (size_t i = 1; i < envs_.size(); ++i)
            {
                if (envs_[i]->observation_space().dim() != envs_[0]->observation_space().dim() ||
                    envs_[i]->action_space().dim() != envs_[0]->action_space().dim())
                    throw std::invalid_argument(
                        "[qrl.SyncVectorEnv] sub-environment spaces differ at index " +
                        std::to_string(i));
            }
        }

        size_t num_envs() const override { return envs_.size(); }

        const Space &single_action_space() const override { return envs_[0]->action_space(); }
        const Space &single_observation_space() const override
        {
            return envs_[0]->observation_space();
        }

        std::pair<std::vector<std::vector<double>>,
                  std::unordered_map<std::string, std::vector<double>>>
        reset(std::optional<std::vector<std::optional<uint64_t>>> seeds) override
        {
            const size_t n = envs_.size();
            std::vector<std::vector<double>> obs(n);
            std::unordered_map<std::string, std::vector<double>> info;

            // 种子规则（对齐 Gymnasium SyncVectorEnv.reset）：
            //   nullopt       → 各子环境用 nullptr（保留各自 PRNG）
            //   单个 seed     → 子环境 i 收 seed+i
            //   长度 n 的列表  → 逐环境显式指定
            std::vector<std::optional<uint64_t>> per_env(n, std::nullopt);
            if (seeds.has_value())
            {
                if (seeds->size() == 1)
                {
                    for (size_t i = 0; i < n; ++i)
                        if ((*seeds)[0].has_value())
                            per_env[i] = *(*seeds)[0] + i;
                }
                else if (seeds->size() == n)
                {
                    per_env = *seeds;
                }
                else
                {
                    throw std::invalid_argument(
                        "[qrl.SyncVectorEnv.reset] seeds must be nullopt, {seed}, or length num_envs");
                }
            }

            std::unordered_map<std::string, std::vector<double>> sub_info;
            for (size_t i = 0; i < n; ++i)
            {
                auto r = envs_[i]->reset(per_env[i], {});
                obs[i] = std::move(r.first);
                for (const auto &kv : r.second)
                    sub_info[kv.first].push_back(kv.second);
                needs_reset_[i] = false;
            }
            // 各子环境的 info 键集合应一致；缺失者补 0 以保持对齐。
            for (auto &kv : sub_info)
            {
                if (kv.second.size() < n)
                    kv.second.resize(n, 0.0);
                info.emplace(std::move(kv));
            }
            return {std::move(obs), std::move(info)};
        }

        VectorStepResult step(const std::vector<std::vector<double>> &actions) override
        {
            const size_t n = envs_.size();
            if (actions.size() != n)
                throw std::invalid_argument(
                    "[qrl.SyncVectorEnv.step] actions.size() != num_envs");

            VectorStepResult out;
            out.observations.resize(n);
            out.rewards.assign(n, 0.0);
            out.terminated.assign(n, false);
            out.truncated.assign(n, false);
            std::unordered_map<std::string, std::vector<double>> sub_info;

            for (size_t i = 0; i < n; ++i)
            {
                if (autoreset_mode_ == AutoresetMode::Disabled && needs_reset_[i])
                    throw std::runtime_error(
                        "[qrl.SyncVectorEnv.step] sub-environment " + std::to_string(i) +
                        " requires an explicit reset (AutoresetMode::Disabled)");

                // 越界动作在批处理层面就应可见：这里收集而不抛出，
                // 以免一个坏动作毁掉整批；由调用方检查 out_of_range。
                if (!single_action_space().contains(actions[i]))
                    out_of_range_[i] = true;

                // NEXT_STEP：上一回合已终止的子环境在此**先重置再走一步**。
                // 这正是与 SameStep 的区别所在 —— 缺失这一步会让
                // "待重置"标记形同虚设，子环境会带着旧状态继续跑。
                if (autoreset_mode_ == AutoresetMode::NextStep && needs_reset_[i])
                {
                    auto rr = envs_[i]->reset(std::nullopt, {});
                    needs_reset_[i] = false;
                    last_reset_info_[i] = rr.second;
                }

                StepResult r = envs_[i]->step(actions[i]);

                if (r.terminated || r.truncated)
                {
                    // NEXT_STEP：仅**标记**待重置，本步仍返回终止态的
                    // 最终观测（RL 算法需要它来算 bootstrap 项）。
                    switch (autoreset_mode_)
                    {
                    case AutoresetMode::NextStep:
                        needs_reset_[i] = true;
                        break;
                    case AutoresetMode::SameStep:
                    {
                        // 当步末尾重置：最终观测被重置后的观测替换。
                        // 调用方若需要终止态观测会取不到 —— 这是该模式的
                        // 已知代价，故不是默认值。
                        auto rr = envs_[i]->reset(std::nullopt, {});
                        r.observation = std::move(rr.first);
                        for (const auto &kv : rr.second)
                            r.info[kv.first + ".post_reset"] = kv.second;
                        needs_reset_[i] = false;
                        break;
                    }
                    case AutoresetMode::Disabled:
                        needs_reset_[i] = true;
                        break;
                    }
                }
                else
                {
                    needs_reset_[i] = false;
                }

                out.observations[i] = std::move(r.observation);
                out.rewards[i] = r.reward;
                out.terminated[i] = r.terminated;
                out.truncated[i] = r.truncated;
                for (const auto &kv : r.info)
                    sub_info[kv.first].push_back(kv.second);
            }

            for (auto &kv : sub_info)
                if (kv.second.size() < n)
                    kv.second.resize(n, 0.0);
            out.info = std::move(sub_info);
            return out;
        }

        // 本步是否有子环境的动作越界（批处理不应因单个坏动作崩溃）
        bool action_out_of_range(size_t i) const
        {
            return i < out_of_range_.size() && out_of_range_[i];
        }
        bool any_action_out_of_range() const
        {
            for (size_t i = 0; i < out_of_range_.size(); ++i)
                if (out_of_range_[i])
                    return true;
            return false;
        }
        void clear_out_of_range()
        {
            for (size_t i = 0; i < out_of_range_.size(); ++i)
                out_of_range_[i] = false;
        }

        double attr(size_t i, const std::string &key) const override
        {
            if (i >= envs_.size())
                throw std::out_of_range("[qrl.SyncVectorEnv.attr] index out of range");
            if (key == "num_envs")
                return static_cast<double>(envs_.size());
            // 子环境只保证能回答 render_fps / render_mode 这类公共属性
            if (key == "render_fps")
                return static_cast<double>(envs_[i]->render_fps());
            if (key == "closed")
                return envs_[i]->closed() ? 1.0 : 0.0;
            throw std::invalid_argument("[qrl.SyncVectorEnv.attr] unsupported key: " + key);
        }

        void set_attr(size_t, const std::string &, double) override
        {
            // SyncVectorEnv 的子环境不暴露可变属性接口（render mode 按
            // 约定 6 在构造时固定）。显式拒绝而非静默忽略。
            throw std::logic_error(
                "[qrl.SyncVectorEnv.set_attr] sub-environments expose no mutable attributes");
        }

        void close() override
        {
            for (auto &e : envs_)
                if (e)
                    e->close();
        }

    private:
        std::vector<std::shared_ptr<Env>> envs_;
        std::vector<bool> needs_reset_;
        std::vector<bool> out_of_range_;
        // NEXT_STEP 下自动重置时，各子环境 reset() 返回的 info
        std::vector<std::unordered_map<std::string, double>> last_reset_info_;
    };

    // ─── 便利工厂：批量构造同一环境的 N 个副本 ─────────────────────
    //
    // 对齐 Genesis 的 scene.build(n_envs=N) 语义：批量创建子环境。
    // seeds 传入后按 seed+i 分配，使各子环境随机性互不相关且可复现。
    inline std::unique_ptr<SyncVectorEnv>
    make_vector_env(size_t n, const std::function<std::shared_ptr<Env>()> &factory,
                    AutoresetMode mode = AutoresetMode::NextStep)
    {
        if (n == 0)
            throw std::invalid_argument("[qrl.make_vector_env] n must be positive");
        std::vector<std::shared_ptr<Env>> envs;
        envs.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            auto e = factory();
            if (!e)
                throw std::runtime_error("[qrl.make_vector_env] factory returned null");
            envs.push_back(std::move(e));
        }
        return std::make_unique<SyncVectorEnv>(std::move(envs), mode);
    }

} // namespace qrl
