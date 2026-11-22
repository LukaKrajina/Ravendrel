#pragma once
#include <memory>
#include <string>
#include <vector>
#include "render/scene.hpp"
#include "simulation_config.h"

namespace rvdlrsp::gui
{

    struct SceneEntity
    {
        std::string name;
        std::string kind;
        size_t body_index = 0;
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

        bool initialize_all();
        bool loadRobotModel();
        bool initPhysics();
        bool loadSceneEnvironment();
        bool initQuantumDevice();
        bool initBrainInterface();
        bool initQuantumLearningMachine();
        bool initRobotControl();

        const SimulationConfig &config() const;

        void step(float dt);

        void start_teleop();
        void stop_teleop();
        float joint_angle() const;
        void set_joint_angle(float v);
        const std::string &teleop_status() const;

        float gravity() const;
        void set_gravity(float v);
        int solver_iterations() const;
        void set_solver_iterations(int v);
        void pause();
        void step_once();
        const std::string &physics_status() const;

        void start_training();
        void stop_training();
        float rl_progress() const;
        float rl_reward() const;
        int rl_episode() const;
        const std::string &rl_status() const;

        float robot_x() const;
        float robot_y() const;
        float robot_z() const;
        float target_x() const;
        float target_z() const;
        std::string sim_time_str() const;

        const std::vector<render::Mesh> &robot_meshes() const;
        const std::vector<render::SceneInstance> &robot_instances() const;
        size_t robot_part_count() const;
        const std::vector<render::Mesh> &scene_meshes() const;
        const std::vector<render::SceneInstance> &scene_instances() const;
        size_t scene_part_count() const;
        bool set_fractal_params(const FractalTerrain &f);

        const std::vector<SceneEntity> &scene_entities() const;
        void set_entity_position(int index, const qpc::Vec3 &pos);
        void set_entity_rotation(int index, const qpc::Quat &rot);
        void set_entity_scale(int index, const qpc::Vec3 &scale);

        bool import_mesh(const std::string &path);
        bool load_scene_file(const std::string &path);
        bool remove_imported_mesh(int entity_index);
        bool rename_imported_mesh(int entity_index, const std::string &new_name);

        const std::string &quantum_backend() const;
        int quantum_num_qubits() const;
        const std::string &quantum_state() const;
        const std::string &quantum_last_measure() const;
        double quantum_normalization_residual() const;

        float consciousness_awareness() const;
        const std::string &consciousness_state() const;
        const std::string &brain_signal() const;
        const std::string &brain_channel_state() const;

        const std::vector<std::string> &scene_items() const;
        const std::vector<std::string> &circuit_gates() const;
        const std::vector<std::string> &blueprint_nodes() const;
        const std::string &log_text() const;

        int sim_step() const;

        bool gpu_accel_active() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}