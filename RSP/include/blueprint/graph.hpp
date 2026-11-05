#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <iostream>

namespace rvdlrsp::blueprint
{

    using NodeId = uint64_t;
    using PinId = uint64_t;

    enum class PinKind
    {
        Input,
        Output
    };

    struct Pin
    {
        PinId id = 0;
        std::string name;
        PinKind kind = PinKind::Input;
        std::string type; // 类型标签（如 "bool" / "float" / "vector3"）
    };

    struct Node
    {
        NodeId id = 0;
        std::string title;
        std::string category; // "behavior_tree" / "material"
        std::vector<Pin> inputs;
        std::vector<Pin> outputs;
        float x = 0.0f, y = 0.0f; // 画布坐标
    };

    struct Edge
    {
        PinId from = 0; // 输出引脚
        PinId to = 0;   // 输入引脚
    };

    class NodeGraph
    {
    private:
        std::vector<Node> nodes_;
        std::vector<Edge> edges_;
        NodeId next_node_ = 1;
        PinId next_pin_ = 1;

    public:
        NodeId add_node(const std::string &title, const std::string &category,
                        const std::vector<std::pair<std::string, std::string>> &inputs,
                        const std::vector<std::pair<std::string, std::string>> &outputs,
                        float x = 0, float y = 0)
        {
            Node n;
            n.id = next_node_++;
            n.title = title;
            n.category = category;
            n.x = x;
            n.y = y;
            for (const auto &p : inputs)
            {
                Pin pin;
                pin.id = next_pin_++;
                pin.name = p.first;
                pin.kind = PinKind::Input;
                pin.type = p.second;
                n.inputs.push_back(pin);
            }
            for (const auto &p : outputs)
            {
                Pin pin;
                pin.id = next_pin_++;
                pin.name = p.first;
                pin.kind = PinKind::Output;
                pin.type = p.second;
                n.outputs.push_back(pin);
            }
            nodes_.push_back(n);
            return n.id;
        }

        void connect(NodeId from_node, const std::string &from_pin,
                     NodeId to_node, const std::string &to_pin)
        {
            const Pin *src = nullptr;
            const Pin *dst = nullptr;
            for (const auto &n : nodes_)
            {
                if (n.id == from_node)
                    for (const auto &p : n.outputs)
                        if (p.name == from_pin)
                            src = &p;
                if (n.id == to_node)
                    for (const auto &p : n.inputs)
                        if (p.name == to_pin)
                            dst = &p;
            }
            if (src && dst)
                edges_.push_back({src->id, dst->id});
        }

        // 按引脚 ID 直接连接（编辑器用）
        void connect_pins(PinId from, PinId to)
        {
            const Pin *src = find_pin(from);
            const Pin *dst = find_pin(to);
            if (!src || !dst)
                return;
            if (src->kind != PinKind::Output || dst->kind != PinKind::Input)
                return;
            edges_.push_back({from, to});
        }

        const std::vector<Node> &nodes() const { return nodes_; }
        std::vector<Node> &nodes() { return nodes_; }
        const std::vector<Edge> &edges() const { return edges_; }

        const Pin *find_pin(PinId id) const
        {
            for (const auto &n : nodes_)
                for (const auto &p : n.inputs)
                    if (p.id == id)
                        return &p;
            for (const auto &n : nodes_)
                for (const auto &p : n.outputs)
                    if (p.id == id)
                        return &p;
            return nullptr;
        }

        Node *find_node(NodeId id)
        {
            for (auto &n : nodes_)
                if (n.id == id)
                    return &n;
            return nullptr;
        }

        const Node *find_node_of_pin(PinId id) const
        {
            for (const auto &n : nodes_)
            {
                for (const auto &p : n.inputs)
                    if (p.id == id)
                        return &n;
                for (const auto &p : n.outputs)
                    if (p.id == id)
                        return &n;
            }
            return nullptr;
        }

        size_t node_count() const { return nodes_.size(); }
        size_t edge_count() const { return edges_.size(); }
    };
}