#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <iostream>
#include "graph.hpp"

namespace rvdlrsp::blueprint
{

    // ─── 行为树蓝图（Sequence / Selector / Action / Condition）────────
    enum class BTStatus
    {
        Success,
        Failure,
        Running
    };

    struct BTNode
    {
        std::string type; // Sequence / Selector / Action
        std::function<BTStatus()> action;
        std::vector<std::shared_ptr<BTNode>> children;
        BTStatus tick()
        {
            if (action)
                return action();
            for (auto &c : children)
                if (c->tick() == BTStatus::Success)
                    return BTStatus::Success;
            return BTStatus::Failure;
        }
    };

    // 封装 NodeGraph，提供语义化的节点创建
    class BehaviorTreeBlueprint
    {
    private:
        NodeGraph graph_;

    public:
        // 序列节点（输入：前置条件；输出：执行流）
        NodeId add_sequence(float x = 0, float y = 0)
        {
            return graph_.add_node("Sequence", "behavior_tree",
                                   {{"enter", "exec"}}, {{"success", "exec"}, {"failure", "exec"}}, x, y);
        }

        NodeId add_selector(float x = 0, float y = 0)
        {
            return graph_.add_node("Selector", "behavior_tree",
                                   {{"enter", "exec"}}, {{"out", "exec"}}, x, y);
        }

        NodeId add_action(const std::string &name, float x = 0, float y = 0)
        {
            return graph_.add_node("Action: " + name, "behavior_tree",
                                   {{"start", "exec"}}, {{"done", "exec"}, {"running", "exec"}}, x, y);
        }

        NodeId add_condition(const std::string &name, float x = 0, float y = 0)
        {
            return graph_.add_node("Condition: " + name, "behavior_tree",
                                   {{"check", "exec"}}, {{"true", "bool"}, {"false", "bool"}}, x, y);
        }

        void link(NodeId from, const std::string &from_pin, NodeId to, const std::string &to_pin)
        {
            graph_.connect(from, from_pin, to, to_pin);
        }

        NodeGraph &graph() { return graph_; }
        const NodeGraph &graph() const { return graph_; }
    };

    // ─── Material / Substrate 蓝图 ────────────────────────────
    // Substrate 节点（纹理/常数/噪声），Material 节点（PBR 参数）
    class MaterialBlueprint
    {
    private:
        NodeGraph graph_;

    public:
        NodeId add_substrate(const std::string &name, float x = 0, float y = 0)
        {
            return graph_.add_node("Substrate: " + name, "material",
                                   {}, {{"color", "float3"}, {"roughness", "float"}}, x, y);
        }

        NodeId add_material(const std::string &name, float x = 0, float y = 0)
        {
            return graph_.add_node("Material: " + name, "material",
                                   {{"base_color", "float3"}, {"metallic", "float"}, {"roughness", "float"}},
                                   {{"shader", "material"}}, x, y);
        }

        NodeId add_constant(float x = 0, float y = 0)
        {
            return graph_.add_node("Constant", "material",
                                   {}, {{"value", "float"}}, x, y);
        }

        void link(NodeId from, const std::string &from_pin, NodeId to, const std::string &to_pin)
        {
            graph_.connect(from, from_pin, to, to_pin);
        }

        NodeGraph &graph() { return graph_; }
        const NodeGraph &graph() const { return graph_; }
    };

}