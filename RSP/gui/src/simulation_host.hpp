#pragma once
#include <memory>
#include <string>
#include <vector>
#include "render/scene.hpp"
#include "simulation_config.h"

namespace rvdlrsp::gui
{

    // 场景实体（World Outliner → Details 选中联动用，纯数据）
    struct SceneEntity
    {
        std::string name;
        std::string kind;      // "part" / "ground" / "chaos" / "imported"
        size_t body_index = 0; // 对应物理刚体索引
        qpc::Vec3 position;
        qpc::Quat rotation;
        qpc::Vec3 scale{1, 1, 1};
        double mass = 0.0;
        std::string collider;
    };

    class SimulationHost
    {
    public:
        SimulationHost();
        explicit SimulationHost(const SimulationConfig &cfg);
        ~SimulationHost();

        SimulationHost(const SimulationHost &) = delete;
        SimulationHost &operator=(const SimulationHost &) = delete;

        // ─── 分步初始化（LoadingScreen 用）────────────────────────
        bool initialize_all();             // 同步跑完 7 步（standalone 回退）
        bool loadRobotModel();             // ① 加载机器人硬件/模拟模型
        bool initPhysics();                // ② 初始化物理引擎
        bool loadSceneEnvironment();       // ③ 加载场景环境
        bool initQuantumDevice();          // ④ 初始化量子设备（失败回退 QVM）
        bool initBrainInterface();         // ⑤ 初始化脑量子接口
        bool initQuantumLearningMachine(); // ⑥ 初始化量子学习机
        bool initRobotControl();           // ⑦ 初始化机器人控制接口

        const SimulationConfig &config() const;

        // ─── 主步进 ────────────────────────────────────────────────
        void step(float dt);

        // ─── 遥操作 ────────────────────────────────────────────────
        void start_teleop();
        void stop_teleop();
        float joint_angle() const;
        void set_joint_angle(float v);
        const std::string &teleop_status() const;

        // ─── 物理 ──────────────────────────────────────────────────
        float gravity() const;
        void set_gravity(float v);
        int solver_iterations() const;
        void set_solver_iterations(int v);
        void pause();
        void step_once();
        const std::string &physics_status() const;

        // ─── RL ────────────────────────────────────────────────────
        void start_training();
        void stop_training();
        float rl_progress() const;
        float rl_reward() const;
        int rl_episode() const;
        const std::string &rl_status() const;

        // ─── 俯视图 ────────────────────────────────────────────────
        float robot_x() const;
        float robot_y() const;
        float robot_z() const;
        float target_x() const;
        float target_z() const;
        std::string sim_time_str() const;

        // ─── 渲染数据 ──────────────────────────────────────────────
        const std::vector<render::Mesh> &robot_meshes() const;
        const std::vector<render::SceneInstance> &robot_instances() const;
        size_t robot_part_count() const;
        const std::vector<render::Mesh> &scene_meshes() const;
        const std::vector<render::SceneInstance> &scene_instances() const;
        size_t scene_part_count() const;
        bool set_fractal_params(const FractalTerrain &f);   // 更新分形参数并重建网格

        // ─── 场景实体 ──────────────────────────────────────────────
        const std::vector<SceneEntity> &scene_entities() const;
        void set_entity_position(int index, const qpc::Vec3 &pos);
        void set_entity_rotation(int index, const qpc::Quat &rot);   // 导入模型旋转
        void set_entity_scale(int index, const qpc::Vec3 &scale);    // 导入模型缩放

        // ─── 场景导入 ──────────────────────────────────────────────
        // 加载外部网格文件（.obj/.gltf/.glb）作为静态装饰刚体加入场景。
        // 返回 true 表示导入成功。
        bool import_mesh(const std::string &path);
        // 加载场景文件（.qscene JSON），替换当前场景网格与环境。
        bool load_scene_file(const std::string &path);
        // 删除导入的网格（按场景实体索引，仅限 kind=="imported"）。
        bool remove_imported_mesh(int entity_index);
        // 重命名导入的网格（仅限 kind=="imported"）。
        bool rename_imported_mesh(int entity_index, const std::string &new_name);

        // ─── 量子 ──────────────────────────────────────────────────
        const std::string &quantum_backend() const;
        int quantum_num_qubits() const;
        const std::string &quantum_state() const;
        const std::string &quantum_last_measure() const;
        double quantum_normalization_residual() const;   // 态矢量归一化残差 |1-‖ψ‖|

        // ─── 意识 / 脑机桥 ─────────────────────────────────────────
        float consciousness_awareness() const;
        const std::string &consciousness_state() const;
        const std::string &brain_signal() const;
        const std::string &brain_channel_state() const;

        // ─── 列表 ──────────────────────────────────────────────────
        const std::vector<std::string> &scene_items() const;
        const std::vector<std::string> &circuit_gates() const;
        const std::vector<std::string> &blueprint_nodes() const;
        const std::string &log_text() const;

        // ─── 指标 ──────────────────────────────────────────────────
        int sim_step() const;

        // ─── GPU 加速 ──────────────────────────────────────────────
        // 后端是否真的启用了 GPU 加速。当前 PhysicsKernel 尚未接入
        // gpu_accel 配置（GPU 后端待实现），故始终返回 false，UI 据此如实反馈。
        bool gpu_accel_active() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
