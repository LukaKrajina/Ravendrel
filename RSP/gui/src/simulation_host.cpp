#include "simulation_host.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>

#include "render/mesh_loader.hpp"
#include "render/json.hpp"
#include "core/robot.hpp"
#include "core/chaos_mesh.hpp"
#include "qpc/physics_kernel.hpp"
#include "qcdrc/teleop.hpp"
#include "qcdrc/teleop_driver.hpp"
#include "control/rl_agent.hpp"
#include "qbNs/qbNs.hpp"
#include "qlm/QLM.hpp"
#include "pcg/rps_fractal.hpp"

namespace rvdlrsp::gui
{

    struct SimulationHost::Impl
    {
        SimulationConfig cfg_;

        // ── 内核（分步构建）────────────────────────────────────────
        std::unique_ptr<qpc::PhysicsKernel> kernel_;
        core::Robot robot_;
        core::ChaosMesh chaos_mesh_;
        qcdrc::Teleop teleop_;                               // 默认构造
        std::unique_ptr<control::QuantumRLAgent> agent_;
        std::unique_ptr<qbns::QbNS> brain_;                  // 脑量子接口
        std::unique_ptr<qlm::QLM> qlm_;                      // 量子学习机
        qcdrc::TeleopDriver::Config drive_cfg_;

        // 地面刚体索引
        size_t ground_body_idx_ = 0;
        size_t chaos_body_idx_ = 0;
        uint32_t chaos_mesh_id_ = 0;
        uint32_t ground_mesh_id_ = 0;
        std::vector<render::Mesh> scene_meshes_;
        std::vector<render::SceneInstance> scene_instances_;
        std::vector<SceneEntity> scene_entities_;
        render::SceneInstance ground_instance_;

        // RPS 分形地形（纯视觉装饰，接入 3D 视口实时渲染）
        uint32_t fractal_mesh_id_ = 0;
        render::SceneInstance fractal_instance_;
        bool fractal_enabled_ = true;

        // 导入的外部网格（静态装饰物）
        struct ImportedObject {
            uint32_t mesh_id = 0;
            size_t body_index = 0;
            render::SceneInstance instance;
            std::string name;
        };
        std::vector<ImportedObject> imported_objects_;

        bool robot_loaded_ = false;
        bool physics_ready_ = false;

        // ── 状态 ──────────────────────────────────────────────────
        float robot_x_ = 0.0f;
        float robot_y_ = 1.0f;
        float robot_z_ = 0.0f;
        float target_x_ = 3.0f;
        float target_z_ = 2.0f;
        float sim_time_ = 0.0f;

        bool teleop_active_ = false;
        float joint_angle_ = 0.0f;
        std::string teleop_status_ = "空闲";

        float gravity_ = 9.81f;
        int solver_iterations_ = 4;
        bool paused_ = false;
        std::string physics_status_ = "运行中";

        bool training_ = false;
        float rl_progress_ = 0.0f;
        double rl_reward_ = 0.0;
        int rl_episode_ = 0;
        int rl_episode_step_ = 0;
        std::string rl_status_ = "空闲";

        std::string quantum_backend_;
        int quantum_num_qubits_ = 0;
        std::string quantum_state_;
        std::string quantum_last_measure_;

        float consciousness_awareness_ = 0.5f;
        std::string consciousness_state_;
        std::string brain_signal_;
        std::string brain_channel_state_;

        std::vector<std::string> scene_items_;
        std::vector<std::string> circuit_gates_;
        std::vector<std::string> blueprint_nodes_;
        std::string log_text_;

        int sim_step_ = 0;

        void reset_scene()
        {
            scene_items_ = {
                "humanoid_robot [MultiBody] 14 parts",
                "ground [AABB] static"};

            circuit_gates_ = {
                "H   @ q0",
                "CNOT @ q0,q1",
                "Rz(θ) @ q2",
                "Measure @ q0"};

            blueprint_nodes_ = {
                "Node: SensorInput  ->  Perception",
                "Node: Perception   ->  Planner",
                "Node: Planner      ->  Actuator",
                "Node: Actuator     ->  Motor"};

            quantum_backend_ = "QVM (本地量子虚拟机)";
            quantum_num_qubits_ = 8;
            quantum_state_ = "叠加态 |ψ⟩ = α|0⟩ + β|1⟩ (Polyhedral Graph)";
            quantum_last_measure_ = "（等待测量）";

            consciousness_state_ = "清醒 / 感知中";
            brain_signal_ = "α 波 (8-12 Hz)";
            brain_channel_state_ = "4 通道在线";

            log_text_ = "[rvdlRSP] 仿真平台初始化完成（多刚体机器人 + 骨架 + 关节约束）";
        }

        void append_log(const std::string &line)
        {
            log_text_ += "\n" + line;
            if (log_text_.size() > 4000)
            {
                size_t cut = log_text_.find('\n', log_text_.size() - 4000);
                if (cut != std::string::npos)
                    log_text_ = log_text_.substr(cut + 1);
            }
        }

        // 构建完整场景网格（机器人 + 地面 + chaos），记录 mesh_id 映射
        void build_scene_meshes()
        {
            scene_meshes_.clear();
            for (const auto &m : robot_.meshes())
                scene_meshes_.push_back(m);

            ground_mesh_id_ = static_cast<uint32_t>(scene_meshes_.size());
            scene_meshes_.push_back(render::make_cube(1.0f));
            ground_instance_.mesh_id = ground_mesh_id_;
            ground_instance_.position = {0, -2.5, 0};
            ground_instance_.scale = {24, 1, 24};

            chaos_mesh_id_ = static_cast<uint32_t>(scene_meshes_.size());
            scene_meshes_.push_back(chaos_mesh_.mesh());

            // RPS 分形地形：截面固定 s 分量，扫 (r,p)，逃逸计数→高度
            {
                const auto &ft = cfg_.fractal;
                fractal_enabled_ = ft.enabled;
                if (fractal_enabled_)
                {
                    auto grid = pcg::escape_grid(-ft.extent, ft.extent,
                                                 -ft.extent, ft.extent,
                                                 ft.slice_s, ft.resolution, ft.resolution, ft.max_iter);
                    fractal_mesh_id_ = static_cast<uint32_t>(scene_meshes_.size());
                    scene_meshes_.push_back(pcg::heightfield_mesh(
                        grid, ft.resolution, ft.resolution,
                        -ft.extent, ft.extent,
                        -ft.extent, ft.extent, ft.height_scale));
                    fractal_instance_.mesh_id = fractal_mesh_id_;
                    fractal_instance_.position = {0, -2.49, 0};   // 略高于地面顶面
                    fractal_instance_.scale = {1, 1, 1};
                }
            }
        }

        static const char *shape_name(qpc::ShapeType t)
        {
            switch (t)
            {
            case qpc::ShapeType::Sphere:
                return "Sphere";
            case qpc::ShapeType::AABB:
                return "AABB";
            case qpc::ShapeType::Capsule:
                return "Capsule";
            case qpc::ShapeType::Cylinder:
                return "Cylinder";
            case qpc::ShapeType::ConvexHull:
                return "ConvexHull";
            }
            return "Unknown";
        }

        void build_scene_entities()
        {
            scene_entities_.clear();

            for (auto &b : robot_.skeleton().bones())
            {
                SceneEntity e;
                e.name = b->name;
                e.kind = "part";
                e.body_index = b->body_index;
                e.position = b->world_pos;
                e.rotation = b->world_rot;
                e.mass = kernel_->body(b->body_index).mass;
                e.collider = shape_name(kernel_->collider(b->body_index).type);
                scene_entities_.push_back(e);
            }

            SceneEntity g;
            g.name = "ground";
            g.kind = "ground";
            g.body_index = ground_body_idx_;
            g.position = {0, -2.5, 0};
            g.collider = "AABB";
            scene_entities_.push_back(g);

            SceneEntity c;
            c.name = "chaos_mesh";
            c.kind = "chaos";
            c.body_index = chaos_body_idx_;
            c.position = chaos_mesh_.instance().position;
            c.rotation = chaos_mesh_.instance().orientation;
            c.mass = kernel_->body(chaos_body_idx_).mass;
            c.collider = "ConvexHull";
            scene_entities_.push_back(c);

            if (fractal_enabled_)
            {
                SceneEntity f;
                f.name = "rps_fractal";
                f.kind = "fractal";
                f.body_index = static_cast<size_t>(-1);   // 纯视觉，无物理刚体
                f.position = fractal_instance_.position;
                f.scale = fractal_instance_.scale;
                f.mass = 0.0;
                f.collider = "None";
                scene_entities_.push_back(f);
            }

            // 导入的外部网格
            for (const auto &obj : imported_objects_)
            {
                SceneEntity e;
                e.name = obj.name;
                e.kind = "imported";
                e.body_index = obj.body_index;
                e.position = obj.instance.position;
                e.rotation = obj.instance.orientation;
                e.scale = obj.instance.scale;
                e.mass = 0.0;
                e.collider = "ConvexHull";
                scene_entities_.push_back(e);
            }
        }
    };

    // ────────────────────────────────────────────────────────────────
    // 构造 / 析构
    // ────────────────────────────────────────────────────────────────
    SimulationHost::SimulationHost()
        : impl_(std::make_unique<Impl>())
    {
        impl_->reset_scene();
        impl_->append_log("[rvdlRSP] 仿真平台初始化（默认配置）");
    }

    SimulationHost::SimulationHost(const SimulationConfig &cfg)
        : impl_(std::make_unique<Impl>())
    {
        impl_->cfg_ = cfg;
        impl_->reset_scene();
        impl_->append_log("[rvdlRSP] 仿真平台初始化（项目: " + cfg.project_name + "）");
    }

    SimulationHost::~SimulationHost() = default;

    const SimulationConfig &SimulationHost::config() const { return impl_->cfg_; }

    // ────────────────────────────────────────────────────────────────
    // 分步初始化
    // ────────────────────────────────────────────────────────────────
    bool SimulationHost::initialize_all()
    {
        return loadRobotModel() && initPhysics() && loadSceneEnvironment() &&
               initQuantumDevice() && initBrainInterface() &&
               initQuantumLearningMachine() && initRobotControl();
    }

    bool SimulationHost::loadRobotModel()
    {
        auto &im = *impl_;
        try
        {
            switch (im.cfg_.robot_type)
            {
            case RobotType::Arm:
                im.robot_ = core::make_arm_robot();
                break;
            case RobotType::Mobile:
                im.robot_ = core::make_mobile_robot();
                break;
            case RobotType::Drone:
                im.robot_ = core::make_drone();
                break;
            case RobotType::Custom:
                im.robot_ = im.cfg_.custom_robot_json.empty()
                                ? core::make_humanoid_robot()
                                : core::robot_from_json(im.cfg_.custom_robot_json);
                break;
            case RobotType::Humanoid:
            default:
                im.robot_ = core::make_humanoid_robot();
                break;
            }
            im.robot_loaded_ = true;
            im.append_log("[load] 机器人模型已加载（" + to_string(im.cfg_.robot_type) + "）");
            return true;
        }
        catch (const std::exception &e)
        {
            im.append_log(std::string("[load][ERROR] 机器人模型加载失败: ") + e.what());
            return false;
        }
    }

    bool SimulationHost::initPhysics()
    {
        auto &im = *impl_;
        // 先以占位 QVM 创建内核，量子设备在 initQuantumDevice 按配置替换
        im.kernel_ = std::make_unique<qpc::PhysicsKernel>(false, 1.0 / 60.0);
        im.kernel_->set_gravity({0.0, im.cfg_.env.gravity, 0.0});
        im.kernel_->set_solver_iterations(4);

        // 地面
        qpc::RigidBody ground;
        ground.set_static(true);
        ground.position = {0, -2.5, 0};
        qpc::Collider gc;
        gc.type = qpc::ShapeType::AABB;
        gc.half_extents = {12, 0.5, 12};
        im.ground_body_idx_ = im.kernel_->add_body(ground, gc);

        // 机器人刚体（依赖内核）
        if (im.robot_loaded_)
            im.robot_.build(*im.kernel_);

        // RL 智能体
        im.agent_ = std::make_unique<control::QuantumRLAgent>(3, 2);

        im.physics_ready_ = true;
        im.append_log("[load] 物理引擎已初始化（重力 " + std::to_string(im.cfg_.env.gravity) + "）");
        return true;
    }

    bool SimulationHost::loadSceneEnvironment()
    {
        auto &im = *impl_;
        if (!im.physics_ready_)
            return false;

        // 障碍物/交互对象：程序化立方体（不接外部 glTF）
        render::Mesh obstacle = render::make_cube(1.0f);
        if (im.chaos_mesh_.build(obstacle, 1.0, {2.0, 3.0, 0.0}))
            im.chaos_body_idx_ = im.chaos_mesh_.add_to(*im.kernel_);
        else
            im.append_log("[load][WARN] 障碍物网格构建失败，已跳过");

        im.build_scene_meshes();
        im.build_scene_entities();
        im.append_log("[load] 场景环境已加载：温度 " + std::to_string(im.cfg_.env.temperature_c) +
                      "°C，风速 " + std::to_string(im.cfg_.env.wind_speed) + " m/s");
        return true;
    }

    bool SimulationHost::initQuantumDevice()
    {
        auto &im = *impl_;
        if (!im.kernel_)
            return false;
        try
        {
            switch (im.cfg_.backend)
            {
            case SimBackend::QM_Superconducting:
                im.kernel_->set_backend(std::make_unique<qhal::QM>(qhal::HardwareModality::Superconducting, 0));
                im.quantum_backend_ = "QM (超导)";
                break;
            case SimBackend::QM_TrappedIon:
                im.kernel_->set_backend(std::make_unique<qhal::QM>(qhal::HardwareModality::TrappedIon, 0));
                im.quantum_backend_ = "QM (离子阱)";
                break;
            case SimBackend::QM_NeutralAtom:
                im.kernel_->set_backend(std::make_unique<qhal::QM>(qhal::HardwareModality::NeutralAtom, 0));
                im.quantum_backend_ = "QM (中性原子)";
                break;
            case SimBackend::QVM:
            default:
                im.kernel_->set_backend(std::make_unique<qhal::QVM>());
                im.quantum_backend_ = "QVM (本地量子虚拟机)";
                break;
            }
        }
        catch (const std::exception &e)
        {
            // 真实量子机连接失败 → 回退 QVM
            im.kernel_->set_backend(std::make_unique<qhal::QVM>());
            im.quantum_backend_ = "QVM (回退)";
            im.append_log(std::string("[load][WARN] 量子设备连接失败，回退 QVM: ") + e.what());
        }

        // 握手：分配 8 qubit + H + 测量
        qhal::IQuantumBackend *be = im.kernel_->backend();
        be->allocate_qubits(8);
        be->apply_h(0);
        int m = be->measure(0);
        im.quantum_num_qubits_ = 8;
        if (im.agent_)
            im.agent_->set_backend(be);
        im.append_log("[load] 量子设备已就绪（" + im.quantum_backend_ + "，握手 q0=" + std::to_string(m) + "）");
        return true;
    }

    bool SimulationHost::initBrainInterface()
    {
        auto &im = *impl_;
        try
        {
            im.brain_ = qbns::QbNS::create_with_qvm(qbns::BMIModality::NonInvasive);
            im.append_log("[load] 脑量子接口已初始化（QbNS, NonInvasive）");
            return true;
        }
        catch (const std::exception &e)
        {
            im.append_log(std::string("[load][ERROR] 脑量子接口初始化失败: ") + e.what());
            return false;
        }
    }

    bool SimulationHost::initQuantumLearningMachine()
    {
        auto &im = *impl_;
        if (!im.kernel_)
            return false;
        try
        {
            im.qlm_ = std::make_unique<qlm::QLM>(im.kernel_->backend(), 16, 4);
            im.append_log("[load] 量子学习机已初始化（QLM, 16 qubits, 4 layers）");
            return true;
        }
        catch (const std::exception &e)
        {
            im.append_log(std::string("[load][ERROR] 量子学习机初始化失败: ") + e.what());
            return false;
        }
    }

    bool SimulationHost::initRobotControl()
    {
        auto &im = *impl_;
        im.teleop_status_ = "就绪";
        im.append_log("[load] 机器人控制接口已初始化（QCDRC 遥操作 + IK + RL）");
        return true;
    }

    // ────────────────────────────────────────────────────────────────
    // 主步进
    // ────────────────────────────────────────────────────────────────
    void SimulationHost::step(float dt)
    {
        auto &im = *impl_;
        if (!im.kernel_)
            return;
        im.sim_step_++;
        im.sim_time_ += dt;

        if (im.teleop_active_)
        {
            // 遥操作：关节角驱动机器人根骨骼运动
            std::vector<double> angles = im.teleop_.teleop_step();
            if (!angles.empty())
                im.joint_angle_ = static_cast<float>(angles[0]);

            // 根骨骼兼容：人形 pelvis / 机械臂 base / 移动 chassis / 无人机 body
            core::Bone *root = im.robot_.skeleton().find("pelvis");
            if (!root) root = im.robot_.skeleton().find("base");
            if (!root) root = im.robot_.skeleton().find("chassis");
            if (!root) root = im.robot_.skeleton().find("body");
            if (root)
            {
                qpc::RigidBody &rb = im.kernel_->body(root->body_index);
                qpc::Vec3 target = qcdrc::TeleopDriver::joint_to_target(angles, im.drive_cfg_);
                rb.apply_force(qcdrc::TeleopDriver::compute_force(rb, target, im.drive_cfg_));
            }
        }

        if (im.training_ && im.agent_ && im.robot_loaded_)
        {
            // 量子 RL 训练一步：观测（目标误差）→ 量子动作 → 驱动力 → 奖励
            core::Bone *root = im.robot_.skeleton().find("pelvis");
            if (!root) root = im.robot_.skeleton().find("base");
            if (!root) root = im.robot_.skeleton().find("chassis");
            if (!root) root = im.robot_.skeleton().find("body");
            if (root && root->body_index < im.kernel_->body_count())
            {
                qpc::RigidBody &rb = im.kernel_->body(root->body_index);
                double dx = im.target_x_ - rb.position.x;
                double dz = im.target_z_ - rb.position.z;
                double dist = std::sqrt(dx * dx + dz * dz);

                // 观测 3 维：[目标误差 X, 目标误差 Z, 水平距离]
                std::vector<double> obs{dx, dz, dist};
                // 量子动作 2 维：[力 X, 力 Z]
                std::vector<double> action = im.agent_->act_quantum(obs);
                double fx = action.empty() ? 0.0 : action[0];
                double fz = action.size() > 1 ? action[1] : 0.0;
                rb.apply_force({fx * im.drive_cfg_.drive_scale, 0.0,
                                fz * im.drive_cfg_.drive_scale});

                // 奖励 = 负水平距离（接近目标奖励增大）
                double reward = -dist;
                im.rl_reward_ = reward;
                im.agent_->store(obs, action, reward);

                ++im.rl_episode_step_;
                if (im.rl_episode_step_ % 10 == 0)
                    im.agent_->train(1);

                // episode 结束：到达目标或步数超限
                if (dist < 0.3 || im.rl_episode_step_ >= 200)
                {
                    ++im.rl_episode_;
                    im.rl_episode_step_ = 0;
                    im.append_log("[rl] episode " + std::to_string(im.rl_episode_) +
                                  " 完成, 最终奖励 " + std::to_string(reward));
                }
                im.rl_progress_ = std::min(1.0f,
                                           static_cast<float>(im.rl_episode_step_) / 200.0f);
            }
        }

        if (!im.paused_)
            im.kernel_->step();

        // 同步机器人骨架 + 渲染实例
        im.robot_.update(*im.kernel_);
        im.chaos_mesh_.update(*im.kernel_);

        im.scene_instances_.clear();
        for (const auto &inst : im.robot_.instances())
            im.scene_instances_.push_back(inst);
        im.scene_instances_.push_back(im.ground_instance_);
        render::SceneInstance ci = im.chaos_mesh_.instance();
        ci.mesh_id = im.chaos_mesh_id_;
        im.scene_instances_.push_back(ci);
        if (im.fractal_enabled_)
            im.scene_instances_.push_back(im.fractal_instance_);

        // 导入的外部网格实例（位置跟随刚体，支持变换编辑）
        for (auto &obj : im.imported_objects_)
        {
            if (obj.body_index < im.kernel_->body_count())
            {
                const auto &rb = im.kernel_->body(obj.body_index);
                obj.instance.position = rb.position;
                obj.instance.orientation = rb.orientation;
            }
            im.scene_instances_.push_back(obj.instance);
        }

        // 更新场景实体（Outliner / Details 联动）
        im.build_scene_entities();

        // 根骨骼位置（俯视图 + 场景图）
        core::Bone *root = im.robot_.skeleton().find("pelvis");
        if (!root) root = im.robot_.skeleton().find("base");
        if (!root) root = im.robot_.skeleton().find("chassis");
        if (!root) root = im.robot_.skeleton().find("body");
        if (root)
        {
            im.robot_x_ = static_cast<float>(root->world_pos.x);
            im.robot_y_ = static_cast<float>(root->world_pos.y);
            im.robot_z_ = static_cast<float>(root->world_pos.z);
        }

        im.consciousness_awareness_ = 0.5f + 0.3f * static_cast<float>(std::sin(im.sim_time_ * 0.5f));

        char buf[128];
        std::snprintf(buf, sizeof(buf), "robot pos(%.2f, %.2f, %.2f)",
                      im.robot_x_, im.robot_y_, im.robot_z_);
        im.scene_items_[0] = buf;
    }

    // ────────────────────────────────────────────────────────────────
    // 遥操作
    // ────────────────────────────────────────────────────────────────
    void SimulationHost::start_teleop()
    {
        auto &im = *impl_;
        im.teleop_active_ = true;
        im.teleop_status_ = "运行中";
        im.append_log("[teleop] 遥操作已启动（QCDRC）");
    }
    void SimulationHost::stop_teleop()
    {
        auto &im = *impl_;
        im.teleop_active_ = false;
        im.teleop_status_ = "已停止";
        im.append_log("[teleop] 遥操作已停止");
    }
    float SimulationHost::joint_angle() const { return impl_->joint_angle_; }
    void SimulationHost::set_joint_angle(float v) { impl_->joint_angle_ = v; }
    const std::string &SimulationHost::teleop_status() const { return impl_->teleop_status_; }

    // ────────────────────────────────────────────────────────────────
    // 物理
    // ────────────────────────────────────────────────────────────────
    float SimulationHost::gravity() const { return impl_->gravity_; }
    void SimulationHost::set_gravity(float v)
    {
        auto &im = *impl_;
        im.gravity_ = v;
        if (im.kernel_)
            im.kernel_->set_gravity({0.0, -static_cast<double>(v), 0.0});
    }
    int SimulationHost::solver_iterations() const { return impl_->solver_iterations_; }
    void SimulationHost::set_solver_iterations(int v)
    {
        auto &im = *impl_;
        im.solver_iterations_ = v;
        if (im.kernel_)
            im.kernel_->set_solver_iterations(v);
    }
    void SimulationHost::pause()
    {
        auto &im = *impl_;
        im.paused_ = true;
        im.physics_status_ = "已暂停";
        im.append_log("[physics] 仿真已暂停");
    }
    void SimulationHost::step_once()
    {
        auto &im = *impl_;
        im.paused_ = true;
        if (im.kernel_)
        {
            im.kernel_->step();
            im.robot_.update(*im.kernel_);
        }
        im.physics_status_ = "单步执行";
        im.append_log("[physics] 单步执行");
    }
    const std::string &SimulationHost::physics_status() const { return impl_->physics_status_; }

    // ────────────────────────────────────────────────────────────────
    // RL（量子强化学习训练循环,接入 step()）
    // ────────────────────────────────────────────────────────────────
    void SimulationHost::start_training()
    {
        auto &im = *impl_;
        im.training_ = true;
        im.rl_episode_ = 0;
        im.rl_episode_step_ = 0;
        im.rl_reward_ = 0.0;
        im.rl_progress_ = 0.0f;
        im.rl_status_ = "训练中";
        im.append_log("[rl] 开始量子强化学习训练（观测 3 维 → 动作 2 维）");
    }
    void SimulationHost::stop_training()
    {
        auto &im = *impl_;
        im.training_ = false;
        im.rl_status_ = "已停止";
        im.append_log("[rl] 停止训练（共 " + std::to_string(im.rl_episode_) + " 个 episode）");
    }
    float SimulationHost::rl_progress() const { return impl_->rl_progress_; }
    float SimulationHost::rl_reward() const { return static_cast<float>(impl_->rl_reward_); }
    int SimulationHost::rl_episode() const { return impl_->rl_episode_; }
    const std::string &SimulationHost::rl_status() const { return impl_->rl_status_; }

    // ────────────────────────────────────────────────────────────────
    // 俯视图
    // ────────────────────────────────────────────────────────────────
    float SimulationHost::robot_x() const { return impl_->robot_x_; }
    float SimulationHost::robot_y() const { return impl_->robot_y_; }
    float SimulationHost::robot_z() const { return impl_->robot_z_; }
    float SimulationHost::target_x() const { return impl_->target_x_; }
    float SimulationHost::target_z() const { return impl_->target_z_; }
    std::string SimulationHost::sim_time_str() const
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f s", impl_->sim_time_);
        return std::string(buf);
    }

    // ────────────────────────────────────────────────────────────────
    // 机器人渲染数据
    // ────────────────────────────────────────────────────────────────
    const std::vector<render::Mesh> &SimulationHost::robot_meshes() const { return impl_->robot_.meshes(); }
    const std::vector<render::SceneInstance> &SimulationHost::robot_instances() const { return impl_->robot_.instances(); }
    size_t SimulationHost::robot_part_count() const { return impl_->robot_.instances().size(); }

    const std::vector<render::Mesh> &SimulationHost::scene_meshes() const { return impl_->scene_meshes_; }
    const std::vector<render::SceneInstance> &SimulationHost::scene_instances() const { return impl_->scene_instances_; }
    size_t SimulationHost::scene_part_count() const { return impl_->scene_instances_.size(); }

    double SimulationHost::quantum_normalization_residual() const
    {
        if (!impl_->kernel_)
            return 0.0;
        return impl_->kernel_->quantum_health().normalization_residual;
    }

    bool SimulationHost::set_fractal_params(const FractalTerrain &f)
    {
        auto &im = *impl_;
        bool changed = (im.cfg_.fractal.enabled != f.enabled ||
                        im.cfg_.fractal.resolution != f.resolution ||
                        im.cfg_.fractal.extent != f.extent ||
                        im.cfg_.fractal.height_scale != f.height_scale ||
                        im.cfg_.fractal.max_iter != f.max_iter ||
                        im.cfg_.fractal.slice_s != f.slice_s);
        im.cfg_.fractal = f;
        if (changed)
            im.build_scene_meshes();
        return changed;
    }

    const std::vector<SceneEntity> &SimulationHost::scene_entities() const { return impl_->scene_entities_; }

    void SimulationHost::set_entity_position(int index, const qpc::Vec3 &pos)
    {
        auto &im = *impl_;
        if (!im.kernel_)
            return;
        if (index < 0 || index >= static_cast<int>(im.scene_entities_.size()))
            return;
        size_t bi = im.scene_entities_[static_cast<size_t>(index)].body_index;
        if (bi < im.kernel_->body_count())
            im.kernel_->body(bi).position = pos;
    }

    void SimulationHost::set_entity_rotation(int index, const qpc::Quat &rot)
    {
        auto &im = *impl_;
        if (index < 0 || index >= static_cast<int>(im.scene_entities_.size()))
            return;
        const SceneEntity &e = im.scene_entities_[static_cast<size_t>(index)];
        // 仅导入的静态模型支持旋转
        if (e.kind != "imported")
            return;
        for (auto &obj : im.imported_objects_)
        {
            if (obj.body_index == e.body_index)
            {
                if (im.kernel_ && obj.body_index < im.kernel_->body_count())
                    im.kernel_->body(obj.body_index).orientation = rot;
                obj.instance.orientation = rot;
                return;
            }
        }
    }

    void SimulationHost::set_entity_scale(int index, const qpc::Vec3 &scale)
    {
        auto &im = *impl_;
        if (index < 0 || index >= static_cast<int>(im.scene_entities_.size()))
            return;
        const SceneEntity &e = im.scene_entities_[static_cast<size_t>(index)];
        // 仅导入的静态模型支持缩放
        if (e.kind != "imported")
            return;
        for (auto &obj : im.imported_objects_)
        {
            if (obj.body_index == e.body_index)
            {
                obj.instance.scale = scale;
                return;
            }
        }
    }

    bool SimulationHost::import_mesh(const std::string &path)
    {
        auto &im = *impl_;
        if (!im.kernel_ || !im.physics_ready_)
            return false;

        try
        {
            render::Mesh mesh = render::MeshLoader::load(path);
            if (mesh.vertices.empty())
                return false;

            uint32_t mesh_id = static_cast<uint32_t>(im.scene_meshes_.size());
            im.scene_meshes_.push_back(mesh);

            // 静态装饰刚体（AABB 碰撞体，尺寸取自网格包围盒）
            qpc::Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
            for (const auto &v : mesh.vertices)
            {
                lo.x = std::min(lo.x, v.position.x);
                lo.y = std::min(lo.y, v.position.y);
                lo.z = std::min(lo.z, v.position.z);
                hi.x = std::max(hi.x, v.position.x);
                hi.y = std::max(hi.y, v.position.y);
                hi.z = std::max(hi.z, v.position.z);
            }
            qpc::Vec3 center = (lo + hi) * 0.5;
            qpc::Vec3 half = (hi - lo) * 0.5;
            double max_half = std::max({half.x, half.y, half.z, 0.1});

            qpc::RigidBody body;
            body.set_static(true);
            body.position = {0.0, 1.0, 0.0};
            qpc::Collider col;
            col.type = qpc::ShapeType::AABB;
            col.half_extents = {max_half, max_half, max_half};
            size_t body_idx = im.kernel_->add_body(body, col);

            Impl::ImportedObject obj;
            obj.mesh_id = mesh_id;
            obj.body_index = body_idx;
            obj.name = path.substr(path.find_last_of("/\\") + 1);
            obj.instance.mesh_id = mesh_id;
            obj.instance.position = {0.0, 1.0, 0.0};
            obj.instance.scale = {1, 1, 1};
            im.imported_objects_.push_back(obj);

            im.append_log("[import] 已导入网格: " + obj.name);
            return true;
        }
        catch (const std::exception &e)
        {
            im.append_log(std::string("[import][ERROR] 网格导入失败: ") + e.what());
            return false;
        }
    }

    bool SimulationHost::load_scene_file(const std::string &path)
    {
        auto &im = *impl_;
        if (!im.kernel_ || !im.physics_ready_)
            return false;

        try
        {
            // 读取 .qscene JSON：{"meshes": ["a.glb", "b.obj"], "gravity": -9.81}
            std::ifstream fs(path);
            if (!fs)
                return false;
            std::stringstream ss;
            ss << fs.rdbuf();
            fs.close();

            json::Value root = json::parse(ss.str());

            // 清空旧的导入对象
            im.imported_objects_.clear();

            // 相对路径 → 相对 .qscene 所在目录解析
            std::string base_dir = path.substr(0, path.find_last_of("/\\") + 1);

            int imported = 0;
            if (root.object().count("meshes") > 0)
            {
                for (const auto &v : root.at("meshes").array())
                {
                    std::string mesh_path = v.string();
                    std::string abs = mesh_path;
                    // 简单判断：不含盘符/根路径则视为相对路径
                    bool is_abs = (mesh_path.size() >= 2 && mesh_path[1] == ':') ||
                                  mesh_path[0] == '/' || mesh_path[0] == '\\';
                    if (!is_abs)
                        abs = base_dir + mesh_path;
                    if (import_mesh(abs))
                        ++imported;
                }
            }

            // 环境参数（可选）
            if (root.object().count("gravity") > 0)
            {
                double g = root.at("gravity").number();
                im.kernel_->set_gravity({0.0, g, 0.0});
            }

            im.append_log("[scene] 场景已加载: " + std::to_string(imported) + " 个网格");
            return imported > 0;
        }
        catch (const std::exception &e)
        {
            im.append_log(std::string("[scene][ERROR] 场景加载失败: ") + e.what());
            return false;
        }
    }

    bool SimulationHost::remove_imported_mesh(int entity_index)
    {
        auto &im = *impl_;
        if (entity_index < 0 || entity_index >= static_cast<int>(im.scene_entities_.size()))
            return false;

        const SceneEntity &e = im.scene_entities_[static_cast<size_t>(entity_index)];
        if (e.kind != "imported")
            return false;

        // 从导入对象列表移除对应 body_index 的项
        const size_t before = im.imported_objects_.size();
        im.imported_objects_.erase(
            std::remove_if(im.imported_objects_.begin(), im.imported_objects_.end(),
                           [&](const Impl::ImportedObject &o) { return o.body_index == e.body_index; }),
            im.imported_objects_.end());

        if (im.imported_objects_.size() == before)
            return false;

        // 同步禁用物理刚体（不再参与积分与碰撞）
        if (im.kernel_)
            im.kernel_->disable_body(e.body_index);

        im.append_log("[import] 已删除导入网格: " + e.name);
        return true;
    }

    bool SimulationHost::rename_imported_mesh(int entity_index, const std::string &new_name)
    {
        auto &im = *impl_;
        if (entity_index < 0 || entity_index >= static_cast<int>(im.scene_entities_.size()))
            return false;
        if (new_name.empty())
            return false;

        const SceneEntity &e = im.scene_entities_[static_cast<size_t>(entity_index)];
        if (e.kind != "imported")
            return false;

        for (auto &obj : im.imported_objects_)
        {
            if (obj.body_index == e.body_index)
            {
                obj.name = new_name;
                im.append_log("[import] 已重命名: " + e.name + " → " + new_name);
                return true;
            }
        }
        return false;
    }

    // ────────────────────────────────────────────────────────────────
    // 量子
    // ────────────────────────────────────────────────────────────────
    const std::string &SimulationHost::quantum_backend() const { return impl_->quantum_backend_; }
    int SimulationHost::quantum_num_qubits() const { return impl_->quantum_num_qubits_; }
    const std::string &SimulationHost::quantum_state() const { return impl_->quantum_state_; }
    const std::string &SimulationHost::quantum_last_measure() const { return impl_->quantum_last_measure_; }

    // ────────────────────────────────────────────────────────────────
    // 意识 / 脑机桥
    // ────────────────────────────────────────────────────────────────
    float SimulationHost::consciousness_awareness() const { return impl_->consciousness_awareness_; }
    const std::string &SimulationHost::consciousness_state() const { return impl_->consciousness_state_; }
    const std::string &SimulationHost::brain_signal() const { return impl_->brain_signal_; }
    const std::string &SimulationHost::brain_channel_state() const { return impl_->brain_channel_state_; }

    // ────────────────────────────────────────────────────────────────
    // 列表
    // ────────────────────────────────────────────────────────────────
    const std::vector<std::string> &SimulationHost::scene_items() const { return impl_->scene_items_; }
    const std::vector<std::string> &SimulationHost::circuit_gates() const { return impl_->circuit_gates_; }
    const std::vector<std::string> &SimulationHost::blueprint_nodes() const { return impl_->blueprint_nodes_; }
    const std::string &SimulationHost::log_text() const { return impl_->log_text_; }

    // ────────────────────────────────────────────────────────────────
    // 指标
    // ────────────────────────────────────────────────────────────────
    int SimulationHost::sim_step() const { return impl_->sim_step_; }

    // GPU 加速：当前后端未实现基于 gpu_accel 的 GPU 调度，如实返回 false。
    bool SimulationHost::gpu_accel_active() const
    {
        return false;
    }
}
