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

        qpc::Vec3 local_pos;
        qpc::Quat local_rot;

        qpc::Vec3 world_pos;
        qpc::Quat world_rot;

        size_t body_index = 0;
        int mesh_id = -1;
        qpc::Vec3 mesh_offset{0, 0, 0};
        qpc::Vec3 mesh_scale{1, 1, 1};

        float length = 0.0f;

        qpc::Vec3 bind_pos;
        qpc::Quat bind_rot;
        render::Mat4 bind_world;
        render::Mat4 inverse_bind;
        int parent_index = -1;
    };

    class Skeleton
    {
    public:
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

        void update_world_transforms()
        {
            if (root_)
                propagate(root_, qpc::Vec3{}, qpc::Quat{});
        }

        size_t size() const { return bones_.size(); }

        size_t index_of(const Bone *b) const
        {
            for (size_t i = 0; i < bones_.size(); ++i)
                if (bones_[i].get() == b)
                    return i;
            return 0;
        }

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