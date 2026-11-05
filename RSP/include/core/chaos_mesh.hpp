#pragma once
#include <string>
#include <vector>
#include <utility>
#include "qpc/physics_kernel.hpp"
#include "qpc/convex_hull.hpp"
#include "render/scene.hpp"
#include "render/mesh_loader.hpp"

namespace rvdlrsp::core
{

    class ChaosMesh
    {
    public:
        ChaosMesh() = default;

        // 从文件加载（.obj/.gltf/.glb）
        // 加载失败（文件损坏 / 格式不支持 / 凸包退化）时返回 false，由调用方回退
        bool load(const std::string &path, double mass, const qpc::Vec3 &pos = {})
        {
            try
            {
                render::Mesh mesh = render::MeshLoader::load(path);
                return build(mesh, mass, pos);
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        bool build(const render::Mesh &mesh, double mass, const qpc::Vec3 &pos = {})
        {
            mesh_ = mesh;
            mass_ = mass;
            initial_pos_ = pos;
            std::vector<qpc::Vec3> pts;
            pts.reserve(mesh_.vertices.size());
            for (const auto &v : mesh_.vertices)
                pts.push_back(v.position);

            const size_t max_pts = 256;
            if (pts.size() > max_pts)
            {
                std::vector<qpc::Vec3> sub;
                sub.reserve(max_pts);
                double step = static_cast<double>(pts.size()) / max_pts;
                for (size_t i = 0; i < max_pts; ++i)
                    sub.push_back(pts[static_cast<size_t>(i * step)]);
                pts = std::move(sub);
            }

            hull_ = qpc::build_convex_hull(pts);
            if (hull_.vertices.size() < 4)
                return false;

            instance_.mesh_id = 0;
            instance_.position = pos;
            instance_.scale = {1, 1, 1};
            return true;
        }

        size_t add_to(qpc::PhysicsKernel &kernel)
        {
            qpc::RigidBody body;
            body.set_mass(mass_);
            body.position = initial_pos_;
            body.restitution = 0.3;
            body.friction = 0.7;

            qpc::Collider col;
            col.type = qpc::ShapeType::ConvexHull;
            col.hull = hull_;
            col.body_index = 0;

            body_index_ = kernel.add_body(body, col);
            return body_index_;
        }

        void update(qpc::PhysicsKernel &kernel)
        {
            const qpc::RigidBody &b = kernel.body(body_index_);
            instance_.position = b.position;
            instance_.orientation = b.orientation;
        }

        const render::Mesh &mesh() const { return mesh_; }
        const render::SceneInstance &instance() const { return instance_; }
        size_t body_index() const { return body_index_; }
        void set_mesh_id(uint32_t id) { instance_.mesh_id = id; }

    private:
        render::Mesh mesh_;
        qpc::ConvexHull hull_;
        double mass_ = 1.0;
        qpc::Vec3 initial_pos_;
        size_t body_index_ = 0;
        render::SceneInstance instance_;
    };
}