#pragma once
#include <memory>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <mutex>
#include "hardware/observability.hpp"
#include "qhal/IQuantumBackend.hpp"
#include "qhal/QM.hpp"
#include "qhal/QVM.hpp"
#include "math.hpp"
#include "rigid_body.hpp"
#include "collision.hpp"
#include "constraint.hpp"
#include "joint.hpp"
#include "broadphase.hpp"
#include "Kokkos_Core.hpp"

namespace rvdlrsp::qpc
{

    // ─────────────────────────────────────────────────────────────
    // Kokkos 并行物理积分
    //
    // 刚体运动状态打包为 trivially-copyable 的 POD 结构，放入
    // Kokkos::View，用 parallel_for 在默认执行空间（CUDA/Serial/
    // OpenMP）上并行积分。四元数积分手写为 KOKKOS_INLINE_FUNCTION，
    // 兼容 device 执行空间。
    // ─────────────────────────────────────────────────────────────

    struct BodyStatePOD
    {
        double px = 0, py = 0, pz = 0;      // position
        double vx = 0, vy = 0, vz = 0;      // linear velocity
        double ox = 0, oy = 0, oz = 0, ow = 1; // orientation quaternion (x,y,z,w)
        double wx = 0, wy = 0, wz = 0;      // angular velocity
        double fx = 0, fy = 0, fz = 0;      // force
        double tx = 0, ty = 0, tz = 0;      // torque
        double inv_mass = 1.0;
        double inv_inertia = 1.0;
        double lin_damp = 0.0;
        double ang_damp = 0.0;
        int is_static = 0;
        int enabled = 1;
    };

    // 四元数积分（device 友好）：dq/dt = 0.5 * (0, omega) ⊗ q
    KOKKOS_INLINE_FUNCTION
    void integrate_quaternion_pod(double &ox, double &oy, double &oz, double &ow,
                                  double wx, double wy, double wz, double h)
    {
        double dqx = 0.5 * (wx * ow + wy * oz - wz * oy);
        double dqy = 0.5 * (wy * ow + wz * ox - wx * oz);
        double dqz = 0.5 * (wz * ow + wx * oy - wy * ox);
        double dqw = 0.5 * (-wx * ox - wy * oy - wz * oz);
        ox += dqx * h;
        oy += dqy * h;
        oz += dqz * h;
        ow += dqw * h;
        double inv = 1.0 / Kokkos::sqrt(ox * ox + oy * oy + oz * oz + ow * ow);
        ox *= inv;
        oy *= inv;
        oz *= inv;
        ow *= inv;
    }

    // Kokkos 积分 functor（避免 extended lambda 不能用于 private 成员函数的限制）
    struct IntegrateBodiesFunctor
    {
        Kokkos::View<BodyStatePOD *> states;
        double gx = 0, gy = 0, gz = 0;
        double h = 0;

        KOKKOS_INLINE_FUNCTION
        void operator()(const int i) const
        {
            BodyStatePOD &s = states(i);
            if (!s.enabled || s.is_static)
                return;

            double ax = s.fx * s.inv_mass + gx;
            double ay = s.fy * s.inv_mass + gy;
            double az = s.fz * s.inv_mass + gz;
            double aax = s.tx * s.inv_inertia;
            double aay = s.ty * s.inv_inertia;
            double aaz = s.tz * s.inv_inertia;

            s.vx += ax * h;
            s.vy += ay * h;
            s.vz += az * h;
            s.wx += aax * h;
            s.wy += aay * h;
            s.wz += aaz * h;

            // 物理阻尼(与步长无关的稳定衰减):v *= 1/(1 + damping*h)
            double lin = 1.0 / (1.0 + s.lin_damp * h);
            double ang = 1.0 / (1.0 + s.ang_damp * h);
            s.vx *= lin; s.vy *= lin; s.vz *= lin;
            s.wx *= ang; s.wy *= ang; s.wz *= ang;

            s.px += s.vx * h;
            s.py += s.vy * h;
            s.pz += s.vz * h;

            integrate_quaternion_pod(s.ox, s.oy, s.oz, s.ow, s.wx, s.wy, s.wz, h);

            s.fx = 0; s.fy = 0; s.fz = 0;
            s.tx = 0; s.ty = 0; s.tz = 0;
        }
    };

    class PhysicsKernel
    {
    private:
        std::unique_ptr<qhal::IQuantumBackend> backend_;
        bool use_real_qm_ = false;
        bool use_kokkos_integration_ = false; // 启用 Kokkos 并行积分
        double dt_ = 1.0 / 60.0;
        Vec3 gravity_{0.0, -9.81, 0.0};
        int solver_iterations_ = 4;

        std::vector<RigidBody> bodies_;
        std::vector<Collider> colliders_;
        std::vector<Contact> contacts_;
        std::vector<Joint> joints_;
        std::vector<bool> body_enabled_;   // 与 bodies_ 对齐：false 表示已禁用（删除）

    public:
        explicit PhysicsKernel(bool use_real_qm = false, double dt = 1.0 / 60.0)
            : use_real_qm_(use_real_qm), dt_(dt)
        {
            if (use_real_qm_)
                backend_ = std::make_unique<qhal::QM>(qhal::HardwareModality::Superconducting, 0);
            else
                backend_ = std::make_unique<qhal::QVM>();
            RVDLRSP_INFO("qpc") << "Physics kernel online ("
                                 << (use_real_qm_ ? "QM" : "QVM") << ", dt=" << dt_ << ").";
        }

        qhal::IQuantumBackend *backend() { return backend_.get(); }

        // 允许内核创建后再注入量子后端（分步初始化用）
        void set_backend(std::unique_ptr<qhal::IQuantumBackend> be) { backend_ = std::move(be); }

        // 启用/禁用 Kokkos 并行积分（默认禁用，保持标量积分）
        void enable_kokkos_integration(bool enable = true)
        {
            use_kokkos_integration_ = enable;
            RVDLRSP_INFO("qpc") << "Kokkos integration "
                                 << (enable ? "enabled" : "disabled") << ".";
        }
        bool kokkos_integration_enabled() const { return use_kokkos_integration_; }

        void set_gravity(const Vec3 &g) { gravity_ = g; }
        void set_solver_iterations(int n) { solver_iterations_ = n; }
        double dt() const { return dt_; }

        // ─── 刚体管理 ──────────────────────────────────────
        size_t add_body(const RigidBody &body, const Collider &col)
        {
            size_t idx = bodies_.size();
            bodies_.push_back(body);
            Collider c = col;
            c.body_index = idx;
            colliders_.push_back(c);
            body_enabled_.push_back(true);
            return idx;
        }

        // 禁用刚体（"软删除"）：不再参与积分与碰撞，保留索引避免重映射。
        void disable_body(size_t i)
        {
            if (i < body_enabled_.size())
                body_enabled_[i] = false;
        }

        bool body_is_enabled(size_t i) const
        {
            return i < body_enabled_.size() && body_enabled_[i];
        }

        RigidBody &body(size_t i)
        {
            if (i >= bodies_.size())
                throw std::out_of_range("[rvdlRSP.qpc] body index out of range");
            return bodies_[i];
        }
        const RigidBody &body(size_t i) const
        {
            if (i >= bodies_.size())
                throw std::out_of_range("[rvdlRSP.qpc] body index out of range");
            return bodies_[i];
        }
        size_t body_count() const { return bodies_.size(); }
        const Collider &collider(size_t i) const
        {
            if (i >= colliders_.size())
                throw std::out_of_range("[rvdlRSP.qpc] collider index out of range");
            return colliders_[i];
        }
        const std::vector<Contact> &contacts() const { return contacts_; }

        // ─── 关节约束 ──────────────────────────────────────
        size_t add_joint(const Joint &j)
        {
            joints_.push_back(j);
            return joints_.size() - 1;
        }
        const std::vector<Joint> &joints() const { return joints_; }

        // 数值健康度报告：有效质量比（sequential impulse 求解的"条件数"物理解释）
        struct HealthReport
        {
            double min_eff_mass = 0.0;  // 最小有效质量 (1/inv_mass_sum)
            double max_eff_mass = 0.0;  // 最大有效质量
            double mass_ratio = 1.0;    // = max/min，极端质量比导致冲量求解不收敛
            bool healthy = true;
        };
        HealthReport numerical_health() const
        {
            HealthReport r;
            double mn = 1e30, mx = 0.0;
            for (const auto &c : contacts_)
            {
                double em = 1.0 / (bodies_[c.a].inv_mass + bodies_[c.b].inv_mass + 1e-12);
                mn = std::min(mn, em);
                mx = std::max(mx, em);
            }
            if (mn < 1e30)
            {
                r.min_eff_mass = mn;
                r.max_eff_mass = mx;
            }
            r.mass_ratio = (mn < 1e30 && mn > 0.0) ? mx / mn : 1.0;
            r.healthy = r.mass_ratio < 1e4;
            return r;
        }

        // 量子态矢量数值健康度（归一化残差 |1-‖ψ‖|）
        qhal::health::NumericHealth quantum_health() const
        {
            auto *qvm = dynamic_cast<qhal::QVM *>(backend_.get());
            if (qvm)
                return qvm->monitor_health();
            return qhal::health::NumericHealth{};
        }

        // ─── 物理步进 ──────────────────────────────────────
        void step(int substeps = 1)
        {
            double h = dt_ / substeps;
            for (int s = 0; s < substeps; ++s)
            {
                if (use_kokkos_integration_)
                    integrate_kokkos(h);
                else
                    integrate(h);
                solve_joints();
                detect_collisions();
                solve(h);
            }
        }

    private:
        // 半隐式欧拉积分
        void integrate(double h)
        {
            for (size_t i = 0; i < bodies_.size(); ++i)
            {
                if (!body_enabled_[i])
                    continue;
                RigidBody &b = bodies_[i];
                if (b.is_static)
                    continue;

                // 重力
                Vec3 accel = b.force * b.inv_mass + gravity_;
                Vec3 ang_accel = b.torque * b.inv_inertia;

                // 速度更新
                b.linear_velocity += accel * h;
                b.angular_velocity += ang_accel * h;

                // 物理阻尼（与步长无关的稳定衰减）
                b.linear_velocity *= 1.0 / (1.0 + b.linear_damping * h);
                b.angular_velocity *= 1.0 / (1.0 + b.angular_damping * h);

                // 位置更新
                b.position += b.linear_velocity * h;
                b.orientation = b.orientation.integrate(b.angular_velocity, h);

                b.clear_forces();
            }
        }

        // 把刚体状态打包为 POD 数组，用 parallel_for 在默认执行空间
        // （CUDA/Serial/OpenMP）上并行积分，写回 bodies_。
        void integrate_kokkos(double h)
        {
            const size_t n = bodies_.size();
            if (n == 0)
                return;

            // 惰性初始化 Kokkos(首次并行积分时),确保 View 构造前已 initialize。
            // 与 runtime_api.cpp 的既有模式一致,避免测试/库使用者忘记初始化
            // 导致 "View constructed before initialize()" 崩溃。
            static std::once_flag kokkos_init_flag;
            std::call_once(kokkos_init_flag, []() {
                if (!Kokkos::is_initialized())
                    Kokkos::initialize();
            });

            // 提取到 host POD 数组
            std::vector<BodyStatePOD> host(n);
            for (size_t i = 0; i < n; ++i)
            {
                const RigidBody &b = bodies_[i];
                BodyStatePOD &s = host[i];
                s.px = b.position.x; s.py = b.position.y; s.pz = b.position.z;
                s.vx = b.linear_velocity.x; s.vy = b.linear_velocity.y; s.vz = b.linear_velocity.z;
                s.ox = b.orientation.x; s.oy = b.orientation.y; s.oz = b.orientation.z; s.ow = b.orientation.w;
                s.wx = b.angular_velocity.x; s.wy = b.angular_velocity.y; s.wz = b.angular_velocity.z;
                s.fx = b.force.x; s.fy = b.force.y; s.fz = b.force.z;
                s.tx = b.torque.x; s.ty = b.torque.y; s.tz = b.torque.z;
                s.inv_mass = b.inv_mass;
                s.inv_inertia = b.inv_inertia;
                s.lin_damp = b.linear_damping;
                s.ang_damp = b.angular_damping;
                s.is_static = b.is_static ? 1 : 0;
                s.enabled = body_enabled_[i] ? 1 : 0;
            }

            const double gx = gravity_.x, gy = gravity_.y, gz = gravity_.z;

            Kokkos::View<BodyStatePOD *> d_states("BodyStates", n);
            auto h_states = Kokkos::create_mirror_view(d_states);
            for (size_t i = 0; i < n; ++i)
                h_states(i) = host[i];
            Kokkos::deep_copy(d_states, h_states);

            IntegrateBodiesFunctor functor;
            functor.states = d_states;
            functor.gx = gx;
            functor.gy = gy;
            functor.gz = gz;
            functor.h = h;
            Kokkos::parallel_for("IntegrateBodies",
                                 Kokkos::RangePolicy<int>(0, static_cast<int>(n)),
                                 functor);
            Kokkos::fence();
            Kokkos::deep_copy(h_states, d_states);

            for (size_t i = 0; i < n; ++i)
            {
                const BodyStatePOD &s = h_states(i);
                RigidBody &b = bodies_[i];
                b.position = {s.px, s.py, s.pz};
                b.linear_velocity = {s.vx, s.vy, s.vz};
                b.orientation = Quat(s.ox, s.oy, s.oz, s.ow);
                b.angular_velocity = {s.wx, s.wy, s.wz};
                b.force = {s.fx, s.fy, s.fz};
                b.torque = {s.tx, s.ty, s.tz};
            }
        }

        // 计算碰撞体的世界空间 AABB(用于 BVH 宽相)
        static Aabb compute_aabb(const RigidBody &body, const Collider &col)
        {
            switch (col.type)
            {
            case ShapeType::Sphere:
            {
                Vec3 r{col.radius, col.radius, col.radius};
                return Aabb(body.position - r, body.position + r);
            }
            case ShapeType::AABB:
                return Aabb(body.position - col.half_extents,
                            body.position + col.half_extents);
            case ShapeType::Capsule:
            case ShapeType::Cylinder:
            {
                Vec3 r{col.radius, col.capsule_half_height + col.radius, col.radius};
                return Aabb(body.position - r, body.position + r);
            }
            case ShapeType::ConvexHull:
            {
                Aabb box;
                for (const auto &v : col.hull.vertices)
                {
                    Vec3 w = body.position + body.orientation.rotate(v);
                    box.min.x = std::min(box.min.x, w.x);
                    box.min.y = std::min(box.min.y, w.y);
                    box.min.z = std::min(box.min.z, w.z);
                    box.max.x = std::max(box.max.x, w.x);
                    box.max.y = std::max(box.max.y, w.y);
                    box.max.z = std::max(box.max.z, w.z);
                }
                return box;
            }
            }
            return Aabb{};
        }

        // 宽相(BVH)+ 窄相碰撞检测
        // 把 O(n²) 全对检测降为 BVH 宽相 O(n log n) + 稀疏窄相。
        void detect_collisions()
        {
            contacts_.clear();
            const size_t n = colliders_.size();
            if (n < 2)
                return;

            // 收集活跃碰撞体 + 计算世界 AABB
            std::vector<size_t> active;
            std::vector<Aabb> boxes;
            active.reserve(n);
            boxes.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                size_t bi = colliders_[i].body_index;
                if (bi >= body_enabled_.size() || !body_enabled_[bi])
                    continue;
                active.push_back(i);
                boxes.push_back(compute_aabb(bodies_[bi], colliders_[i]));
            }

            // 构建 BVH,查询潜在碰撞对,再做窄相精确检测
            BroadPhase bp;
            bp.build(boxes);
            for (const auto &pr : bp.query_pairs())
            {
                size_t ci = active[static_cast<size_t>(pr.first)];
                size_t cj = active[static_cast<size_t>(pr.second)];
                size_t ia = colliders_[ci].body_index;
                size_t jb = colliders_[cj].body_index;
                RigidBody &a = bodies_[ia];
                RigidBody &b = bodies_[jb];
                if (a.is_static && b.is_static)
                    continue;

                Contact c;
                if (dispatch(a, colliders_[ci], b, colliders_[cj], c))
                    contacts_.push_back(c);
            }
        }

        // 形状组合分派（圆柱：球精确，其余退化为胶囊近似）
        static bool dispatch(const RigidBody &a, const Collider &ca,
                             const RigidBody &b, const Collider &cb, Contact &c)
        {
            ShapeType ta = ca.type, tb = cb.type;

            if (ta == ShapeType::Cylinder)
            {
                if (tb == ShapeType::Sphere)
                    return CollisionDetector::cylinder_sphere(a, ca, b, cb, c);
                if (tb == ShapeType::Cylinder)
                    return CollisionDetector::cylinder_cylinder(a, ca, b, cb, c);
                if (tb == ShapeType::Capsule)
                    return CollisionDetector::cylinder_capsule(a, ca, b, cb, c);
                if (tb == ShapeType::AABB)
                    return CollisionDetector::cylinder_aabb(a, ca, b, cb, c);
            }
            if (tb == ShapeType::Cylinder)
            {
                if (ta == ShapeType::Sphere)
                    return CollisionDetector::cylinder_sphere(b, cb, a, ca, c);
                if (ta == ShapeType::Capsule)
                    return CollisionDetector::cylinder_capsule(b, cb, a, ca, c);
                if (ta == ShapeType::AABB)
                    return CollisionDetector::cylinder_aabb(b, cb, a, ca, c);
            }

            if (ta == ShapeType::ConvexHull)
            {
                if (tb == ShapeType::ConvexHull)
                    return CollisionDetector::convex_convex(a, ca, b, cb, c);
                if (tb == ShapeType::Sphere)
                    return CollisionDetector::convex_sphere(a, ca, b, cb, c);
                if (tb == ShapeType::AABB)
                    return CollisionDetector::convex_aabb(a, ca, b, cb, c);
                if (tb == ShapeType::Capsule)
                    return CollisionDetector::convex_capsule(a, ca, b, cb, c);
                if (tb == ShapeType::Cylinder)
                    return CollisionDetector::convex_cylinder(a, ca, b, cb, c);
            }
            if (tb == ShapeType::ConvexHull)
            {
                if (ta == ShapeType::Sphere)
                    return CollisionDetector::convex_sphere(b, cb, a, ca, c);
                if (ta == ShapeType::AABB)
                    return CollisionDetector::convex_aabb(b, cb, a, ca, c);
                if (ta == ShapeType::Capsule)
                    return CollisionDetector::convex_capsule(b, cb, a, ca, c);
                if (ta == ShapeType::Cylinder)
                    return CollisionDetector::convex_cylinder(b, cb, a, ca, c);
            }

            if (ta == ShapeType::Capsule && tb == ShapeType::Capsule)
                return CollisionDetector::capsule_capsule(a, ca, b, cb, c);
            if (ta == ShapeType::Capsule && tb == ShapeType::Sphere)
                return CollisionDetector::capsule_sphere(a, ca, b, cb, c);
            if (tb == ShapeType::Capsule && ta == ShapeType::Sphere)
                return CollisionDetector::capsule_sphere(b, cb, a, ca, c);
            if (ta == ShapeType::Capsule && tb == ShapeType::AABB)
                return CollisionDetector::capsule_aabb(a, ca, b, cb, c);
            if (tb == ShapeType::Capsule && ta == ShapeType::AABB)
                return CollisionDetector::capsule_aabb(b, cb, a, ca, c);
            if (ta == ShapeType::Sphere && tb == ShapeType::Sphere)
                return CollisionDetector::sphere_sphere(a, ca, b, cb, c);
            if (ta == ShapeType::Sphere)
                return CollisionDetector::sphere_aabb(a, ca, b, cb, c);
            if (tb == ShapeType::Sphere)
                return CollisionDetector::sphere_aabb(b, cb, a, ca, c);
            return CollisionDetector::aabb_aabb(a, ca, b, cb, c);
        }

        // 迭代求解约束
        void solve(double h)
        {
            (void)h;
            for (int it = 0; it < solver_iterations_; ++it)
            {
                for (auto &c : contacts_)
                {
                    RigidBody &a = bodies_[c.a];
                    RigidBody &b = bodies_[c.b];
                    ConstraintSolver::solve_contact(a, b, c);
                }
            }
            for (auto &c : contacts_)
            {
                RigidBody &a = bodies_[c.a];
                RigidBody &b = bodies_[c.b];
                ConstraintSolver::positional_correction(a, b, c);
            }
        }

        // 关节约束求解（在碰撞检测之前，保证骨骼连接稳定）
        void solve_joints()
        {
            for (int it = 0; it < solver_iterations_; ++it)
            {
                for (auto &j : joints_)
                {
                    if (j.body_a >= bodies_.size() || j.body_b >= bodies_.size())
                        continue;
                    RigidBody &a = bodies_[j.body_a];
                    RigidBody &b = bodies_[j.body_b];
                    JointSolver::solve(a, b, j);
                }
            }
        }
    };
}