#pragma once
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include "qpc/math.hpp"
#include "render/mat4.hpp"

namespace rvdlrsp::core
{

    struct Bone
    {
        std::string name;
        Bone *parent = nullptr;
        std::vector<Bone *> children;

        // 局部变换（相对父骨骼）
        qpc::Vec3 local_pos;
        qpc::Quat local_rot;

        // 世界变换（update_world_transforms 前向传播）
        qpc::Vec3 world_pos;
        qpc::Quat world_rot;

        // 关联刚体（物理）与零件网格（渲染）
        size_t body_index = 0;
        int mesh_id = -1;
        qpc::Vec3 mesh_offset{0, 0, 0};   // 零件网格相对骨骼原点的偏移
        qpc::Vec3 mesh_scale{1, 1, 1};    // 零件网格缩放

        // 骨骼长度（到子骨骼的距离，物理装配用）
        float length = 0.0f;

        // 绑定姿态（蒙皮用）：记录初始局部变换
        qpc::Vec3 bind_pos;
        qpc::Quat bind_rot;
        render::Mat4 bind_world;     // 世界绑定矩阵（局部→世界）
        render::Mat4 inverse_bind;   // 逆绑定矩阵（世界→局部）
        int parent_index = -1;       // 父骨骼在 bones_ 中的下标
    };

    class Skeleton
    {
    public:
        // 添加骨骼（parent 为空则为根）
        Bone *add_bone(const std::string &name, Bone *parent,
                       const qpc::Vec3 &local_pos = {},
                       const qpc::Quat &local_rot = {})
        {
            auto bone = std::make_unique<Bone>();
            bone->name = name;
            bone->parent = parent;
            bone->local_pos = local_pos;
            bone->local_rot = local_rot;
            Bone *raw = bone.get();
            bones_.push_back(std::move(bone));

            if (parent)
            {
                parent->children.push_back(raw);
                raw->parent_index = static_cast<int>(index_of(parent));
            }
            else if (!root_)
                root_ = raw;

            return raw;
        }

        Bone *root() const { return root_; }

        Bone *find(const std::string &name)
        {
            for (auto &b : bones_)
                if (b->name == name)
                    return b.get();
            return nullptr;
        }

        // 前向传播世界变换（root 的局部 = 世界，子 = 父世界 * 局部）
        void update_world_transforms()
        {
            if (root_)
                propagate(root_, qpc::Vec3{}, qpc::Quat{});
        }

        // 骨骼数量 / 下标查询
        size_t size() const { return bones_.size(); }

        size_t index_of(const Bone *b) const
        {
            for (size_t i = 0; i < bones_.size(); ++i)
                if (bones_[i].get() == b)
                    return i;
            return 0;
        }

        // 计算绑定姿态：记录当前局部变换 + 世界绑定矩阵 + 逆绑定矩阵
        void compute_bind_pose()
        {
            for (auto &b : bones_)
            {
                b->bind_pos = b->local_pos;
                b->bind_rot = b->local_rot;
            }
            if (root_)
                compute_bind_world(root_, render::Mat4::identity());
            for (auto &b : bones_)
                b->inverse_bind = b->bind_world.inverse();
        }

        const std::vector<std::unique_ptr<Bone>> &bones() const { return bones_; }

    private:
        void propagate(Bone *bone, const qpc::Vec3 &parent_pos, const qpc::Quat &parent_rot)
        {
            // 世界位置 = 父世界位置 + 父世界旋转 * 局部位置
            bone->world_pos = parent_pos + parent_rot.rotate(bone->local_pos);
            bone->world_rot = (parent_rot * bone->local_rot).normalized();

            for (auto *child : bone->children)
                propagate(child, bone->world_pos, bone->world_rot);
        }

        void compute_bind_world(Bone *bone, const render::Mat4 &parent)
        {
            render::Mat4 local = render::Mat4::model(bone->bind_pos, bone->bind_rot, {1, 1, 1});
            bone->bind_world = parent * local;
            for (auto *child : bone->children)
                compute_bind_world(child, bone->bind_world);
        }

        std::vector<std::unique_ptr<Bone>> bones_;
        Bone *root_ = nullptr;
    };
}