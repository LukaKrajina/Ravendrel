# rvdlRSP 义肢/义眼 安全与合规文档

> 本文档面向脑意识控制的义肢（EMG 肌电驱动）与义眼（EEG 脑电驱动）系统，
> 用于医疗级安全认证（FDA 510(k)/De Novo、CE MDR）的前置风险分析。
> 状态：阶段3 初稿，需随临床/工程验证迭代。

---

## 1. 范围与适用对象

| 项 | 说明 |
|---|---|
| 系统 | rvdlRSP 义肢控制子系统（EMG → 关节意图 → 执行器） |
| 系统 | rvdlRSP 义眼控制子系统（EEG → 注视意图 → 注视/相机） |
| 安全等级 | 参考 IEC 60601-1（医用电气设备安全）、ISO 14971（风险管理）、IEC 62304（软件生命周期） |

---

## 2. 风险分析（Hazard Analysis）

按 ISO 14971 对危险源进行识别与分级。严重度（S）、概率（P）、风险（R = S×P）：

| # | 危险源 | 可能后果 | S(1-5) | P(1-5) | R | 缓解措施（代码落点） |
|---|---|---|---|---|---|---|
| H1 | 执行器失控（力矩/速度超限） | 关节超范围运动致伤 | 5 | 3 | 15 | `SafetyController::clamp_*` 限位 |
| H2 | 控制指令丢失（通信中断） | 关节卡死/误动 | 4 | 3 | 12 | `Watchdog` 超时急停 |
| H3 | 编码器反馈错误/漂移 | 位置闭环失效 | 4 | 2 | 8 | `EncoderConsistencyCheck` |
| H4 | 传感器冗余通道分歧 | 单通道故障未被发现 | 4 | 2 | 8 | `DualChannelRedundancy` |
| H5 | EMG 信号噪声/伪迹 | 误触发关节动作 | 3 | 3 | 9 | 死区 + 包络阈值 |
| H6 | 心跳丢失（模块无响应） | 系统僵死 | 3 | 2 | 6 | `HeartbeatMonitor` |
| H7 | 脑电信号漂移（义眼） | 注视方向错误 | 2 | 3 | 6 | 兴奋度归一化 + 基线标定 |

---

## 3. FMEA（失效模式与影响分析）

针对关键组件，识别失效模式、影响与检测手段：

| 组件 | 失效模式 | 影响 | 检测 | 当前状态 |
|---|---|---|---|---|
| 执行器（电机） | 堵转 | 关节无法运动 | 编码器速度=0 而命令≠0 | `EncoderConsistencyCheck` 可检测 |
| 执行器（电机） | 超速 | 关节失控 | 速度超限 | `SafetyController::clamp_velocity` |
| 编码器 | 读数跳变 | 位置反馈错误 | 与命令偏差超限 | `EncoderConsistencyCheck` |
| 冗余传感器 | 通道 A/B 分歧 | 无法判定真实值 | 分歧超阈值 | `DualChannelRedundancy` |
| 主控模块 | 无响应 | 指令停滞 | 心跳超时 | `HeartbeatMonitor` / `Watchdog` |
| EMG 采集 | 电极脱落 | 信号为 0 或噪声 | 信号幅度异常 | `SimEmgSource`/`ExternalBioSignalSource` 需补充检测 |
| EEG 采集 | 电极阻抗高 | 脑电质量差 | 阻抗检测 | 待接入真实设备 |

---

## 4. 安全用例（Safety Use Cases）

### SC1：急停（Emergency Stop）
- **触发**：任意故障检测组件（看门狗/编码器校验/双通道冗余）判定异常。
- **动作**：`SafetyController::emergency_stop()` → 所有 `IActuator::emergency_stop()`，指令归零。
- **验证**：`test_safety.cpp` 中 `watchdog_timeout`、`safety_clamp` 用例。

### SC2：看门狗超时
- **触发**：主循环在 `timeout` 内未调用 `feed()`。
- **动作**：`Watchdog::check()` 触发一次急停回调。
- **验证**：`test_safety.cpp` 中 `watchdog_timeout` 用例。

### SC3：编码器不一致
- **触发**：命令位置与反馈位置偏差 > 阈值。
- **动作**：`EncoderConsistencyCheck::validate()` 返回 false，上层触发急停。
- **验证**：`test_safety.cpp` 中 `encoder_consistency` 用例。

### SC4：冗余通道分歧
- **触发**：双传感器读数分歧 > 阈值。
- **动作**：`DualChannelRedundancy::agree()` 返回 false，标记故障。
- **验证**：`test_safety.cpp` 中 `dual_channel_redundancy` 用例。

### SC5：安全限位
- **触发**：命令超出位置/速度/力矩限值。
- **动作**：`SafetyController::clamp_*` 钳制到安全范围。
- **验证**：`test_hardware.cpp` 中 `safety_clamp` 用例。

---

## 5. 代码 ↔ 文档对应关系

| 文档项 | 代码位置 |
|---|---|
| 心跳监测 | `hardware/fault_detection.hpp` → `HeartbeatMonitor` |
| 看门狗 | `hardware/fault_detection.hpp` → `Watchdog` |
| 编码器一致性 | `hardware/fault_detection.hpp` → `EncoderConsistencyCheck` |
| 双通道冗余 | `hardware/fault_detection.hpp` → `DualChannelRedundancy` |
| 安全限位/急停 | `hardware/safety_controller.hpp` → `SafetyController` |
| 结构化日志/指标/健康 | `hardware/observability.hpp` → `Logger`/`MetricsRegistry`/`HealthReporter` |
| EMG/EEG 信号源 | `hardware/bio_signal.hpp` |
| 执行器/编码器 | `hardware/actuator.hpp` |

---

## 6. 待办（认证前必须补齐）

1. **真实设备闭环验证**：`ExternalBioSignalSource` / `ExternalActuator` 需在真实 EMG/EEG 与电机上做硬件在环（HIL）测试。
2. **电极/阻抗检测**：EMG/EEG 电极脱落检测（当前桩未实现）。
3. **失效安全（Fail-Safe）设计**：急停需硬件级（继电器/制动器），不能仅靠软件。
4. **软件验证**：IEC 62304 等级（B/C 类）对应的单元测试覆盖率、需求追溯矩阵。
5. **可用性工程**：IEC 62366 可用性工程文件。
6. **临床评估**：如按医疗器械申报，需临床评价/试验数据。