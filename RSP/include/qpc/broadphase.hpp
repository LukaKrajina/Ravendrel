#pragma once
#include <vector>
#include <utility>
#include <algorithm>
#include "math.hpp"

namespace rvdlrsp::qpc
{

    // ─────────────────────────────────────────────────────────────
    // 宽相碰撞检测：BVH（层次包围盒树）
    //
    // 把 O(n²) 的全对全碰撞检测降到 O(n log n)
    // 引擎的必备组件。自顶向下构建，按最长轴质心划分。
    // ─────────────────────────────────────────────────────────────

    // 轴对齐包围盒（AABB）
    struct Aabb
    {
        Vec3 min{1e30, 1e30, 1e30};
        Vec3 max{-1e30, -1e30, -1e30};

        Aabb() = default;
        Aabb(const Vec3 &mn, const Vec3 &mx) : min(mn), max(mx) {}

        bool overlaps(const Aabb &o) const
        {
            return min.x <= o.max.x && max.x >= o.min.x &&
                   min.y <= o.max.y && max.y >= o.min.y &&
                   min.z <= o.max.z && max.z >= o.min.z;
        }

        Aabb merged(const Aabb &o) const
        {
            return Aabb(
                {std::min(min.x, o.min.x), std::min(min.y, o.min.y), std::min(min.z, o.min.z)},
                {std::max(max.x, o.max.x), std::max(max.y, o.max.y), std::max(max.z, o.max.z)});
        }

        Vec3 center() const { return (min + max) * 0.5; }
    };

    // BVH 节点
    struct BvhNode
    {
        Aabb box;
        int left = -1;       // 左子节点索引
        int right = -1;      // 右子节点索引
        int body_index = -1; // 叶子节点对应的刚体索引（-1 = 内部节点）
    };

    class BroadPhase
    {
    private:
        std::vector<BvhNode> nodes_;
        std::vector<Aabb> boxes_;
        int root_ = -1;

        static double centroid_axis(const Aabb &b, int axis)
        {
            Vec3 c = b.center();
            return (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
        }

        int build_node(std::vector<int> &indices, int lo, int hi)
        {
            // 合并包围盒
            Aabb box;
            for (int i = lo; i < hi; ++i)
                box = box.merged(boxes_[indices[i]]);

            if (hi - lo == 1)
            {
                BvhNode node;
                node.box = box;
                node.body_index = indices[lo];
                nodes_.push_back(node);
                return static_cast<int>(nodes_.size() - 1);
            }

            // 选最长轴，按质心排序划分
            Vec3 ext = box.max - box.min;
            int axis = 0;
            if (ext.y > ext.x) axis = 1;
            if (ext.z > (axis == 0 ? ext.x : ext.y)) axis = 2;

            std::sort(indices.begin() + lo, indices.begin() + hi,
                      [this, axis](int a, int b) {
                          return centroid_axis(boxes_[a], axis) < centroid_axis(boxes_[b], axis);
                      });

            int mid = (lo + hi) / 2;
            int left = build_node(indices, lo, mid);
            int right = build_node(indices, mid, hi);

            BvhNode node;
            node.box = box;
            node.left = left;
            node.right = right;
            nodes_.push_back(node);
            return static_cast<int>(nodes_.size() - 1);
        }

        void cross_query(int a, int b, std::vector<std::pair<int, int>> &pairs) const
        {
            if (a < 0 || b < 0)
                return;
            if (!nodes_[a].box.overlaps(nodes_[b].box))
                return;

            bool a_leaf = nodes_[a].body_index >= 0;
            bool b_leaf = nodes_[b].body_index >= 0;

            if (a_leaf && b_leaf)
            {
                pairs.push_back({nodes_[a].body_index, nodes_[b].body_index});
                return;
            }
            if (a_leaf)
            {
                cross_query(a, nodes_[b].left, pairs);
                cross_query(a, nodes_[b].right, pairs);
            }
            else if (b_leaf)
            {
                cross_query(nodes_[a].left, b, pairs);
                cross_query(nodes_[a].right, b, pairs);
            }
            else
            {
                cross_query(nodes_[a].left, nodes_[b].left, pairs);
                cross_query(nodes_[a].left, nodes_[b].right, pairs);
                cross_query(nodes_[a].right, nodes_[b].left, pairs);
                cross_query(nodes_[a].right, nodes_[b].right, pairs);
            }
        }

        void self_query(int node, std::vector<std::pair<int, int>> &pairs) const
        {
            if (node < 0)
                return;
            const BvhNode &n = nodes_[node];
            if (n.body_index >= 0)
                return; // 叶子

            cross_query(n.left, n.right, pairs); // 跨子树碰撞
            self_query(n.left, pairs);
            self_query(n.right, pairs);
        }

    public:
        void build(const std::vector<Aabb> &boxes)
        {
            boxes_ = boxes;
            nodes_.clear();
            root_ = -1;
            if (boxes_.empty())
                return;
            std::vector<int> indices(boxes_.size());
            for (size_t i = 0; i < indices.size(); ++i)
                indices[i] = static_cast<int>(i);
            root_ = build_node(indices, 0, static_cast<int>(indices.size()));
        }

        // 返回潜在碰撞对（body 索引对，未做窄相）
        std::vector<std::pair<int, int>> query_pairs() const
        {
            std::vector<std::pair<int, int>> pairs;
            if (root_ >= 0)
                self_query(root_, pairs);
            return pairs;
        }

        size_t node_count() const { return nodes_.size(); }
        const std::vector<BvhNode> &nodes() const { return nodes_; }
    };
}