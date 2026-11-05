#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "hardware/observability.hpp"
#include "skeleton.hpp"
#include "qpc/physics_kernel.hpp"
#include "qpc/joint.hpp"
#include "render/mesh_generator.hpp"
#include "render/scene.hpp"
#include "render/json.hpp"

namespace rvdlrsp::core
{

    // 几何体种类（用于程序化生成零件网格）
    enum class PartShape
    {
        Box,
        Cylinder,
        Capsule,
        Sphere,
        Cone,
        Torus,
        Plane
    };

    // 零件定义（一个骨骼）
    struct PartDef
    {
        std::string name;
        std::string parent; // 父骨骼名（空 = 根）
        qpc::JointType joint = qpc::JointType::BallSocket;
        qpc::Vec3 local_pos; // 相对父骨骼的位置
        qpc::ShapeType collider = qpc::ShapeType::Capsule;
        double radius = 0.1;
        double half_height = 0.2; // 胶囊/圆柱半高
        double mass = 1.0;
        PartShape shape = PartShape::Capsule;
        qpc::Vec3 mesh_scale{1, 1, 1};

        // 关节锚点（相对父骨骼，可选；默认用 local_pos）
        qpc::Vec3 local_anchor{0, 0, 0};
        bool use_local_anchor = false;
    };

    class Robot
    {
    public:
        Robot() = default;

        // 添加零件定义
        void add_part(const PartDef &p) { parts_.push_back(p); }

        // 构建：生成骨架 + 刚体 + 关节 + 渲染数据
        void build(qpc::PhysicsKernel &kernel)
        {
            if (built_)
            {
                RVDLRSP_WARN("core") << "Robot already built, skipping rebuild.";
                return;
            }
            built_ = true;

            for (const auto &p : parts_)
            {
                Bone *parent = p.parent.empty() ? nullptr : skeleton_.find(p.parent);
                Bone *b = skeleton_.add_bone(p.name, parent, p.local_pos);
                b->length = static_cast<float>(p.half_height * 2.0);
                b->mesh_scale = p.mesh_scale;
                part_by_name_[p.name] = &p;
            }

            for (const auto &p : parts_)
            {
                qpc::RigidBody body;
                body.set_mass(p.mass);
                body.position = p.local_pos;
                body.restitution = 0.2;
                body.friction = 0.8;

                qpc::Collider col;
                col.type = p.collider;
                col.radius = p.radius;
                col.capsule_half_height = p.half_height;
                if (p.collider == qpc::ShapeType::AABB)
                    col.half_extents = {p.radius, p.half_height, p.radius};

                size_t idx = kernel.add_body(body, col);
                body_index_by_name_[p.name] = idx;
                Bone *bp = skeleton_.find(p.name);
                if (bp)
                    bp->body_index = idx;
            }

            for (const auto &p : parts_)
            {
                if (p.parent.empty())
                    continue;
                auto it = body_index_by_name_.find(p.parent);
                if (it == body_index_by_name_.end())
                    continue;

                qpc::Joint j;
                j.type = p.joint;
                j.body_a = it->second;
                j.body_b = body_index_by_name_[p.name];
                const qpc::RigidBody &parent_body = kernel.body(j.body_a);
                qpc::Vec3 anchor_offset = p.use_local_anchor ? p.local_anchor : p.local_pos;
                j.anchor = parent_body.position + parent_body.orientation.rotate(anchor_offset);
                j.stiffness = 0.8;
                kernel.add_joint(j);
            }

            build_render_data();
        }

        // 每帧：从物理状态同步骨架世界变换 + 渲染实例
        void update(qpc::PhysicsKernel &kernel)
        {
            // 从刚体位置更新骨骼世界变换
            for (auto &b : skeleton_.bones())
            {
                const qpc::RigidBody &body = kernel.body(b->body_index);
                b->world_pos = body.position;
                b->world_rot = body.orientation;
            }

            // 更新渲染实例（每零件一个）
            for (size_t i = 0; i < parts_.size() && i < instances_.size(); ++i)
            {
                Bone *b = skeleton_.find(parts_[i].name);
                if (!b)
                    continue;
                instances_[i].position = b->world_pos;
                instances_[i].orientation = b->world_rot;
            }
        }

        Skeleton &skeleton() { return skeleton_; }

        const std::vector<render::Mesh> &meshes() const { return meshes_; }
        const std::vector<render::SceneInstance> &instances() const { return instances_; }

    private:
        void build_render_data()
        {
            meshes_.clear();
            instances_.clear();

            for (const auto &p : parts_)
            {
                render::Mesh mesh;
                qpc::Vec3 scale = p.mesh_scale;
                switch (p.shape)
                {
                case PartShape::Box:
                    mesh = render::make_cube(static_cast<float>(p.radius * 2.0));
                    break;
                case PartShape::Cylinder:
                    mesh = render::make_cylinder(static_cast<float>(p.radius),
                                                 static_cast<float>(p.half_height * 2.0));
                    break;
                case PartShape::Sphere:
                    mesh = render::make_sphere(static_cast<float>(p.radius));
                    break;
                case PartShape::Capsule:
                default:
                    mesh = render::make_capsule(static_cast<float>(p.radius),
                                                static_cast<float>(p.half_height * 2.0));
                    break;
                case PartShape::Cone:
                    mesh = render::make_cone(static_cast<float>(p.radius),
                                             static_cast<float>(p.half_height * 2.0));
                    break;
                case PartShape::Torus:
                    mesh = render::make_torus(static_cast<float>(p.half_height),
                                              static_cast<float>(p.radius));
                    break;
                case PartShape::Plane:
                    mesh = render::make_plane(static_cast<float>(p.radius * 2.0));
                    break;
                }

                meshes_.push_back(std::move(mesh));

                render::SceneInstance inst;
                inst.mesh_id = static_cast<uint32_t>(meshes_.size() - 1);
                inst.position = p.local_pos;
                inst.scale = scale;
                instances_.push_back(inst);
            }
        }

        std::vector<PartDef> parts_;
        Skeleton skeleton_;
        std::unordered_map<std::string, size_t> body_index_by_name_;
        std::unordered_map<std::string, const PartDef *> part_by_name_;
        std::vector<render::Mesh> meshes_;
        std::vector<render::SceneInstance> instances_;
        bool built_ = false;
    };

    // ─── 工厂：从 JSON 字符串装配机器人 ──────────────────────────
    inline Robot robot_from_json(const std::string &json_str)
    {
        json::Value root = json::parse(json_str);
        Robot robot;

        auto to_joint = [](const std::string &s) -> qpc::JointType
        {
            if (s == "Hinge")
                return qpc::JointType::Hinge;
            if (s == "Fixed")
                return qpc::JointType::Fixed;
            if (s == "Prismatic")
                return qpc::JointType::Prismatic;
            if (s == "Distance")
                return qpc::JointType::Distance;
            return qpc::JointType::BallSocket;
        };
        auto to_collider = [](const std::string &s) -> qpc::ShapeType
        {
            if (s == "Sphere")
                return qpc::ShapeType::Sphere;
            if (s == "AABB")
                return qpc::ShapeType::AABB;
            if (s == "Cylinder")
                return qpc::ShapeType::Cylinder;
            return qpc::ShapeType::Capsule;
        };
        auto to_shape = [](const std::string &s) -> PartShape
        {
            if (s == "Box")
                return PartShape::Box;
            if (s == "Sphere")
                return PartShape::Sphere;
            if (s == "Cone")
                return PartShape::Cone;
            if (s == "Torus")
                return PartShape::Torus;
            if (s == "Plane")
                return PartShape::Plane;
            if (s == "Cylinder")
                return PartShape::Cylinder;
            return PartShape::Capsule;
        };
        auto vec3 = [](const json::Value &arr) -> qpc::Vec3
        {
            return {arr[0].number(), arr[1].number(), arr[2].number()};
        };

        for (const auto &pv : root.at("parts").array())
        {
            PartDef p;
            p.name = pv.at("name").string();
            if (pv.object().count("parent") > 0)
                p.parent = pv.at("parent").string();
            if (pv.object().count("joint") > 0)
                p.joint = to_joint(pv.at("joint").string());
            if (pv.object().count("pos") > 0)
                p.local_pos = vec3(pv.at("pos"));
            if (pv.object().count("collider") > 0)
                p.collider = to_collider(pv.at("collider").string());
            if (pv.object().count("radius") > 0)
                p.radius = pv.at("radius").number();
            if (pv.object().count("half_height") > 0)
                p.half_height = pv.at("half_height").number();
            if (pv.object().count("mass") > 0)
                p.mass = pv.at("mass").number();
            if (pv.object().count("shape") > 0)
                p.shape = to_shape(pv.at("shape").string());
            if (pv.object().count("scale") > 0)
                p.mesh_scale = vec3(pv.at("scale"));
            if (pv.object().count("anchor") > 0)
            {
                p.local_anchor = vec3(pv.at("anchor"));
                p.use_local_anchor = true;
            }
            robot.add_part(p);
        }
        return robot;
    }

    // ─── 工厂：创建标准仿人形机器人 ────────────────────────────────
    inline Robot make_humanoid_robot()
    {
        Robot robot;

        // 骨盆（根）
        robot.add_part({"pelvis", "", qpc::JointType::BallSocket, {0, 1.0, 0}, qpc::ShapeType::AABB, 0.22, 0.15, 3.0, PartShape::Box, {1, 1, 1}});

        // 脊柱 → 胸 → 头
        robot.add_part({"spine", "pelvis", qpc::JointType::BallSocket, {0, 0.35, 0}, qpc::ShapeType::Capsule, 0.12, 0.15, 1.5, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"chest", "spine", qpc::JointType::BallSocket, {0, 0.30, 0}, qpc::ShapeType::AABB, 0.24, 0.18, 2.0, PartShape::Box, {1, 1, 1}});
        robot.add_part({"head", "chest", qpc::JointType::BallSocket, {0, 0.28, 0}, qpc::ShapeType::Sphere, 0.14, 0.0, 0.8, PartShape::Sphere, {1, 1, 1}});

        // 手臂（左/右）
        robot.add_part({"arm_L", "chest", qpc::JointType::BallSocket, {-0.36, 0.10, 0}, qpc::ShapeType::Capsule, 0.08, 0.16, 0.6, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"forearm_L", "arm_L", qpc::JointType::Hinge, {0, -0.30, 0}, qpc::ShapeType::Capsule, 0.07, 0.15, 0.5, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"hand_L", "forearm_L", qpc::JointType::BallSocket, {0, -0.28, 0}, qpc::ShapeType::Sphere, 0.07, 0.0, 0.3, PartShape::Sphere, {1, 1, 1}});

        robot.add_part({"arm_R", "chest", qpc::JointType::BallSocket, {0.36, 0.10, 0}, qpc::ShapeType::Capsule, 0.08, 0.16, 0.6, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"forearm_R", "arm_R", qpc::JointType::Hinge, {0, -0.30, 0}, qpc::ShapeType::Capsule, 0.07, 0.15, 0.5, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"hand_R", "forearm_R", qpc::JointType::BallSocket, {0, -0.28, 0}, qpc::ShapeType::Sphere, 0.07, 0.0, 0.3, PartShape::Sphere, {1, 1, 1}});

        // 腿（左/右）
        robot.add_part({"leg_L", "pelvis", qpc::JointType::BallSocket, {-0.16, -0.25, 0}, qpc::ShapeType::Capsule, 0.10, 0.22, 1.2, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"shin_L", "leg_L", qpc::JointType::Hinge, {0, -0.42, 0}, qpc::ShapeType::Capsule, 0.09, 0.20, 1.0, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"foot_L", "shin_L", qpc::JointType::BallSocket, {0, -0.38, 0.08}, qpc::ShapeType::AABB, 0.10, 0.06, 0.5, PartShape::Box, {1.6, 0.4, 1}});

        robot.add_part({"leg_R", "pelvis", qpc::JointType::BallSocket, {0.16, -0.25, 0}, qpc::ShapeType::Capsule, 0.10, 0.22, 1.2, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"shin_R", "leg_R", qpc::JointType::Hinge, {0, -0.42, 0}, qpc::ShapeType::Capsule, 0.09, 0.20, 1.0, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"foot_R", "shin_R", qpc::JointType::BallSocket, {0, -0.38, 0.08}, qpc::ShapeType::AABB, 0.10, 0.06, 0.5, PartShape::Box, {1.6, 0.4, 1}});

        return robot;
    }

    // ─── 工厂：机器臂（底座 → 肩 → 上臂 → 前臂 → 腕 → 夹爪）───────
    inline Robot make_arm_robot()
    {
        Robot robot;
        robot.add_part({"base", "", qpc::JointType::Fixed, {0, 0.3, 0}, qpc::ShapeType::Cylinder, 0.35, 0.25, 5.0, PartShape::Cylinder, {1, 1, 1}});
        robot.add_part({"shoulder", "base", qpc::JointType::Hinge, {0, 0.5, 0}, qpc::ShapeType::Sphere, 0.18, 0.0, 1.5, PartShape::Sphere, {1, 1, 1}});
        robot.add_part({"upper_arm", "shoulder", qpc::JointType::Hinge, {0, 0.35, 0}, qpc::ShapeType::Capsule, 0.12, 0.3, 1.2, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"forearm", "upper_arm", qpc::JointType::Hinge, {0, 0.3, 0}, qpc::ShapeType::Capsule, 0.09, 0.28, 0.9, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"wrist", "forearm", qpc::JointType::BallSocket, {0, 0.28, 0}, qpc::ShapeType::Sphere, 0.08, 0.0, 0.4, PartShape::Sphere, {1, 1, 1}});
        robot.add_part({"gripper", "wrist", qpc::JointType::Fixed, {0, 0.12, 0}, qpc::ShapeType::AABB, 0.06, 0.06, 0.3, PartShape::Box, {1.4, 0.4, 1}});
        return robot;
    }

    // ─── 工厂：义肢手臂（接受腔 → 上臂 → 肘 → 前臂 → 腕 → 假手）───
    // 脑意识控制关节角，量子 RL 辅助精细动作；作为 Robot 零件装配，
    // 拥有刚体 + 关节 + 渲染，可与控制层的 ProstheticLimb 联动。
    inline Robot make_prosthetic_arm()
    {
        Robot robot;
        robot.add_part({"socket", "", qpc::JointType::Fixed, {0, 1.2, 0}, qpc::ShapeType::Cylinder, 0.10, 0.10, 1.5, PartShape::Cylinder, {1, 1, 1}});
        robot.add_part({"upper_arm", "socket", qpc::JointType::BallSocket, {0, 0.18, 0}, qpc::ShapeType::Capsule, 0.08, 0.22, 0.9, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"elbow", "upper_arm", qpc::JointType::Hinge, {0, -0.38, 0}, qpc::ShapeType::Sphere, 0.06, 0.0, 0.4, PartShape::Sphere, {1, 1, 1}});
        robot.add_part({"forearm", "elbow", qpc::JointType::Hinge, {0, -0.30, 0}, qpc::ShapeType::Capsule, 0.06, 0.20, 0.7, PartShape::Capsule, {1, 1, 1}});
        robot.add_part({"wrist", "forearm", qpc::JointType::BallSocket, {0, -0.32, 0}, qpc::ShapeType::Sphere, 0.05, 0.0, 0.3, PartShape::Sphere, {1, 1, 1}});
        robot.add_part({"prosthetic_hand", "wrist", qpc::JointType::BallSocket, {0, -0.14, 0}, qpc::ShapeType::Sphere, 0.07, 0.0, 0.3, PartShape::Sphere, {1, 1, 1}});
        return robot;
    }

    // ─── 工厂：轮式移动机器人（底盘 + 激光雷达 + 四轮）────────────
    inline Robot make_mobile_robot()
    {
        Robot robot;
        robot.add_part({"chassis", "", qpc::JointType::Fixed, {0, 0.35, 0}, qpc::ShapeType::AABB, 0.5, 0.18, 4.0, PartShape::Box, {1, 1, 1}});
        robot.add_part({"lidar", "chassis", qpc::JointType::Fixed, {0, 0.22, 0}, qpc::ShapeType::Cylinder, 0.15, 0.08, 0.5, PartShape::Cylinder, {1, 1, 1}});
        robot.add_part({"wheel_FL", "chassis", qpc::JointType::Hinge, {-0.4, -0.15, 0.45}, qpc::ShapeType::Cylinder, 0.14, 0.05, 0.4, PartShape::Cylinder, {1, 1, 1}});
        robot.add_part({"wheel_FR", "chassis", qpc::JointType::Hinge, {0.4, -0.15, 0.45}, qpc::ShapeType::Cylinder, 0.14, 0.05, 0.4, PartShape::Cylinder, {1, 1, 1}});
        robot.add_part({"wheel_BL", "chassis", qpc::JointType::Hinge, {-0.4, -0.15, -0.45}, qpc::ShapeType::Cylinder, 0.14, 0.05, 0.4, PartShape::Cylinder, {1, 1, 1}});
        robot.add_part({"wheel_BR", "chassis", qpc::JointType::Hinge, {0.4, -0.15, -0.45}, qpc::ShapeType::Cylinder, 0.14, 0.05, 0.4, PartShape::Cylinder, {1, 1, 1}});
        return robot;
    }

    // ─── 工厂：四旋翼无人机（机身 + 四臂 + 四旋翼）────────────────
    inline Robot make_drone()
    {
        Robot robot;
        robot.add_part({"body", "", qpc::JointType::Fixed, {0, 0.6, 0}, qpc::ShapeType::AABB, 0.3, 0.08, 1.2, PartShape::Box, {1, 1, 1}});
        const char *arms[4] = {"arm_FL", "arm_FR", "arm_BL", "arm_BR"};
        const qpc::Vec3 arm_pos[4] = {{-0.3, 0.02, 0.3}, {0.3, 0.02, 0.3}, {-0.3, 0.02, -0.3}, {0.3, 0.02, -0.3}};
        const char *rotors[4] = {"rotor_FL", "rotor_FR", "rotor_BL", "rotor_BR"};
        for (int i = 0; i < 4; ++i)
        {
            robot.add_part({arms[i], "body", qpc::JointType::Fixed, arm_pos[i], qpc::ShapeType::Capsule, 0.03, 0.25, 0.2, PartShape::Capsule, {1, 1, 1}});
            robot.add_part({rotors[i], arms[i], qpc::JointType::Hinge, {0, 0.12, 0}, qpc::ShapeType::Cylinder, 0.25, 0.02, 0.2, PartShape::Cylinder, {1, 1, 1}});
        }
        return robot;
    }
}
