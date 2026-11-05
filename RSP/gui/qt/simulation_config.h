#pragma once
#include <string>

namespace rvdlrsp::gui
{

    enum class RobotType
    {
        Humanoid,
        Arm,
        Mobile,
        Drone,
        Custom
    };
    enum class SceneTemplate
    {
        Industrial,
        Home,
        Outdoor,
        GroundAtmosphere,
        SpaceStation,
        Custom
    };
    enum class Workspace
    {
        SimDebug,
        RobotControl,
        SceneEdit
    };
    enum class SimBackend
    {
        QVM,
        QM_Superconducting,
        QM_TrappedIon,
        QM_NeutralAtom
    };
    enum class PhysicsEngine
    {
        AlphaPHY_v1,
        AlphaPHY_v2
    };

    // 场景环境物理因素（对应加载第③步：温度/风速/重力/灯光）
    struct SceneEnvironment
    {
        double temperature_c = 22.0;
        double wind_speed = 0.0;   // m/s
        double wind_dir_deg = 0.0; // 风向（度）
        double gravity = -9.81;    // m/s²
        float sun_dir[3] = {0.3f, -1.0f, 0.5f};
        float sun_intensity = 1.2f;
    };

    // RPS 分形地形参数（分形渲染）
    struct FractalTerrain
    {
        bool enabled = true;        // 是否生成分形地形
        int resolution = 128;       // 每边采样点数（W=H）
        double extent = 3.0;        // 截面范围 [-extent, extent]²
        double height_scale = 0.02; // 逃逸计数 → 世界高度系数
        int max_iter = 128;         // 最大迭代次数
        double slice_s = 0.0;       // 固定 s 分量（第三维截面）
    };

    struct SimulationConfig
    {
        std::string project_name = "untitled";
        std::string project_path; // .qrsp.json 绝对路径（持久化用）
        RobotType robot_type = RobotType::Humanoid;
        SceneTemplate scene_template = SceneTemplate::Industrial;
        Workspace workspace = Workspace::SimDebug;
        SimBackend backend = SimBackend::QVM;
        PhysicsEngine physics_engine = PhysicsEngine::AlphaPHY_v1;
        bool gpu_accel = false;
        std::string engine_version = "1.0.0";
        SceneEnvironment env;
        FractalTerrain fractal;        // RPS 分形地形参数
        std::string custom_robot_json; // RobotType::Custom 时使用
    };

    // ─── 枚举 ↔ 字符串 ────────────────────────
    inline std::string to_string(RobotType v)
    {
        switch (v)
        {
        case RobotType::Arm:
            return "Arm";
        case RobotType::Mobile:
            return "Mobile";
        case RobotType::Drone:
            return "Drone";
        case RobotType::Custom:
            return "Custom";
        case RobotType::Humanoid:
        default:
            return "Humanoid";
        }
    }
    inline std::string to_string(SceneTemplate v)
    {
        switch (v)
        {
        case SceneTemplate::Home:
            return "Home";
        case SceneTemplate::Outdoor:
            return "Outdoor";
        case SceneTemplate::GroundAtmosphere:
            return "GroundAtmosphere";
        case SceneTemplate::SpaceStation:
            return "SpaceStation";
        case SceneTemplate::Custom:
            return "Custom";
        case SceneTemplate::Industrial:
        default:
            return "Industrial";
        }
    }
    inline std::string to_string(Workspace v)
    {
        switch (v)
        {
        case Workspace::RobotControl:
            return "RobotControl";
        case Workspace::SceneEdit:
            return "SceneEdit";
        case Workspace::SimDebug:
        default:
            return "SimDebug";
        }
    }
    inline std::string to_string(SimBackend v)
    {
        switch (v)
        {
        case SimBackend::QM_Superconducting:
            return "QM_Superconducting";
        case SimBackend::QM_TrappedIon:
            return "QM_TrappedIon";
        case SimBackend::QM_NeutralAtom:
            return "QM_NeutralAtom";
        case SimBackend::QVM:
        default:
            return "QVM";
        }
    }
    inline std::string to_string(PhysicsEngine v)
    {
        return (v == PhysicsEngine::AlphaPHY_v2) ? "AlphaPHY_v2" : "AlphaPHY_v1";
    }

    inline RobotType robot_type_from_string(const std::string &s)
    {
        if (s == "Arm")
            return RobotType::Arm;
        if (s == "Mobile")
            return RobotType::Mobile;
        if (s == "Drone")
            return RobotType::Drone;
        if (s == "Custom")
            return RobotType::Custom;
        return RobotType::Humanoid;
    }
    inline SceneTemplate scene_template_from_string(const std::string &s)
    {
        if (s == "Home")
            return SceneTemplate::Home;
        if (s == "Outdoor")
            return SceneTemplate::Outdoor;
        if (s == "GroundAtmosphere")
            return SceneTemplate::GroundAtmosphere;
        if (s == "SpaceStation")
            return SceneTemplate::SpaceStation;
        if (s == "Custom")
            return SceneTemplate::Custom;
        return SceneTemplate::Industrial;
    }
    inline Workspace workspace_from_string(const std::string &s)
    {
        if (s == "RobotControl")
            return Workspace::RobotControl;
        if (s == "SceneEdit")
            return Workspace::SceneEdit;
        return Workspace::SimDebug;
    }
    inline SimBackend backend_from_string(const std::string &s)
    {
        if (s == "QM_Superconducting")
            return SimBackend::QM_Superconducting;
        if (s == "QM_TrappedIon")
            return SimBackend::QM_TrappedIon;
        if (s == "QM_NeutralAtom")
            return SimBackend::QM_NeutralAtom;
        return SimBackend::QVM;
    }
    inline PhysicsEngine physics_engine_from_string(const std::string &s)
    {
        return (s == "AlphaPHY_v2") ? PhysicsEngine::AlphaPHY_v2 : PhysicsEngine::AlphaPHY_v1;
    }

    // ─── 显示名（中文 UI 用）──────────────────────────────────
    inline std::string display_name(RobotType v)
    {
        switch (v)
        {
        case RobotType::Arm:
            return "机器臂";
        case RobotType::Mobile:
            return "移动机器人";
        case RobotType::Drone:
            return "无人机";
        case RobotType::Humanoid:
            return "人形机器人";
        case RobotType::Custom:
            return "自定义";
        }
        return "人形机器人";
    }
    inline std::string display_name(SceneTemplate v)
    {
        switch (v)
        {
        case SceneTemplate::Industrial:
            return "工业产线";
        case SceneTemplate::Home:
            return "家庭环境";
        case SceneTemplate::Outdoor:
            return "户外场景";
        case SceneTemplate::GroundAtmosphere:
            return "地面与大气层";
        case SceneTemplate::SpaceStation:
            return "太空与太空站";
        case SceneTemplate::Custom:
            return "自定义";
        }
        return "工业产线";
    }
    inline std::string display_name(Workspace v)
    {
        switch (v)
        {
        case Workspace::SimDebug:
            return "仿真调试";
        case Workspace::RobotControl:
            return "机器人控制";
        case Workspace::SceneEdit:
            return "场景编辑";
        }
        return "仿真调试";
    }
    inline std::string display_name(SimBackend v)
    {
        switch (v)
        {
        case SimBackend::QVM:
            return "QVM（本地量子虚拟机）";
        case SimBackend::QM_Superconducting:
            return "QM（超导）";
        case SimBackend::QM_TrappedIon:
            return "QM（离子阱）";
        case SimBackend::QM_NeutralAtom:
            return "QM（中性原子）";
        }
        return "QVM";
    }
    inline std::string display_name(PhysicsEngine v)
    {
        return (v == PhysicsEngine::AlphaPHY_v2) ? "AlphaPHY 2.0" : "AlphaPHY 1.0";
    }
}