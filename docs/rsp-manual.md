# rvdlRSP 量子机器人仿真平台手册

`rvdlRSP` 是一个 C++20 量子机器人仿真平台，统一入口 `#include "rvdlRSP.hpp"`（位于 `RSP/include/`），复用 `qhal`（QM/QVM）、`vedaRos`、`numqk`，覆盖物理、渲染、蓝图、量子 RL、义肢义眼、遥操作到 Qt GUI 与量子图形 UI。

> 本手册按「用什么、怎么调用」组织，诚实说「快速掌握该门编程语言」。每项均标注已实现可用（大部分）或边界限制。

## 目录

- [构建与测试](#构建与测试)
- [物理内核 qpc](#物理内核-qpc)
- [渲染与蓝图](#渲染与蓝图)
- [量子 RL 控制](#量子-rl-控制)
- [qrl：Gymnasium 风格 RL 接口](#qrlgymnasium-风格-rl-接口)
- [义肢与义眼](#义肢与义眼)
- [硬件抽象与遥操作](#硬件抽象与遥操作)
- [Qt 桌面界面](#qt-桌面界面)
- [量子图形 PEQG / 量子 UI PCQW](#量子图形-peqg--量子-ui-pcqw)
- [安全与边界](#安全与边界)

---

## 构建与测试

前置：按 README「构建前必读」配置 LLVM / Kokkos / CUDA / Vulkan / Qt6 路径。

```bash
cd runtime
cmake --build build --target rvdlRSP                      # 仿真平台主程序
cmake --build build --target rvdlRSP_rl_physics_demo      # 量子 RL 端到端示例（需 -DRVDLRSP_BUILD_EXAMPLES=ON）
cmake --build build --target rvdlRSP_tests                # 单元测试（87 用例 / 24 套件）
ctest -R rvdlRSP_tests                                    # 运行测试
```

## 物理内核 qpc

`RSP/include/qpc/`：数学原语 `math.hpp`（`Vec3`/四元数）、刚体 `rigid_body.hpp`、碰撞 `collision.hpp`（球/胶囊/AABB）、约束 `constraint.hpp`、关节 `joint.hpp`（Hinge/BallSocket）、凸包 `convex_hull.hpp`、`PhysicsKernel`（主步进，可选 Kokkos 并行）、`broadphase.hpp`（BVH 宽相）。

**高度场地形**（对标 Genesis Terrain）：

```cpp
TerrainSpec spec;
spec.nx = 3; spec.ny = 3; spec.subterrain_size_x = 12.0;
spec.subterrain_types = { Flat, Sloped, Stairs, Wave, PyramidStairs, Flat,
                          DiscreteObstacles, SteppingStones, Sloped }; // 9 条目行优先
spec.horizontal_scale = 0.25; spec.vertical_scale = 0.005; spec.seed = 42;
Heightfield field(spec);
double h = field.height_at(x,z);
Vec3 n = field.normal_at(x,z);
TerrainBody body(field);
body.place_sphere(x,z,r);
body.support_force(x,y,z,r);
```

> 边界：地形只有高度图碰撞（无 SDF），无法表达悬垂/陡壁；真正窄相（球/盒 vs 高度场）未实现，目前靠 `support_force`。

## 渲染与蓝图

- **render**（`include/render/`）：Vulkan 上下文 / 3D 管线（`vulkan_context.hpp` / `renderer.hpp` / `pipeline3d.hpp`）、场景 / 网格 / 天空盒、OBJ/glTF/glb 加载、纹理 + 内置 PNG/JPEG 解码、PBR（Cook-Torrance）。
- **blueprint**（`include/blueprint/`）：`graph.hpp`（节点图）、`blueprint.hpp`、`blueprint_editor.hpp`（Nuklear 可视化编辑器），行为树 + Material/Substrate 节点图。
- **qpu / circuit**：`include/qpu/qpu_design.hpp`（量子处理器设计）、`include/circuit/robot_circuit.hpp`（机器人电路）。
- **pcg**：`include/pcg/pcg_framework.hpp`，种子驱动程序化内容生成。

## 量子 RL 控制

`RSP/include/control/`：

| 类 | 用途 |
| --- | --- |
| `QuantumRLAgent`（`rl_agent.hpp`） | 策略权重 + 量子探索：`act` / `act_quantum` / `store` / `train`，`set_backend()` 注入后端 |
| `RLPipeline`（`rl_pipeline.hpp`） | `train`：端到端训练（IEnvironment + agent + config） |
| `PhysicsEnvironment` | 接入 RL 的物理环境 |
| `ConsciousnessController` | 脑量子波 bits → 兴奋度 → 增益/阻尼/目标偏移 |
| `FieldConsciousnessController` | 神经场序参量 → 意识调制 |
| `brain_bridge.hpp` / `brain_bridge_qbns.hpp` | 脑机接口桥 / QbNS 桥 |

```cpp
QuantumRLAgent agent(obs_dim, act_dim, 0.01);
agent.set_backend(kernel.backend());      // 注入量子后端
auto action = agent.act_quantum(obs);     // H + Rz(atan(obs_i)*2) 编码，测量作 ± 扰动
```

## qrl：Gymnasium 风格 RL 接口

`include/control/qrl.hpp` / `wrappers.hpp` / `vec_env.hpp`，命名空间 `qrl`。补齐 terminated/truncated 分离、`reset(seed=)`、一等 space、dict info 等规范。

```cpp
#include "control/qrl.hpp"
#include "control/wrappers.hpp"
#include "control/vec_env.hpp"

qrl::EnvSpec spec;
spec.id = "rvdlrsp/Reach-v0";
spec.entry_point = "make_reach_env";
spec.max_episode_steps = 500;
qrl::register_env(spec, [](const qrl::EnvSpec&) {
    return std::make_shared<ReachEnv>();
});
auto env = qrl::make("rvdlrsp/Reach-v0");
auto wrapped = std::make_shared<qrl::RecordEpisodeStatistics>(
                   std::make_shared<qrl::TimeLimit>(env, 500));
auto r = wrapped->reset(42, {});
qrl::StepResult s = wrapped->step(action);
const double bootstrap = s.terminated ? 0.0 : 1.0;   // 正确的 value target
```

**autoreset**：`VectorEnv`（`make_vector_env(64, factory)`）默认 `NextStep` 模式——terminated/truncated 的下一步自动 reset；`ResetOnStep` 为显式。最容易错的是把 `truncated`（超时）当 `terminated`（真终止），两者必须分开处理。

可用包装器：`TimeLimit`、`ClipAction`、`TransformReward`、`NormalizeObservation`、`RecordEpisodeStatistics`、`LegacyEnvAdapter`（旧 `IEnvironment` 适配）。

## 义肢与义眼

```cpp
#include "control/prosthetic_driver.hpp"
double reward = rvdlrsp::control::train_prosthetic_physics(/*episodes=*/50);

ProstheticLimb limb(make_prosthetic_arm_joints());
limb.set_backend(kernel.backend());
limb.set_intent(brain_bits);
limb.step(); limb.feedback(reward); limb.train(epochs);

BionicEye eye;
eye.set_intent(brain_bits);
eye.step();
qpc::Vec3 gaze = eye.gaze_direction();
```

`BionicEyeCamera` 实现 `ICamera`，输出随注视变化的 RGB 帧；`ProstheticRobotDriver` 把义肢关节角映射到刚体。安全与临床合规见 [SAFETY.md](../RSP/SAFETY.md) / [CLINICAL.md](../RSP/CLINICAL.md)。

## 硬件抽象与遥操作

- **hardware**（`include/hardware/`）：`IActuator` + 增量编码器、EMG/EEG 生物信号、`SafetyController`（限位/急停）、故障检测（`Watchdog` / `HeartbeatMonitor` / `EncoderConsistencyCheck` / `DualChannelRedundancy`）、可观测性（Logger / MetricsRegistry / HealthReporter）、HIL 在环。
- **qcdrc**（`include/qcdrc/`）：`ICamera`/`RgbFrame`、全身动捕、真实设备接入、遥操作与驱动、行为克隆。
- **bridge**：`include/bridge/vedaros_bridge.hpp` 与 VedaROS 桥接。

## Qt 桌面界面

`rvdlRSP_gui`（Qt6 Widgets + QVulkanWindow）：`gui/src/simulation_host.hpp/.cpp`（`SimulationHost`：物理 / 遥操作 / RL / QVM / 意识）、`gui/qt/main_window.cpp`（QTabWidget）、`gui/qt/vulkan_viewport.cpp`（3D 视口）、`gui/qt/panels.cpp`（World Outliner / Details）。构建时自动 `windeployqt` 并代码签名。

## 量子图形 PEQG / 量子 UI PCQW

位于 `runtime/include/qhal/`：`QuantumState.hpp`（纯计算，可 headless）、`QuantumGfx.hpp` / `QuantumUi.hpp`（C ABI 实现在 `runtime/src/`）、`QuantumUiNuklear.hpp`（兼容层）。

**投影算子**（`QuantumFrame`）：`LocalBloch`（n=2 无损）、`Quaternionic`、`EntanglementSphere`、`Nested`、`Hypersphere`、`Wigner`、`Density`。常用 API：`dirichlet(x,peaks)`、`amplitude_kernel(i,j,peaks)`、`separable_across(A|B)`、`entanglement_exists(qa,qb)`、`concurrence()`、`pair_entanglement()`、`normalization_residual()`。

C ABI 31 个函数：帧生命周期 `rvdl_qgfx_frame_new/capture/set_amplitudes/release`；谓词 `concurrence/quaternionic/entropy/barcode/same_class`；复值图元 `amplitude/amplitude_phase/interference/phase_hue`；UI `rvdl_qqui_qubit_glyph/qubit_row/entangle_link/uncertainty_bin/wigner_slice/nested_bar/amplitude_strip/state_card`。

```rvdl
// rvdl 接入：须在 cgui_begin_frame / cgui_end_frame 之间
int32 f = qgfx_frame_new();
qgfx_frame_capture(f);
double c = qgfx_concurrence(f);
qqui_entangle_link(/*...*/);
```

## 安全与边界

- 物理阻尼用与步长无关的系数，不硬编码 `*0.999`。
- `concurrence()` 仅 n=2 精确（Wootters），n>2 返回 0；`pair_entanglement()` n>2 恒返回 0——不编造数字。
- Frame 只承载纯态；新帧振幅全零（残差=1.0，表示"尚无数据"），调用前先查 `rvdl_qgfx_frame_residual()`。
- `entanglement_exists(qa,qb)` 在 n>2 问的是二分 `{qa,qb}|其余`，不是两比特单独纠缠。

相关：[SAFETY.md](../RSP/SAFETY.md) · [CLINICAL.md](../RSP/CLINICAL.md) · [EXAMPLES.md](../RSP/EXAMPLES.md)