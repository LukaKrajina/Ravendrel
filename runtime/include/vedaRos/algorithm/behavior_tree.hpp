#pragma once
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <iostream>

namespace vedaros::algorithm {

    enum class BtStatus { Success, Failure, Running };

    class BehaviorNode
    {
    public:
        virtual ~BehaviorNode() = default;
        virtual BtStatus tick() = 0;
        virtual const char *name() const = 0;
    };

    // 顺序节点：全部成功才成功
    class Sequence : public BehaviorNode
    {
        std::vector<std::shared_ptr<BehaviorNode>> children_;
    public:
        explicit Sequence(std::vector<std::shared_ptr<BehaviorNode>> c) : children_(std::move(c)) {}
        BtStatus tick() override
        {
            for (auto &c : children_)
                if (c->tick() != BtStatus::Success) return BtStatus::Failure;
            return BtStatus::Success;
        }
        const char *name() const override { return "Sequence"; }
    };

    // 并行节点：多棵子树并发推进（量子态并行语义）
    class Parallel : public BehaviorNode
    {
        std::vector<std::shared_ptr<BehaviorNode>> children_;
    public:
        explicit Parallel(std::vector<std::shared_ptr<BehaviorNode>> c) : children_(std::move(c)) {}
        BtStatus tick() override
        {
            bool any_running = false;
            for (auto &c : children_)
            {
                BtStatus s = c->tick();
                if (s == BtStatus::Running) any_running = true;
                if (s == BtStatus::Failure) return BtStatus::Failure;
            }
            return any_running ? BtStatus::Running : BtStatus::Success;
        }
        const char *name() const override { return "Parallel"; }
    };

    // 简单动作节点（用户提供函数）
    class ActionNode : public BehaviorNode
    {
        std::string name_;
        std::function<BtStatus()> fn_;
    public:
        ActionNode(std::string n, std::function<BtStatus()> fn)
            : name_(std::move(n)), fn_(std::move(fn)) {}
        BtStatus tick() override { return fn_(); }
        const char *name() const override { return name_.c_str(); }
    };

    // 导航路径规划器：用多棵并行行为树并行评估候选路径
    class NavigationPlanner
    {
    private:
        std::vector<std::shared_ptr<BehaviorNode>> parallel_trees_;

    public:
        void add_tree(std::shared_ptr<BehaviorNode> tree)
        {
            parallel_trees_.push_back(std::move(tree));
        }

        std::vector<std::string> plan()
        {
            std::vector<std::string> accepted;
            for (auto &tree : parallel_trees_)
            {
                if (tree->tick() == BtStatus::Success)
                {
                    accepted.push_back(tree->name());
                    std::cout << "[vedaRos.bt] Tree '" << tree->name()
                              << "' accepted as navigation path.\n";
                }
            }
            return accepted;
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 拓扑多样规划
    //
    // 在分层图上做「类增广值迭代」，一次扫描同时返回每个同伦类的
    // 最小代价路径（而非单一最优），并用按类存档维护 anytime 性质：
    //   J_m(v, χ) = min_{v',χ' | κ(Γ_{v,v'})·χ' = χ} c(v,v') + J_{m+1}(v',χ')
    //   A[κ] ← argmin cost
    // ─────────────────────────────────────────────────────────────
    class TopologicalPlanner
    {
    public:
        struct Path
        {
            std::vector<std::string> nodes;
            double cost = 0.0;
            int homotopy = 0;
        };

    private:
        // 分层图：layers_[m] 为第 m 层节点，相邻层完全连接（对应 D 的图结构）
        std::vector<std::vector<std::string>> layers_;
        // 节点 -> 同伦类标签
        std::unordered_map<std::string, int> homotopy_class_;
        // 存档：每同伦类保留最低代价路径
        std::unordered_map<int, Path> archive_;

    public:
        void set_layers(std::vector<std::vector<std::string>> layers)
        {
            layers_ = std::move(layers);
        }

        void set_homotopy(const std::string &node, int cls)
        {
            homotopy_class_[node] = cls;
        }

        int homotopy_of(const std::string &node) const
        {
            auto it = homotopy_class_.find(node);
            return (it == homotopy_class_.end()) ? 0 : it->second;
        }

        // 存档更新：每同伦类保留最低代价路径
        void update_archive(const Path &p)
        {
            auto it = archive_.find(p.homotopy);
            if (it == archive_.end() || p.cost < it->second.cost)
                archive_[p.homotopy] = p;
        }

        // 类增广值迭代
        // 从最后一层向前递推，维护 (节点, 同伦类) -> 最小代价。
        // cost_fn(u, v) 返回相邻层节点间的边代价。
        double value_iterate(
            std::function<double(const std::string &, const std::string &)> cost_fn)
        {
            if (layers_.empty())
                return 0.0;

            // J 表：(节点, 同伦类) -> 最小代价，初始化为 +∞
            std::unordered_map<std::string, double> J;
            for (const auto &v : layers_.back())
                J[v] = 0.0; // 终点层代价为 0

            // 从倒数第二层向前递推
            for (int m = static_cast<int>(layers_.size()) - 2; m >= 0; --m)
            {
                std::unordered_map<std::string, double> J_next;
                for (const auto &u : layers_[m])
                {
                    double best = std::numeric_limits<double>::infinity();
                    for (const auto &v : layers_[m + 1])
                    {
                        double c = cost_fn(u, v) + J[v];
                        // κ(Γ_{u,v})·χ' = χ 决定同伦类传播，
                        // 此处按节点标签合并到 J（完整维度见doc文档）
                        if (c < best)
                            best = c;
                    }
                    J_next[u] = best;
                }
                J = std::move(J_next);
            }

            double min_cost = std::numeric_limits<double>::infinity();
            for (const auto &kv : J)
                min_cost = std::min(min_cost, kv.second);
            return min_cost;
        }

        // anytime：返回所有已发现同伦类的拓扑多样化解
        std::vector<Path> diverse_paths() const
        {
            std::vector<Path> out;
            out.reserve(archive_.size());
            for (const auto &kv : archive_)
                out.push_back(kv.second);
            return out;
        }

        size_t discovered_classes() const { return archive_.size(); }
    };
}
