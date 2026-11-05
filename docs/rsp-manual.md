# 量子机器人仿真平台手册（rvdlRSP）

> `rvdlRSP/` 是一个量子机器人仿真平台（C++20），复用运行时能力 `qhal`（QM/QVM 量子后端）、`vedaRos`（量子机器人 OS）与 `numqk`（张量库）。
> 统一入口：`#include "rvdlRSP.hpp"`。
> 本手册覆盖各子系统的架构、关键类型、义肢/义眼专项、构建测试与安全合规。

---

## 目录

1. [架构总览](#1-架构总览)
2. [物理内核 qpc](#2-物理内核-qpc)
3. [渲染 render](#3-渲染-render)
4. [蓝图 blueprint](#4-蓝图-blueprint)
5. [量子处理器设计 qpu 与电路 circuit](#5-量子处理器设计-qpu-与电路-circuit)
6. [程序化内容生成 pcg](#6-程序化内容生成-pcg)
7. [控制与量子强化学习 control](#7-控制与量子强化学习-control)
7b. [RL 对齐层 qrl（Gymnasium 兼容）](#7b-rl-对齐层-qrlgymnasium-兼容)
8. [义肢 / 义眼（脑意识 + 量子 RL）](#8-义肢--义眼脑意识--量子-rl)
9. [硬件抽象 hardware](#9-硬件抽象-hardware)
10. [QCDRC 遥操作](#10-qcdrc-遥操作)
11. [桥接 bridge](#11-桥接-bridge)
12. [Qt 桌面仿真界面](#12-qt-桌面仿真界面)
13. [构建与测试](#13-构建与测试)
14. [安全与临床合规](#14-安全与临床合规)
15. [量子图形与量子 UI（PEQG / PCQW）](#15-量子图形与量子-uipeqg--pcqw)

---

## 1. 架构总览

```
rvdlRSP/
├── include/
│   ├── core/        世界 / 骨架 / 机器人 / 混沌网格
│   ├── qpc/         物理内核（刚体 / 碰撞 / 约束 / 关节 / 凸包 / BVH 宽相）
│   ├── qpu/         量子处理器设计
│   ├── circuit/     机器人电路设计
│   ├── blueprint/   行为树 + 节点图蓝图
│   ├── pcg/         程序化内容生成
│   ├── render/      3D 渲染（Vulkan PBR + 网格/纹理加载）
│   ├── editor/      rvdl 编辑器
│   ├── control/     量子 RL + 意识控制 + 义肢/义眼 + 场意识
│   ├── hardware/    执行器 / 生物信号 / 安全 / 故障检测 / HIL
│   ├── bridge/      VedaROS 桥
│   └── qcdrc/       相机 / 动捕 / 遥操作 / 行为克隆
├── src/             入口、Vulkan 上下文、JIT host、RL demo
├── gui/             Qt 6 仿真界面
├── shaders/         PBR 着色器（GLSL）
└── tests/           单元测试
```

---

## 2. 物理内核 qpc

位置：`include/qpc/`。参考 AlphaPHY 的刚体动力学 + 碰撞检测 + 约束求解，量子接口委托给 QM/QVM。

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| 数学 | `math.hpp` | 向量 / 四元数等数学原语 |
| 刚体 | `rigid_body.hpp` | 刚体状态（位置 / 朝向 / 速度 / 阻尼） |
| 碰撞 | `collision.hpp` | 球体 / 胶囊体 / AABB 等形状碰撞 |
| 约束 | `constraint.hpp` | 约束求解 |
| 关节 | `joint.hpp` | 关节类型（Hinge / BallSocket 等） |
| 凸包 | `convex_hull.hpp` | 凸包生成 |
| 物理内核 | `physics_kernel.hpp` | 主物理步进（可选 Kokkos 并行积分） |
| 宽相 | `broadphase.hpp` | BVH 宽相碰撞（O(N²) → O(N log N)） |
| 地形 | `terrain.hpp` | 高度场地形（9 种子地形，种子驱动可复现） |

> 物理阻尼使用与步长无关的阻尼系数（`linear_damping` / `angular_damping`），不再硬编码 `*0.999`。

### 2.1 高度场地形（Genesis `Terrain` 对标）

Genesis 用 `gs.morphs.Terrain` 在 `build()` 时把地形烘焙为渲染网格 + 碰撞高度图/SDF。
`qpc/terrain.hpp` 采用同一思路，接口形态保持一致：

```cpp
TerrainSpec spec;
spec.nx = 3;  spec.ny = 3;                       // 3×3 子地形
spec.subterrain_size_x = 12.0;                   // 每块 12m × 12m
spec.subterrain_types = {                         // 9 个条目，行优先
    SubterrainType::Flat,       SubterrainType::Sloped,      SubterrainType::Stairs,
    SubterrainType::Wave,       SubterrainType::PyramidStairs, SubterrainType::Flat,
    SubterrainType::DiscreteObstacles, SubterrainType::SteppingStones, SubterrainType::Sloped,
};
spec.horizontal_scale = 0.25;                     // 水平采样间距
spec.vertical_scale = 0.005;                      // 高度量化步长
spec.seed = 42;                                   // 固定 seed ⇒ 完全可复现

Heightfield field(spec);
const double h  = field.height_at(x, z);          // O(1) 双线性插值
const Vec3   n  = field.normal_at(x, z);          // O(1) 中心差分
const double sp = field.max_height();             // spawn 用安全高度

TerrainBody body(field);                          // 与物理内核的接线
body.place_sphere(x, z, r);                       // 把半径 r 的球放到地面上
body.support_force(x, y, z, r);                   // 坡面法线方向的支撑力（含截断）
```

9 种子地形命名与 Genesis 一致（`flat_terrain` / `sloped_terrain` / `stairs_terrain` …），
便于迁移；`subterrain_type_by_name()` 提供反向查找。

**三点如实记录：**

1. 只有**高度图**碰撞（沿 Y 轴的 height-vs-height），**没有 SDF**。代价是无法表达
   overhang（洞穴/悬垂）与陡峭壁面；收益是查询恒为 O(1)，且对机器人仿真的典型地形
   （斜坡/楼梯/起伏）已足够。这是刻意的取舍。
2. 随机化放在 `TerrainSpec::seed` 上，而非 Genesis 的 `Terrain(randomize=True)` 构建期参数。
   语义等价但更显式，且构建期随机化需要固定 seed 才能复现。
3. Terrain **不是碰撞体**，而是碰撞查询的数据源——与 Genesis 一致。真正的窄相
   （球/盒 vs 高度场）未实现；目前由 `TerrainBody::support_force` 提供支撑力模型。
   把 4×4 窄相接入 `PhysicsKernel` 是后续工作。

**子地形独立性**：每个子地形用独立的 RNG 流，因此「只改第 3 块地形」不会连带改变
第 7 块的形状。这一点由 `terrain_mixed_subterrain_blocks_independent` 固定。

---

## 3. 渲染 render

位置：`include/render/`。3D 渲染 + PBR（Cook-Torrance）+ 纹理采样 + OBJ/glTF/glb 加载 + 内置 PNG/JPEG 解码。

| 组件 | 说明 |
| --- | --- |
| `vulkan_context.hpp` / `renderer.hpp` / `pipeline3d.hpp` | Vulkan 渲染上下文与 3D 管线 |
| `scene.hpp` / `mesh.hpp` / `mesh_generator.hpp` / `sky.hpp` | 场景 / 网格 / 天空盒 |
| `obj_loader.hpp` / `gltf_loader.hpp` / `mesh_loader.hpp` | 模型加载 |
| `texture.hpp` / `texture_decoder.hpp` | 纹理与 PNG/JPEG 解码 |
| `mat4.hpp` / `json.hpp` | 数学与 JSON |

---

## 4. 蓝图 blueprint

位置：`include/blueprint/`。行为树 + Material/Substrate 节点图 + Nuklear 可视化编辑器。

| 组件 | 说明 |
| --- | --- |
| `graph.hpp` | 节点图 |
| `blueprint.hpp` | 蓝图定义 |
| `blueprint_editor.hpp` | Nuklear 可视化编辑器 |

---

## 5. 量子处理器设计 qpu 与电路 circuit

| 子系统 | 位置 | 说明 |
| --- | --- | --- |
| QPU/QPL 设计 | `include/qpu/qpu_design.hpp` | 量子处理器与量子编程层设计器 |
| 机器人电路 | `include/circuit/robot_circuit.hpp` | 机器人电路设计与连接 |

---

## 6. 程序化内容生成 pcg

位置：`include/pcg/pcg_framework.hpp`。种子驱动程序化内容生成（PCG）。

---

## 7. 控制与量子强化学习 control

位置：`include/control/`。这是平台最核心的子系统之一。

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| 控制入口 | `control.hpp` | 子系统的统一入口 |
| 量子 RL 智能体 | `rl_agent.hpp` | `QuantumRLAgent`：策略权重 + 量子探索（`act` / `act_quantum` / `store` / `train`） |
| RL 管线 | `rl_pipeline.hpp` | `RLPipeline::train`：端到端训练循环（`IEnvironment` + agent + config） |
| 物理环境 | `physics_environment.hpp` | 物理环境（接入 RL 的 `IEnvironment`） |
| 意识控制 | `consciousness_controller.hpp` | `ConsciousnessController`：脑量子波 bits → 兴奋度 → 增益/阻尼/目标偏移 |
| 场意识控制 | `field_consciousness.hpp` | `FieldConsciousnessController`：神经场序参量 → 意识调制 |
| 脑机桥 | `brain_bridge.hpp` | 脑机接口桥 |
| 脑机桥（QbNS） | `brain_bridge_qbns.hpp` | 桥接 QbNS 量子脑网络 |
| 义肢/义眼 | `prosthetic.hpp` / `prosthetic_driver.hpp` | 见 [第 8 节](#8-义肢--义眼脑意识--量子-rl) |
| 异步管线 | `async_pipeline.hpp` | 异步训练管线 |

### 7.1 量子强化学习

`QuantumRLAgent` 在经典线性策略之上引入量子探索：

- `act(obs)`：经典策略（`obs → action` 线性映射）。
- `act_quantum(obs)`：将观测编码进量子态（`H + Rz(atan(obs_i)·2)`），测量结果作为 ±探索扰动。
- `store(obs, action, reward)` / `train(epochs)`：经验回放 + 监督式策略修正。

```cpp
QuantumRLAgent agent(obs_dim, act_dim, 0.01);
agent.set_backend(kernel.backend());   // 注入量子后端
auto action = agent.act_quantum(obs);
```

### 7.2 意识控制

`ConsciousnessController::compute(brain_bits)` 将脑量子波测量结果（0/1 序列）映射为意识调制因子：

| 调制 | 范围 | 含义 |
| --- | --- | --- |
| `arousal` | [0,1] | 归一化兴奋度 |
| `gain_scale` | [0.7,1.3] | 兴奋度高 → 增益放大 |
| `damping_scale` | [0.7,1.3] | 兴奋度低 → 阻尼提高 |
| `target_offset_x/z` | — | 意识驱动的目标偏移 |

---

## 7b. RL 对齐层 qrl（Gymnasium 兼容）

位置：`include/control/qrl.hpp`、`wrappers.hpp`、`vec_env.hpp`，命名空间 `qrl`。

### 7b.1 为什么需要这一层

此前的 RL 接口是玩具级的：`reset() → obs`、`step() → StepResult{obs, reward, done}`。
与 [Gymnasium](https://gymnasium.farama.org/) 相比，它缺的不是功能，而是**一组被广泛实现的约定**。
Gymnasium 成为标准（论文 [arXiv:2407.17032](https://arxiv.org/abs/2407.17032)）正是因为它把七件事定成规范：

| # | 约定 | 若不遵守的后果 |
| --- | --- | --- |
| 1 | `terminated` / `truncated` **分离** | value target 算错：应为 `r + γV(s')(1 − terminated)` |
| 2 | `reset(seed=…)` 而非独立 `seed()` | 「每回合不同种子」不可行，整次训练无法复现 |
| 3 | space 是带 `contains()` 的一等类型 | 越界动作静默饱和，训练悄悄退化 |
| 4 | `info` 为 dict | 奖励分解需改环境代码才能记录 |
| 5 | 每步返回**全新** observation | replay buffer 持有被覆写的缓冲区 |
| 6 | render mode 构造时固定 | 部分后端无法中途切换 |
| 7 | `metadata["render_fps"]` 诚实声明 | 视频录制时序错误 |

> **与 Genesis 的对比（重要）**：Genesis **没有**实现 `gymnasium.Env`。其 `Go2Env.step()`
> 返回 4 元组 `(obs, rew, reset_buf, extras)`（done 合并），观测是 `TensorDict` 而非
> `spaces.Box`。因此「量子仿真平台 + 标准 RL API」这一组合目前是空位，`qrl::*` 正针对该空位。

### 7b.2 核心类型

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| `Space` / `Box` / `Discrete` | `qrl.hpp` | 带 `contains()`（含 NaN/Inf 判定）与 `sample(rng)` 的一等空间 |
| `Env` | `qrl.hpp` | `reset(seed, options) → (obs, info)`；`step → StepResult{terminated, truncated, info}` |
| `EnvSpec` / `Registry` / `register_env` / `make` | `qrl.hpp` | 按 id 构造环境，使策略代码可跨环境复用 |
| `Wrapper` 基类 | `wrappers.hpp` | 包装器链，`unwrapped()` 逐层下钻 |
| `TimeLimit` | `wrappers.hpp` | 超时产生 **`truncated`**，不触碰 `terminated` |
| `ClipAction` / `TransformReward` / `NormalizeObservation` | `wrappers.hpp` | 动作钳制 / 奖励仿射变换 / Welford 在线标准化 |
| `RecordEpisodeStatistics` | `wrappers.hpp` | 回合结束时把统计写入 `info`，记录器零改动 |
| `LegacyEnvAdapter` | `wrappers.hpp` | 把旧 `IEnvironment` 适配为 `qrl::Env`，**不破坏**既有环境 |
| `VectorEnv` / `SyncVectorEnv` / `make_vector_env` | `vec_env.hpp` | 批量 reset/step，`AutoresetMode` 三态 |

### 7b.3 autoreset 语义（最容易搞错的一处）

`AutoresetMode::NextStep`（默认，对齐 Gymnasium）：子环境终止后**下一步**重置，
且终止当步仍返回最终观测（RL 需要它算 bootstrap 项）。

```cpp
auto v = make_vector_env(64, [] { return std::make_shared<MyEnv>(); });
v->reset(std::vector<std::optional<uint64_t>>{std::optional<uint64_t>(42)}); // 子环境 i 收 seed+i
auto s = v->step(batch_actions);   // s.terminated / s.truncated / s.info 全部按子环境对齐
```

`SAME_STEP` 当步即重置（会丢失终止态观测，故非默认）；`DISABLED` 要求外部手工重置
（此时对未重置的子环境调用 `step()` 会抛异常而非静默产出错误轨迹）。

### 7b.4 使用示例

```cpp
#include "control/qrl.hpp"
#include "control/wrappers.hpp"
#include "control/vec_env.hpp"

qrl::EnvSpec spec;
spec.id = "rvdlrsp/Reach-v0";
spec.entry_point = "make_reach_env";
spec.max_episode_steps = 500;
qrl::register_env(spec, [](const qrl::EnvSpec &) {
    return std::make_shared<ReachEnv>();
});

auto env = qrl::make("rvdlrsp/Reach-v0");
auto wrapped = std::make_shared<qrl::RecordEpisodeStatistics>(
                   std::make_shared<qrl::TimeLimit>(env, 500));
auto r = wrapped->reset(42, {});
qrl::StepResult s = wrapped->step(action);
const double bootstrap = s.terminated ? 0.0 : 1.0;   // 正确的 value target
```

---

## 8. 义肢 / 义眼（脑意识 + 量子 RL）

位置：`include/control/prosthetic.hpp` / `prosthetic_driver.hpp`。
义肢由脑意识提供「意图」目标角，量子 RL 学习补偿动作；义眼由脑意识控制注视方向，量子 RL 学习追踪补偿。

### 8.1 核心组件

| 组件 | 说明 |
| --- | --- |
| `ProstheticLimb` | 义肢运动单元：`set_intent`（脑意识 bits → 目标角）、`step`（量子 RL 补偿）、`feedback` / `train` |
| `BionicEye` | 义眼：`set_intent`（兴奋度 → pan/tilt）、`step`（追踪补偿）、`gaze_direction()`（单位注视向量） |
| `BionicEyeCamera` | 义眼相机：实现 `ICamera`，针孔投影生成随注视方向变化的 RGB 帧 |
| `ProstheticRobotDriver` | 义肢 → Robot 刚体驱动（关节角 → 骨骼朝向） |
| `ProstheticEnvironment` | 义肢 RL 环境（`IEnvironment`） |
| `ProstheticPhysicsEnvironment` | 义肢 + 物理内核的端到端 RL 环境 |
| `make_prosthetic_arm_joints()` | 标准上肢关节工厂（肩/肘/腕/拇指/食指） |

### 8.2 示例：端到端训练

```cpp
#include "control/prosthetic_driver.hpp"
double reward = rvdlrsp::control::train_prosthetic_physics(/* episodes= */ 50);
```

### 8.3 关键接口

```cpp
// 义肢
ProstheticLimb limb(make_prosthetic_arm_joints());
limb.set_backend(kernel.backend());
limb.set_intent(brain_bits);   // 脑意识意图
limb.step();                   // 量子 RL 补偿 + 关节推进
limb.feedback(reward);
limb.train(epochs);

// 义眼
BionicEye eye;
eye.set_intent(brain_bits);
eye.step();
qpc::Vec3 gaze = eye.gaze_direction();
```

---

## 9. 硬件抽象 hardware

位置：`include/hardware/`。

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| 执行器 | `actuator.hpp` | 统一电机接口（`IActuator`）+ 增量编码器采样 |
| 生物信号 | `bio_signal.hpp` | EMG/EEG 信号源（仿真 / 外部） |
| 安全控制器 | `safety_controller.hpp` | `SafetyController`：限位 / 急停 |
| 故障检测 | `fault_detection.hpp` | `Watchdog` / `HeartbeatMonitor` / `EncoderConsistencyCheck` / `DualChannelRedundancy` |
| 可观测性 | `observability.hpp` | `Logger` / `MetricsRegistry` / `HealthReporter` |
| HIL 在环 | `hil_harness.hpp` | 硬件在环测试 |

---

## 10. QCDRC 遥操作

位置：`include/qcdrc/`。

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| 相机 | `camera.hpp` | RGB 相机（`ICamera` / `RgbFrame`） |
| 动捕 | `mocap.hpp` | 全身动捕 |
| 设备 | `device.hpp` | 真实设备接入 |
| 遥操作 | `teleop.hpp` / `teleop_driver.hpp` | 遥操作与驱动 |
| 行为克隆 | `cloning.hpp` | 行为克隆 |

---

## 11. 桥接 bridge

位置：`include/bridge/vedaros_bridge.hpp`。将 rvdlRSP 与 VedaROS 量子机器人 OS 桥接（详见 [README](../README.md) 的 VedaROS 章节）。

---

## 12. Qt 桌面仿真界面

`rvdlRSP_gui` 基于 Qt 6 Widgets + QVulkanWindow。

| 组件 | 位置 | 说明 |
| --- | --- | --- |
| 仿真内核 | `gui/src/simulation_host.hpp/.cpp` | `SimulationHost`：物理步进 / 遥操作 / RL / QVM / 意识 |
| 主窗口 | `gui/qt/main_window.cpp` | QTabWidget 面板布局 |
| Vulkan 视口 | `gui/qt/vulkan_viewport.cpp` | QVulkanWindow 真 3D 视口 |
| 面板 | `gui/qt/panels.cpp` | World Outliner / Details 面板 |

---

## 13. 构建与测试

```bash
cd runtime
cmake --build build --target rvdlRSP                      # 仿真平台主程序
cmake --build build --target rvdlRSP_rl_physics_demo      # 量子 RL 端到端示例（需 -DRVDLRSP_BUILD_EXAMPLES=ON）
cmake --build build --target rvdlRSP_tests                # 单元测试
ctest -R rvdlRSP_tests                                    # 运行测试
```

> 构建前请先按 [README 构建前必读](../README.md) 配置依赖路径（LLVM / Kokkos / CUDA / Vulkan / Qt 6 等）。

---

## 14. 安全与临床合规

| 文档 | 说明 |
| --- | --- |
| [SAFETY.md](../rvdlRSP/SAFETY.md) | ISO 14971 风险分析（H1-H7 危险源）+ FMEA + 安全用例（SC1-SC5） |
| [CLINICAL.md](../rvdlRSP/CLINICAL.md) | FDA De Novo/510(k)、CE MDR 临床评估方案（有效性/安全性/可用性终点） |

关键安全组件（`SafetyController` / `Watchdog` / `EncoderConsistencyCheck` / `DualChannelRedundancy` / `HeartbeatMonitor`）见 [第 9 节](#9-硬件抽象-hardware)。

---

> 相关文档：[README](../README.md) · [rvdl 语言手册](./rvdl-language-manual.md) · [rvdl 量子学习手册](./rvdl-quantum-learning-manual.md)

---

## 15. 量子图形与量子 UI（PEQG / PCQW）

### 15.1 定位：补齐哪一层

rvdl 已有两套**经典**图形层：

| 层 | ABI | 内容 |
| --- | --- | --- |
| `cgfx_*` | `runtime/include/qhal/ClassicGfx.hpp` | 2D 通用原语（矩形/线/**折线**/椭圆/三角形） |
| `cgui_*` | `runtime/include/qhal/ClassicGui.hpp` | 立即模式 GUI 控件（按钮/文本/面板） |

两者都是**应用无关**的绘图原语，无法表达量子语义——它们能画「一根线」，
但不能说明这根线代表「纠缠」还是「可分离」。

`qgfx_*` 与 `qqui_*` 补齐的正是这一层：**以量子态为一等公民**的绘图原语与控件。

| 层 | 头文件 | 实现 |
| --- | --- | --- |
| 状态模型（纯计算，无图形依赖） | `runtime/include/qhal/QuantumState.hpp` | 头文件内联 |
| 量子图形引擎 ABI | `runtime/include/qhal/QuantumGfx.hpp` | `runtime/src/QuantumGfx.cpp` |
| 量子 UI 库 ABI | `runtime/include/qhal/QuantumUi.hpp` | `runtime/src/QuantumUi.cpp` |
| Nuklear 兼容层 | — | `runtime/include/qhal/QuantumUiNuklear.hpp` |

`QuantumState.hpp` **不依赖 Vulkan / Nuklear / 任何图形 API**，仅用标准库，
因此可独立 headless 单元测试（`rvdlRSP/tests/test_qgfx.cpp`）。

### 15.2 范式一：显式投影的量子图形学（PEQG）

#### 立论

n 量子比特的纯态 living 在 CP^(2^n−1) 中，实维数 `2(2^n−1)`：

| n | 实维数 | 后果 |
| --- | --- | --- |
| 1 | 2 | Bloch 球 S²，**恰好完整** |
| 2 | 6 | 超过任何 2D 平面 → Bloch 类图必为有损投影 |
| 3 | 14 | 同上，且差距迅速扩大 |

因此**不存在**等距的「量子态图像」。文献中所有「Bloch 球推广」（超球 / 八元数 /
双 Bloch / 2^n−1 嵌套球）都是**有意的子空间投影**，而非完整的可视化。

#### 五条设计原则

| 原则 | 内容 | 依据 |
| --- | --- | --- |
| **P1** 投影显式化 | 投影算子是 API 的一等参数，可在运行期反查「丢了多少自由度」 | arXiv:2212.03448、arXiv:2602.14661 |
| **P2** 谓词可读化 | 每个投影自带 O(1) 代数判据，可直接从图形读出真伪 | arXiv:1512.07006、arXiv:2305.07596 (PRR 6, 023077) |
| **P3** 复值精确图元 | 幅度图元用 \|a\| 控强度、arg(a) 控**干涉相位**，权重取 Dirichlet 核 | arXiv:2610.00618 (ACM TOG 2026) |
| **P4** 层级内蕴 | 嵌套球把 2^n 个振幅压成 2^(n−1) 个同心壳，20 比特态仍可读 | arXiv:2607.04979、arXiv:1905.00041 |
| **P5** 一次模拟多视图 | 所有原语只读同一份 frame 快照，切换视图零重算 | — |

#### P3 为什么重要

Dirichlet splatting（arXiv:2610.00618）证明了：有限窗 DFT 的真实 PSF 是
**Dirichlet 核**（复值、振荡）而非高斯。用高斯图元会静默丢弃 10–20% 的旁瓣能量
以及支配相干干涉的相位。对振幅可视化同理——**丢掉相位即丢掉干涉，而干涉正是
量子态区别于经典分布的信息**。

```cpp
// 精确、闭式、振荡。x→0 的洛必达极限 = 2N+1（不是 (N+½)²）
static double QuantumFrame::dirichlet(double x, int peaks);
// 相位加权的复值核：干涉项 ∝ a_i a_j*（经典直方图此值恒为 0）
qcomplex    QuantumFrame::amplitude_kernel(size_t i, size_t j, int peaks);
```

#### 投影算子一览

| 投影 | 目标独立维数（n=2 / n=3） | 说明 |
| --- | --- | --- |
| `LocalBloch` | 6 / 9 | 局域 Bloch 向量；n=2 时**恰好无损** |
| `Quaternionic` | 3 / 3 | 立体投影；四元数部分 = concurrence/2 |
| `EntanglementSphere` | 6 / 9 | 3 球 × 3 坐标，但受秩-1 约束 → 以源维数封顶 |
| `Nested` | 3 / 3 | 任一时刻只画最内层壳 |
| `Hypersphere` | 3 / 3 | Bloch 超球；距离 = Bures 度量 |
| `Wigner` | 2 / 2 | 相空间单截面 |
| `Density` | 6 / 14 | 纯态 rank-1 → 独立参数 = 源维数 |

> **记账口径**：返回的是**独立参数个数**，而非图元上画出的坐标个数。两者在
> `EntanglementSphere` 上不同（画 9 个坐标但只有 6 个独立参数）。契约保证
> `target_dims ≤ source_dims` 恒成立，因此该投影不会被误报为「增维」。

### 15.3 范式二：谓词耦合的量子控件（PCQW）

`cgui_*` 是经典立即模式控件，没有「量子」概念。`qqui_*` 在控件层引入四条新原则：

| 原则 | 内容 | 依据 |
| --- | --- | --- |
| **W1** 控件携带谓词 | 纠缠连线**宽度**编码纠缠量；可分离时转为灰色低注意力 → 「不纠缠」是**前注意**信号，无需读数字 | arXiv:2305.07596 |
| **W2** 不确定性内生 | 概率 bin 的**宽度**编码 shot noise（置信度的几何属性），1σ 区间以顶帽表示 | arXiv:2504.12776、arXiv:2508.00937 |
| **W3** 布局由量子结构决定 | 嵌套条布局完全由比特数推出，调用者**不选 LOD** | arXiv:2607.04979 |
| **W4** 交互式图元优先 | `rvdl_qqui_amplitude_strip` 返回被选中的基态索引，使「点一个振幅，看它变成什么概率」闭环成立 | arXiv:2311.07980、arXiv:1808.00532 |

**W1 的口径统一**：纠缠量与「是否纠缠」由**两个不同**的谓词提供
（`pair_entanglement` 给强度，`entanglement_exists` 给二分），二者不可互相冒充。
UI 不会「看起来纠缠」而「算出来可分离」，也不会反之。

### 15.4 纠缠判据的三层语义

这是本节最容易被误用的部分，因此在 API 与测试中双重固定：

| 层 | API | 语义 | 在纯态下 |
| --- | --- | --- | --- |
| **L1** | `separable_across(A\|B)` | 二分 A\|B 是否可分离 | Schmidt 秩 1 ⟺ 可分离，**充要** |
| **L2** | `all_marginals_mixed`（测试中实现） | 全部单比特边缘混合 ⟹ 真多体纠缠 | **充分**（Koenig–Winter / Gühne–Vidal） |
| **L3** | — | 二可分：可分为 A⊗BC 且 B\|C 也可分离 | 例：Bell(q0q1) ⊗ \|0⟩(q2) |

**两条必须记住的边界：**

1. **Gühne–Vidal 判据是充分非必要条件。** 某个边缘**不**混合 **不能**推出可分离。
   反例（`gvh_vidal_witness_is_sufficient_not_necessary` 固定）：Bell(q0q1)⊗\|0⟩(q2) 的
   q2 边缘是纯态，但 q0\|q1 仍然纠缠。UI 不得把「某边缘不混合」渲染成「可分离」。

2. **`entanglement_exists(qa, qb)` 在 n>2 时问的是二分 `{qa,qb} | 其余`**，
   **不是**「qa 与 qb 这两个比特单独是否纠缠」。Bell(q0q1)⊗\|0⟩(q2)：
   - `entanglement_exists(0, 0)` → `true`（{q0}\|{q1,q2}，Bell 对跨此切分）
   - `entanglement_exists(0, 1)` → `false`（{q0,q1}\|{q2}，张量积）

   要问「单比特 q 与其余」，请传 `entanglement_exists(q, q)`。

**Frame 的契约**：只承载**纯态**振幅向量。statevector 模拟器
（`backend->get_state_vector()`）产出的正是纯态；混态需 4^n 存储，不属同一抽象层。
框架以 `normalization_residual()` 显式暴露这一点，**不假装能画混态**。

新帧的振幅为**全零**（非物理，归一化残差 = 1.0），刻意**不是** \|0…0⟩：
零向量如实标记「尚无数据」，而 \|0…0⟩ 会伪装成合法真空态。调用方应先查
`rvdl_qgfx_frame_residual()` 再信任任何谓词读数。

### 15.5 度量的诚实边界

| API | n=2 | n>2 | 说明 |
| --- | --- | --- | --- |
| `concurrence()` | 精确（Wootters，O(1)） | 返回 0 | n≠2 无定义，**不编造数字** |
| `pair_entanglement()` | = concurrence | **恒返回 0（不可用）** | 见下 |
| `separable_across()` | 充要 | 充要 | Schmidt 秩判据 |
| `entanglement_exists()` | 充要 | 充要 | 语义见 15.4 |
| 纠缠条码 | — | 编码非增信息 | 见下 |

**为何 `pair_entanglement` 不外推到 n>2**：自然的外推是 Schmidt 间隙 λ₀−λ₁，
但它**错误**——Bell 态的谱是 {1/√2, 1/√2}，间隙为 0，而 Bell 恰是最大纠缠
（concurrence = 2λ₀λ₁ = 1）。任何只依赖「谱的单参数差」的度量都无法同时给出
Bell = 1 与可分离 = 0。正确的 n>2 方案需构造该比特对的 **4×4 约化密度矩阵**并用
Wootters 混态公式（需复厄米特征值），超出本 Frame 的 O(1) 预算。
故此处**明确不提供**，而非返回一个似是而非的数字——后者会让 UI 悄悄画出错误的纠缠强度。

**关于纠缠条码的诚实说明**：Hamilton–Leditzky（arXiv:2307.07492）以形变总关联为
滤流的持久同调条码，其「比标量纠缠度量更具区分力」的严格结论适用于**混态**。
本 Frame 只承载纯态，Schmidt 定理保证谱已含全部纠缠信息，故纯态下条码的作用是
**编码能力**而非信息量：条码把谱压成 (birth, death) 对，天然支持比较/排序/哈希，
而 concurrence 把谱压成单个数、丢失谱形状。因此在 **Schmidt 秩相同但谱形状不同**
的态对之间，条码判据仍能区分，而秩本身不能。这是「条码 > 秩」的准确表述；
若称其「比 concurrence 更有信息量」，在纯态情形下并不成立。

### 15.6 ABI 一览（31 个函数）

```c
// 帧生命周期（句柄 = u64，高位编码世代号；失效句柄安全返回 0/null）
uint64_t rvdl_qgfx_frame_new(void);
int32_t  rvdl_qgfx_frame_capture(uint64_t h, void *backend, int32_t num_qubits);
void     rvdl_qgfx_frame_set_amplitudes(uint64_t h, const double *re_im, int32_t count, int32_t n);
void     rvdl_qgfx_frame_release(uint64_t h);

// 诊断与投影内省（P1）
double   rvdl_qgfx_frame_residual(uint64_t h);
int32_t  rvdl_qgfx_frame_num_qubits(uint64_t h);
const char *rvdl_qgfx_projection_name(int32_t projection);
double   rvdl_qgfx_projection_source_dims(int32_t num_qubits);
double   rvdl_qgfx_projection_target_dims(int32_t projection, int32_t num_qubits);

// 谓词（P2）
double   rvdl_qgfx_concurrence(uint64_t h);
int32_t  rvdl_qgfx_quaternionic(uint64_t h, double *out4);
double   rvdl_qgfx_entropy(uint64_t h, int32_t first, int32_t count, int32_t whole);
int32_t  rvdl_qgfx_barcode(uint64_t h, int32_t first, int32_t count, double *bars, int32_t max);
int32_t  rvdl_qgfx_same_class(uint64_t a, uint64_t b, int32_t first, int32_t count, double tol);

// 复值精确幅度图元（P3）
double   rvdl_qgfx_amplitude(uint64_t h, int32_t basis, int32_t peaks);
double   rvdl_qgfx_amplitude_phase(uint64_t h, int32_t basis);
double   rvdl_qgfx_interference(uint64_t h, int32_t i, int32_t j);
double   rvdl_qgfx_phase_hue(double phase);

// 嵌套层级（P4）
int32_t  rvdl_qgfx_nested_shells(int32_t num_qubits);
double   rvdl_qgfx_nested_mass(uint64_t h, int32_t shell, int32_t total);
void     rvdl_qgfx_bloch(uint64_t h, int32_t qubit, double *out2);
double   rvdl_qgfx_probability(uint64_t h, int32_t basis);

// 量子 UI（PCQW）
void     rvdl_qqui_qubit_glyph(int32_t x, int32_t y, int32_t size, uint64_t h, int32_t q, int32_t c, int32_t a);
void     rvdl_qqui_qubit_row(int32_t x, int32_t y, int32_t cell, uint64_t h, int32_t first, int32_t count, int32_t c, int32_t a);
double   rvdl_qqui_entangle_link(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint64_t h, int32_t qa, int32_t qb, int32_t c, int32_t a);
void     rvdl_qqui_uncertainty_bin(int32_t x, int32_t baseline_y, int32_t slot_w, double p, double sigma, int32_t c, int32_t a);
void     rvdl_qqui_wigner_slice(int32_t x, int32_t y, int32_t w, int32_t h, uint64_t f, int32_t first, int32_t res);
void     rvdl_qqui_nested_bar(int32_t x, int32_t y, int32_t cell, uint64_t h, int32_t c, int32_t a);
int32_t  rvdl_qqui_amplitude_strip(int32_t x, int32_t y, int32_t w, int32_t h, uint64_t f, int32_t c, int32_t a);
void     rvdl_qqui_state_card(int32_t x, int32_t y, int32_t w, uint64_t h, int32_t projection, int32_t c, int32_t a);
```

### 15.7 rvdl 语言接入

已注册进 MIR 声明表（`MirModuleBuilder.hpp`）、JIT 符号表（`JIT.hpp`）与
TextMate 语法高亮（`syntaxes/rvdl.tmLanguage.json`）。rvdl 源码中直接使用：

```
qgfx_frame_new()                       // 返回 u64 句柄
qgfx_frame_capture(h, backend, 3)      // 从量子后端捕获振幅
qgfx_concurrence(h)                    // 两比特纠缠读数
qgfx_quaternionic(h, coords)           // 取出四元数坐标供绘制
qgfx_same_class(h1, h2, 0, 1, 1e-6)   // 状态同一性判据

qqui_entangle_link(x0, y0, x1, y1, h, 0, 1, 0xFF4040, 255)
qqui_amplitude_strip(x, y, w, h, frame, 0xFFFFFF, 200)
```

> 调用须在 `cgui_begin_frame` / `cgui_end_frame` 之间（与 cgfx/cgui 共用同一
> Nuklear 窗口）。经典图层与量子图层可在同一帧内任意混用。

### 15.8 测试

| 文件 | 覆盖 |
| --- | --- |
| `tests/test_qgfx.cpp`（21 项） | 帧生命周期与句柄安全、投影内省、谓词、Dirichlet 核、相位承载、嵌套层级、无画布健壮性 |
| `tests/test_qgfx_witness.cpp`（6 项） | 见证条件的充分非必要性、L1 充要性、二可分 vs 真多体纠缠 |
| `tests/test_physics.cpp` 等（既有） | 回归 |

核心断言覆盖：`separable_across` 的秩判定、失效句柄不崩溃、越界索引安全、
未归一化帧暴露残差、NaN 判为越界、相位在概率分布相同的两个态之间仍可区分。
