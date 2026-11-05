#pragma once
#include <string>
#include <vector>
#include <queue>
#include <iostream>
#include "hardware/observability.hpp"

namespace rvdlrsp::circuit
{

    // ─────────────────────────────────────────────────────────────
    // 机器人电路设计
    //
    // 引脚级建模 + 连线,提供:
    //   - 引脚级连接(组件输出引脚 → 组件输入引脚)
    //   - 直接邻接 / 路径连通性分析(BFS)
    //   - 信号传播拓扑排序(Kahn 算法)
    //   - 悬空引脚检测(未连接的输入/输出)
    // ─────────────────────────────────────────────────────────────

    struct Pin
    {
        int id = -1;
        int component = -1; // 所属组件
        bool is_input = false;
    };

    struct ComponentNode
    {
        int id = -1;
        std::string type;
        std::vector<int> input_pins;  // 引脚 id 列表
        std::vector<int> output_pins;
    };

    struct Wire
    {
        int from_pin = -1; // 源引脚 id(输出)
        int to_pin = -1;   // 目标引脚 id(输入)
    };

    class RobotCircuit
    {
    private:
        std::vector<ComponentNode> nodes_;
        std::vector<Wire> wires_;
        std::vector<Pin> pins_; // 引脚 id == 数组下标(从 0 递增)

    public:
        // 添加组件,返回组件 id;input_pins/output_pins 为各自引脚数
        int add_component(const std::string &type, int input_pins, int output_pins)
        {
            ComponentNode n;
            n.id = static_cast<int>(nodes_.size());
            n.type = type;
            for (int i = 0; i < input_pins; ++i)
            {
                Pin p;
                p.id = static_cast<int>(pins_.size());
                p.component = n.id;
                p.is_input = true;
                n.input_pins.push_back(p.id);
                pins_.push_back(p);
            }
            for (int i = 0; i < output_pins; ++i)
            {
                Pin p;
                p.id = static_cast<int>(pins_.size());
                p.component = n.id;
                p.is_input = false;
                n.output_pins.push_back(p.id);
                pins_.push_back(p);
            }
            nodes_.push_back(n);
            return n.id;
        }

        // 连接:from 组件的第 from_pin_idx 个输出引脚 → to 组件的第 to_pin_idx 个输入引脚
        bool connect(int from_component, int from_pin_idx, int to_component, int to_pin_idx)
        {
            if (from_component < 0 || from_component >= static_cast<int>(nodes_.size()) ||
                to_component < 0 || to_component >= static_cast<int>(nodes_.size()))
                return false;
            const ComponentNode &src = nodes_[from_component];
            const ComponentNode &dst = nodes_[to_component];
            if (from_pin_idx < 0 || from_pin_idx >= static_cast<int>(src.output_pins.size()))
                return false;
            if (to_pin_idx < 0 || to_pin_idx >= static_cast<int>(dst.input_pins.size()))
                return false;
            wires_.push_back({src.output_pins[from_pin_idx], dst.input_pins[to_pin_idx]});
            return true;
        }

        // 直接邻接:a 的输出连到 b 的输入,或反之
        bool connected_directly(int a, int b) const
        {
            for (const auto &w : wires_)
            {
                int ca = pins_[w.from_pin].component;
                int cb = pins_[w.to_pin].component;
                if ((ca == a && cb == b) || (ca == b && cb == a))
                    return true;
            }
            return false;
        }

        // 路径连通性(BFS):a 与 b 是否通过若干连线连通
        bool connected(int a, int b) const
        {
            if (a == b)
                return true;
            int n = static_cast<int>(nodes_.size());
            if (a < 0 || a >= n || b < 0 || b >= n)
                return false;
            std::vector<bool> visited(n, false);
            std::queue<int> q;
            visited[a] = true;
            q.push(a);
            while (!q.empty())
            {
                int cur = q.front();
                q.pop();
                for (const auto &w : wires_)
                {
                    int ca = pins_[w.from_pin].component;
                    int cb = pins_[w.to_pin].component;
                    int nxt = -1;
                    if (ca == cur)
                        nxt = cb;
                    else if (cb == cur)
                        nxt = ca;
                    if (nxt >= 0 && !visited[nxt])
                    {
                        if (nxt == b)
                            return true;
                        visited[nxt] = true;
                        q.push(nxt);
                    }
                }
            }
            return false;
        }

        // 拓扑排序(Kahn):按信号传播方向(from → to)返回组件顺序。
        // 若存在环则结果长度 < 组件数。
        std::vector<int> topological_order() const
        {
            int n = static_cast<int>(nodes_.size());
            std::vector<int> indegree(n, 0);
            std::vector<std::vector<int>> adj(n);
            for (const auto &w : wires_)
            {
                int from = pins_[w.from_pin].component;
                int to = pins_[w.to_pin].component;
                if (from == to)
                    continue;
                adj[from].push_back(to);
                indegree[to]++;
            }
            std::queue<int> q;
            for (int i = 0; i < n; ++i)
                if (indegree[i] == 0)
                    q.push(i);
            std::vector<int> order;
            while (!q.empty())
            {
                int cur = q.front();
                q.pop();
                order.push_back(cur);
                for (int nxt : adj[cur])
                    if (--indegree[nxt] == 0)
                        q.push(nxt);
            }
            return order;
        }

        // 悬空引脚:未出现在任何连线中的输入/输出引脚 id
        std::vector<int> dangling_pins() const
        {
            std::vector<bool> used(pins_.size(), false);
            for (const auto &w : wires_)
            {
                used[w.from_pin] = true;
                used[w.to_pin] = true;
            }
            std::vector<int> dangling;
            for (const auto &p : pins_)
                if (!used[p.id])
                    dangling.push_back(p.id);
            return dangling;
        }

        size_t node_count() const { return nodes_.size(); }
        size_t wire_count() const { return wires_.size(); }
        const std::vector<ComponentNode> &nodes() const { return nodes_; }
        const std::vector<Wire> &wires() const { return wires_; }
    };
}
