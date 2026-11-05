<div align="center">

<img src="icons/readme.png" alt="RVDL" width="128"/>

# Ravendrel — 量子编程语言

[![Stars](https://img.shields.io/github/stars/LukaKrajina/Ravendrel?style=social)](https://github.com/LukaKrajina/Ravendrel/stargazers)
[![Forks](https://img.shields.io/github/forks/LukaKrajina/Ravendrel?style=social)](https://github.com/LukaKrajina/Ravendrel/network/members)

![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue?style=for-the-badge)
![Language](https://img.shields.io/badge/lang-TypeScript%20%7C%20C%2B%2B20-orange?style=for-the-badge)
![Runtime](https://img.shields.io/badge/runtime-LLVM%20ORC%20JIT-informational?style=for-the-badge)
![License](https://img.shields.io/badge/license-MIT-green?style=for-the-badge)

[English](./README.en.md)

</div>

Ravendrel（`.rvdl`）是一门面向「量子计算 + 神经接口 + 量子语言模型 + 量子机器人」的实验性编程语言（直到通用量子设备诞生，才能达到正式）。本仓库提供了完整的 **VSCode 语言支持扩展**、**LLVM ORC JIT 运行时**、**命令行工具链**、**Web 推理界面**、**Go 安装器**、**实时量子可视化器**、**VedaROS 量子机器人操作系统**与 **QbNS 量子脑网络**

*** 

## ✨ 特性

| 特性               | 说明                                                                                                                                                                                                                                                                                                                                        |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 量子编程             | 量子比特分配、量子对象、测量、Bell 态、量子寄存器（`Qubit` / `QObject` / `measure` / `DiracState` / `BellState` / `QuantumRegister`）                                                                                                                                                                                                                             |
| 量子硬件抽象           | 自动探测真实量子机（超导 / 离子阱 / 中性原子 / **光子芯片** 四种物理模态，`RVDL_BACKEND=qm:photon` 选光子后端），否则回退到本地 QVM 模拟器；离线时物理后端以「理想态矢量（`IdealStateCore`）+ 硬件噪声通道（振幅阻尼 / 相位翻转 / 去极化）」桥接，保证 Born 规则坍缩语义                                                                                                                                                                 |
| 量子语言模型 QLM       | 变分量子电路训练、模型导出（`.rvdlm`）、文本 → 量子态编码与解码                                                                                                                                                                                                                                                                                                     |
| QRC 量子储备池        | `qrc_new` / `qrc_train` / `qrc_probe` / `qrc_predict` / `qrc_release` 五个内置函数 + `QReservoir` 类型：合成正弦时序训练线性读出，验证「无贫瘠高原」的量子储备计算                                                                                                                                                                                                              |
| TQNF 拓扑量子神经场     | QObject 层量子门 `qgate_h`/`qgate_x`/`qgate_rz`/`qgate_cnot`（借用不消费）、跨对象受控门 `qgate_cnot_pair`（POVM 弱测量）、非破坏期望 `qexpect_z`、部分坍缩测量 `qmeasure`、qubit 数查询 `qobj_num_qubits`、SWAP test 注意力 `qattention`、态量度 `qstate_entropy`/`qstate_fidelity`、测量熵 `shannon4`/`shannon8`、可训练性诊断 `dla_dim`（动力学李代数维数）                                                 |
| POVM 正定算子值测量     | 跨对象受控门（主 qubit 控制辅助 qubit）实现弱测量，主态不完全坍缩；多 qubit 变分 ansatz 经 `qobj_num_qubits` 态维度对齐                                                                                                                                                                                                                                                       |
| 量子通道逆因果容量        | `retrocausal_imax` / `_idoe` / `_q_capacity` / `_c_capacity` / `_q_one_shot` / `_gain` / `_deformed(kind, p[, x])`：基于 Choi 矩阵的 P-CTC 回程能力（max-information / Doeblin / 渐近与单次量子·经典容量 / q-变形容量 / 回程增益），`kind` 覆盖去极化 / 退相 / 比特翻转 / 振幅阻尼 / 幺正 Hadamard / 比特-相位翻转                                                                               |
| 快子 KK 双空间 CTC    | `retrocausal_ctc_q_capacity` / `_c_capacity` / `_gain(n, theta, mu2, lambda)` / `retrocausal_ctc_dephasing(theta)`：把抽象噪声升级为「快子场在 KK 额外维（DD 双维度）传播」物理产生（Wilson line 退相 / 维度扩散去极化 / 快子真空阻尼），`@layer(coord=...)` 的维度即额外维数 n                                                                                                                  |
| 非欧几里德曲面体几何       | `geodesic_distance(a, b)`（Fubini-Study 测地线距离，SWAP-test 重叠）/ `inversion(R)`（T-对偶反转 `R→1/R`）/ `hyperbolic_metric(x)` / `hyperbolic_distance(x, y)`（Poincaré 球度规与双曲距离）                                                                                                                                                                       |
| 脑机接口 BCI         | `mind_read` / `mind_train` / `mind_feedback`，通过 QbNS Transducer 将神经信号编码为量子态                                                                                                                                                                                                                                                               |
| QbNS 量子脑网络       | `Transducer`（神经→量子编码）、`Rmx`（分布式混合网络）、`qbw`（脑量子波 / 量子流 / 分布链接）                                                                                                                                                                                                                                                                             |
| VedaROS 量子机器人 OS | 量子分布式操作系统：QDDP 去中心化协议、类 rclcpp 客户端库、rvdl 自定义语言、坐标变换树、行为树导航、纠缠意识规划、硬件抽象                                                                                                                                                                                                                                                                    |
| 实时量子可视化          | `qvm_visualizer`：电路网格、态矢量、Bloch 球、量子对象、测量历史、性能指标六窗口实时观察                                                                                                                                                                                                                                                                                   |
| JIT 执行           | 基于 LLVM ORC JIT 的即时编译与执行                                                                                                                                                                                                                                                                                                                  |
| AOT 编译           | `rvdl compile` 将脚本编译为原生二进制（x32 / x64 / arm64 / android）                                                                                                                                                                                                                                                                                   |
| Android APK 构建   | `rvdl build apk <file.rvdl>` 一键把 `.rvdl` 项目交叉编译为 Android APK（AOT 编译 + 轻量运行时 + Gradle 打包），ARM64 设备安装即运行                                                                                                                                                                                                                                    |
| `.rvdli` 模块系统    | `mod` / `use` / `import` / `export` / `requires` 模块声明与导入导出，打包为 `.rvdli` 模块（**QOBF v2 加密二进制**：二进制序列化 + ChaCha20 流加密 + HMAC 防篡改，打开为乱码）动态加载与调用                                                                                                                                                                                               |
| 类型系统             | `form` / `trait` / `impl` / `template` / `rank` 声明式类型、泛型与 trait 实现                                                                                                                                                                                                                                                                        |
| 一行多变量定义          | 单行声明多个同类型变量（`int32 a = 1, b = 2, c = 3;`），后声明的变量可引用先声明的变量                                                                                                                                                                                                                                                                                 |
| 量子门原语            | 内置 `x` `y` `z` `h` `s` `t` `rz` `rx` `ry` `cnot` `toffoli` `swap` `qft` `iqft` `braid` 门、受控门 `cx` `ch` `crz` `cswap` `c_toffoli` `cqft` `cbraid` 与 `measure_x` / `measure_y` 测量                                                                                                                                                           |
| 量子语言迁移           | `rvdl migrate` 把 OpenQASM 2.0/3.0、Q#、Quil、Silq 源码友好地转译为 `.rvdl`（source-to-source，多前端 + 统一 CircuitIR + 单发射器，含 qelib1 门库自动分解）                                                                                                                                                                                                               |
| 真实并发             | `spawn` 块编译为独立线程函数，经 `rvdl_spawn` 内建以 `std::thread` 启动（detach），支持闭包捕获外层变量，配合 Q-Digest 静态竞争检测                                                                                                                                                                                                                                              |
| 多维标签函数           | `@layer(time, thread, coord[, cost, deadline][, dual])` 注解式入口，执行过程映射到「时间 × 线程 × 运行层坐标」多维拓扑，编译期自动推导平行/叠加/投影/反转/交叉五类边 + 调用传播延迟（`E-TOP005`/`E-TOP006` 诊断）+ 运行时 `rvdl_runtime_run_topology` 按 time 分层、按 coord 码距并行调度                                                                                                                          |
| 可逆编织门合成          | `@[gate]`/`@[undo]`/`@[steer]`/`@[unitary]`/`@[measure]` 标签：可逆对偶 U†（门序反转 + 逐门取逆）与相干控制 Λ(U) 的自动合成                                                                                                                                                                                                                                          |
| 量子物理特性           | `@[coherence]`/`@[noise]`/`@[basis]`/`@[decoherence_free]`/`@[error_correction]` 标签：噪声模型与相干时间约束，门后自动注入噪声到 QVM 与多硬件后端                                                                                                                                                                                                                      |
| 经典编译属性           | `@[inline]`/`@[noinline]`/`@[pure]`/`@[cold]`/`@[hot]`/`@[noreturn]`/`@[export]`，映射到 LLVM 函数属性                                                                                                                                                                                                                                            |
| 系统级内核编程          | 读寄存器内建（`read_cr3`/`rdmsr`/`wrmsr`/`cpuid`/`rdtsc`/`xgetbv` 等，把 CR3/MSR/CPUID 叶读回 `uint64` 变量）、裸汇编块 `asm { }` + `@[naked]`（多指令序列：中断存根保存全部寄存器 + iretq、切栈、gdt 远返回）、函数指针 `fn<ret(params)>` 间接调用、`@[packed]` 精确字节布局（GPT/FAT/ATA 磁盘结构）、`volatile_load/store`（MMIO 轮询）、定长数组 `arr<T,N>` 字面量与索引、位域、完整 uint64 无符号运算与复数类型                            |
| VS Code 扩展       | vsx 打包：esbuild bundle + runtime 产物打包进 `bin/`（`bin/win32-x64/`、`bin/linux-x64/`），**一个 vsix 同时携带 Windows 与 Linux 运行时**，按 `process.platform` 自动选择，**安装即运行/编译/构建 rvdl**（无需单独安装 runtime）；语法高亮 + 语义感知补全 + 运行/编译/构建命令 + **中英双语悬停提示**（关键字/类型/内置函数详细文档，随系统语言自动切换）+ 右键菜单迁移（`.qasm`/`.qs`/`.quil`/`.slq` → `.rvdl`）                                  |
| VS Code 侧边栏      | 仿 Flutter DevTools 的 Activity Bar 侧边栏：**编译目标**（x32/x64/arm64/android 一键切换，编译时透传；Android 目标为 `aarch64-linux-android`，需 NDK 交叉编译器）、**操作**（运行 / 编译 / 构建(IR) / **一键打 Android APK**（debug，命令面板可选 release）/ 迁移快捷按钮）、**性能监测**（编译/执行耗时历史曲线图，`globalState` 跨会话持久化 + 清空按钮）、**量子对象与比特**（daemon 实时快照：多 qubit 约化密度矩阵 Bloch 球 3D 可旋转 + 态矢量概率柱状图 + 测量历史） |
| HTTP 推理服务        | `rvdl serve` 提供 OpenAI 兼容的 `chat/completions`、`embeddings`、`models` 接口                                                                                                                                                                                                                                                                    |
| Web 聊天界面         | 基于 Vite + Tailwind + Dexie 的流式推理聊天界面                                                                                                                                                                                                                                                                                                      |
| 工具链管理            | Go 编写的 `ravendrelup` 安装器与版本代理                                                                                                                                                                                                                                                                                                             |
| SE 编辑器           | 轻量 rvdl 桌面编辑器（Electron + CodeMirror），复用语言服务器编译管线，支持文件打开/保存/运行                                                                                                                                                                                                                                                                             |
| rvdlRSP 仿真平台     | 量子机器人仿真平台：物理内核（QM/QVM）、Vulkan PBR 渲染、蓝图编辑、QCDRC 遥操作、量子 RL、脑意识控制                                                                                                                                                                                                                                                                           |
| rvdlRSP Qt GUI   | 基于 Qt 6 + QVulkanWindow 的桌面仿真界面：真 3D 视口、World Outliner / Details 面板、遥操作 / RL / 意识控制                                                                                                                                                                                                                                                       |
| 义肢 / 义眼          | 脑意识控制的义肢（`ProstheticLimb`，EMG 肌电）与义眼（`BionicEye`，EEG 脑电），辅以量子强化学习（复用 `QuantumRLAgent` + `ConsciousnessController`），支持物理义肢驱动与义眼相机                                                                                                                                                                                                          |
| 国产化适配            | 龙芯 LoongArch（LA64）架构检测与 `idle 0`/`dbar 0` 原语适配、LSX/LASX 向量扩展检测；摩尔线程 MUSA GPU 后端检测（类 CUDA，mcc 编译器 + musa 运行时）                                                                                                                                                                                                                              |
| QCOS 可启动内核       | 用 rvdl 编写裸机内核：freestanding 内核核心库 `qcos_core`（无 LLVM/Kokkos/libstdc++）+ 引导源码 `runtime/qcos/`（Multiboot2 汇编 / 链接脚本 / GRUB），经 `build-qcos-*.ps1` 构建可启动 ELF / ISO，QEMU 运行；隶属正式项目 [QuarkOS](https://github.com/LukaKrajina/QuarkOS)                                                                                                            |
| QChain 量子区块链     | 量子加密 + 量子区块链底层（`runtime/include/qchain/`）：后量子密码（格 KEM + 哈希签名）、QKD、QDS、QDBA、量子货币、可编程量子代币；rvdl 内置函数（`qchain_*`）+ daemon C ABI                                                                                                                                                                                                               |
| 光子量子后端           | `qhal/PhotonicBackend.hpp`：连续变量高斯态 + 完整梳态（GKP 编码，高斯包络叠加），GHZ 纠缠保真度 `Pz=Px=1`、`CHSH=2√2`（Bell 违背）；高斯门（displace/squeeze/beam\_splitter）+ 光子噪声 + `bell_verify()` 纠缠验证；`RVDL_BACKEND=qm:photon` 启用                                                                                                                                            |
| 抗超时空与抗破解         | 快子场因果哨兵 `CausalityGuard`（能量守恒 + 额外维 KK 对称，防超时空篡改）、`UnicodeHash256` 语义哈希（汉字/符号 → 拼音/英文 → SHA-256，基于 Unihan 全量拼音表）、`SpacetimeCipher` 时空加密（快子场混沌 keystream + SHA-256 OFB/CTR）                                                                                                                                                                |

***

## 📁 目录结构

```
ravendrel-vscode/
├── client/                    VSCode 扩展客户端（LanguageClient）
├── server/                    语言服务器：lexer / parser / semantic / IR / mmi / CLI / HTTP API 路由
│   └── rvdl.cmd                 `rvdl` 命令行入口（Windows）
├── runtime/                   C++20 运行时：LLVM ORC JIT + 量子硬件抽象层（QHAL）
│   ├── src/                   入口与 C ABI 核心（main.cpp / runtime_api.cpp / QObject.hpp / QDataEncoder.hpp）与内核核心（qcos_core.cpp / qcos_smoke.cpp）
│   ├── qcos/                  可启动 QCOS 内核引导源码（boot_x86_32.S / boot_x86_64.S / kernel_main.cpp / rvdl_shim.cpp / qcos_syscall.hpp / linker_x86_64.ld / grub 配置）
│   ├── android/               Android 轻量运行时（runtime_android.cpp + JNI 桥、CMakeLists.txt 交叉编译、apk/ Gradle 工程）
│   ├── benchmarks/            物理/数学验证基准（-DRVDL_BUILD_BENCHMARKS=ON）
│   ├── examples/              GUI/框架示例（-DRVDL_BUILD_EXAMPLES=ON）
│   │   └── gui/               Fox GUI 框架演示
│   └── include/
│       ├── qcos/              内核核心（freestanding）：驯龙系统 DragonPmm（伙伴系统/空闲链表/TLSF/per-CPU 缓存）+ BitmapPmm（v1 位图）、Vmm 虚拟内存、Spinlock/TicketSpinlock、Sched、Serial、Qms 量子数值
│       ├── qhal/              量子硬件抽象层（QM / QVM / JIT / SandboxJIT / MMI / VisualizationService / IdealStateCore 理想态+噪声桥接 / 多种物理后端：超导 / 离子阱 / 中性原子 / 光子 PhotonicBackend / Karma 量子虚拟化 QVPU / KarmaBus 量子虚拟化 QVPL）
│       ├── qbNs/              量子脑网络 QbNS（Transducer / Rmx / qbw 脑量子波 / qbNSBridge）
│       ├── vedaRos/           量子机器人操作系统 VedaROS（core / bridge / algorithm / hardware / quantum）
│       ├── gui/               实时量子可视化器（protocol / components / windows / src）
│       ├── qlm/               量子语言模型 QLM
│       ├── qml/               量子机器学习原语（Layer / RVDLModelFormat / Inference）
│       ├── numqk/             数值张量库 Numqk（Tensor / Autograd / SoftLogic）
│       ├── spacetime/         时空微分几何求解器（Foliation / Integrators / KaluzaKlein / NeuralField / SpectralEEG / TachyonField 等）
│       ├── verify/            静态验证器（Verifier / SmtLibEmitter / IntervalAbstract）
│       ├── stub/              桩实现（nabStub.hpp）
│       └── utils/             工具（FastPhaseRetrieval / Ga）
├── SE/                   轻量 rvdl 桌面编辑器（Electron + CodeMirror，复用 server 编译管线）
│   ├── src/                   Electron 主进程 + 编译管线复用 + daemon 连接
│   └── renderer/              CodeMirror 编辑器 + 输出 Shell + 状态栏
├── rvdlRSP/                  量子机器人仿真平台（C++20，作为 runtime 的新 target）
│   ├── include/               core / qpc / qpu / circuit / blueprint / pcg / render / editor / control / bridge / qcdrc
│   ├── src/                   入口、Vulkan 上下文、JIT host、stb_image 实现
│   ├── examples/              RSP 示例程序（-DRVDLRSP_BUILD_EXAMPLES=ON）
│   ├── gui/                   Qt 6 仿真平台 UI（qt 窗口 + simulation_host 仿真内核）
│   ├── shaders/               PBR 着色器（GLSL）
│   └── tests/                 单元测试（87 个用例 / 24 个套件）
├── chatWebUI/              Web 推理聊天界面（Vite + Tailwind + Dexie）
├── installer/                 Go 工具链安装器 / 版本代理（ravendrelup）
├── scripts/                   构建 / 签名脚本（Windows 构建 rvdl_rt、Kokkos 安装、QCOS 内核/ISO/GRUB 镜像、sign.ps1、deploy-wdac、Linux 构建安装）
├── examples/                  示例 `.rvdl` 程序
├── syntaxes/                  TextMate 语法高亮
├── vendor/                    第三方库（GLFW、Nuklear 等）
└── vcpkg/                     vcpkg 依赖管理
```

***

## ⚠️ 构建前必读

> **重要**：本仓库中的 `.vscode/*.json`、`runtime/.clangd` 与 `runtime/CMakeLists.txt` 包含作者本机的**绝对路径**（Kokkos、CUDA、LLVM、GLFW、编译器位置等）。**开发者必须根据自己的实际安装路径修改这些文件后，才能正确构建。** 请勿直接使用仓库中的硬编码路径。

需要修改的位置：

| 文件                              | 需修改项                                                                                      |
| ------------------------------- | ----------------------------------------------------------------------------------------- |
| `runtime/CMakeLists.txt`        | `Kokkos_DIR`（第 58 行，`C:/Libraries/kokkos/lib/cmake/Kokkos`）、GLFW 库目录 `lib-vc2026`（第 48 行） |
| `rvdlRSP/CMakeLists.txt`        | `Qt6_DIR`（第 127 行，`C:/Qt/6.11.2/msvc2022_64/lib/cmake/Qt6`，仅构建 `rvdlRSP_gui` 时需要）         |
| `runtime/.clangd`               | Kokkos / CUDA 的 `-I` include 路径（第 7-8 行）                                                  |
| `.vscode/c_cpp_properties.json` | `compilerPath`、Kokkos/CUDA/LLVM 的 `includePath`                                           |
| `.vscode/settings.json`         | `cmake.sourceDirectory`（工作区绝对路径）                                                          |

依赖的下载与编译方法详见 [🛠️ 依赖与构建](#-依赖与构建)。

***

## 🚀 快速开始

### 1. VSCode 扩展

```bash
npm install
npm run compile        # 编译 client 与 server（tsc -b）
```

在 VSCode 中按 `F5` 启动扩展调试，或通过 `vsce package` 打包安装。打开任意 `.rvdl` 文件即可获得语法高亮、自动补全与语义诊断。

- **运行脚本**：`Ctrl+Alt+N`（macOS：`Cmd+Alt+N`）或点击编辑器右上角 ▶ 按钮。

#### 打包发布（vsix，runtime 内置于 `bin/`）

```bash
npm run package:linux   # Linux vsix（原生 runtime + *.so 进 bin/）
npm run package:win     # Windows vsix（交叉编译 runtime.exe + *.dll 进 bin/）
npm run package:all     # 两个平台
```

产物输出到 `dist/ravendrel-lang-<版本>-linux-x64.vsix` 与 `dist/ravendrel-lang-<版本>-win32-x64.vsix`。

- **Linux**：`scripts/build-linux.sh` 构建 runtime（依赖 LLVM / Kokkos / Vulkan / GLFW），`scripts/prepare-vsix-runtime.sh` 复制进 `bin/`。
- **Windows（交叉编译）**：`scripts/build-windows-cross.sh` 在 Linux 上用 **MinGW-w64**（`x86_64-w64-mingw32-g++`）交叉编译 runtime，需设置 `MINGW_DEPS_ROOT` 指向 MinGW 版 LLVM / Kokkos / Vulkan / zlib / zstd（约定见 `scripts/mingw-w64-toolchain.cmake` 头部）。
- 打包流程统一由 `scripts/package-vsix.sh` 编排（编译 client/server → 构建 runtime → 复制 `bin/` → `vsce package`）。

### 2. C++ 运行时

运行时依赖 **LLVM**、**Kokkos**、**Vulkan + GLFW + Nuklear**。完整依赖清单与编译方法见 [🛠️ 依赖与构建](#-依赖与构建)。

> ⚠️ 构建前请先按 [构建前必读](#-构建前必读) 修改硬编码路径，或通过命令行 `-D` 参数覆盖。

```bash
cd runtime
cmake -B build -S . -G "Ninja" \
  -DCMAKE_CXX_COMPILER="<你的 clang-cl / g++ / clang++ 路径>" \
  -DCMAKE_TOOLCHAIN_FILE="<你的 vcpkg 路径>/scripts/buildsystems/vcpkg.cmake" \
  -DLLVM_DIR="<你的 LLVM cmake 目录>" \
  -DKokkos_DIR="<你的 Kokkos 安装>/lib/cmake/Kokkos" \
  -DCUDAToolkit_ROOT="<你的 CUDA 安装目录>"
cmake --build build --config Release --parallel
```

构建产生以下目标：

| 目标                | 类型  | 说明                                                                             |
| ----------------- | --- | ------------------------------------------------------------------------------ |
| `rvdl_rt`         | 共享库 | 量子核心（LLVM ORC JIT + QHAL + Kokkos），导出 C ABI（`RuntimeApi.h`）                    |
| `qcos_core`       | 静态库 | freestanding 内核核心（无 LLVM / Kokkos / libstdc++，仅编译器内置）                          |
| `qcos_smoke`      | 可执行 | 宿主机冒烟测试（校验 Pmm / Sched / Spinlock / LibcSubset 逻辑）                             |
| `qchain_selftest` | 可执行 | 量子区块链核心自测（SHA3/ML-KEM/因果哨兵/UTXO 等）                                             |
| `mir_module_test` | 可执行 | MirModuleBuilder 下沉测试（MIR JSON → LLVM Module）                                  |
| `runtime`         | 可执行 | 壳子：socket 守护进程（`--daemon` 监听 `localhost:50052`）+ stdin 交互，经 C ABI 调用 `rvdl_rt` |
| `qvm_visualizer`  | 可执行 | 实时量子可视化器（Vulkan + GLFW + Nuklear）                                              |
| `transmitter`     | 可执行 | QLM 发射器                                                                        |

**物理/数学验证基准** （`-DRVDL_BUILD_BENCHMARKS=ON`，见 [runtime/BENCHMARKS.md](runtime/BENCHMARKS.md)）：

| 目标                             | 类型  | 说明                                  |
| ------------------------------ | --- | ----------------------------------- |
| `spacetime_benchmark`          | 可执行 | 核心快子场验证：线性增长率/静态 kink/能量守恒/4阶收敛     |
| `spacetime_extended_benchmark` | 可执行 | 扩展模块：谱 FFT/S² 谐波/高亏格/KK/O(D,D) 广义度规 |
| `neural_eeg_benchmark`         | 可执行 | 皮层神经场对称性破缺 + EEG alpha 频段谱分析        |
| `nucleation_visualization`     | 可执行 | 畴壁成核可视化（输出 PGM 序列）                  |
| `run_benchmarks`               | 自定义 | 一键运行全部基准（退出码 0=全通过）                 |

**GUI/框架示例** （`-DRVDL_BUILD_EXAMPLES=ON`，见 [runtime/EXAMPLES.md](runtime/EXAMPLES.md)）：

| 目标             | 类型  | 说明                                    |
| -------------- | --- | ------------------------------------- |
| `fox_showcase` | 可执行 | Fox GUI 框架演示（macOS 27 Golden Gate 风格） |

**rvdlRSP 仿真平台** （见 [RSP/EXAMPLES.md](RSP/EXAMPLES.md)）：

| 目标                        | 类型  | 说明                                           |
| ------------------------- | --- | -------------------------------------------- |
| `rvdlRSP`                 | 可执行 | 量子机器人仿真平台主程序（物理内核 + Vulkan PBR + 蓝图 + 遥操作）   |
| `rvdlRSP_rl_physics_demo` | 可执行 | 量子 RL 端到端训练示例（`-DRVDLRSP_BUILD_EXAMPLES=ON`） |
| `rvdlRSP_benchmark`       | 可执行 | RSP 性能基准                                     |
| `rvdlRSP_tests`           | 可执行 | rvdlRSP 单元测试（87 个用例 / 24 个套件）                |
| `rvdlRSP_sim`             | 静态库 | 仿真内核（物理 / 遥操作 / RL / QVM / 意识，需 Qt 6）        |
| `rvdlRSP_gui`             | 可执行 | Qt 6 桌面仿真界面（需 Qt 6，可选）                       |

- **前台模式（stdin/stdout）**：`./runtime`
- **守护进程模式**：`./runtime --daemon`（供 CLI、API 服务与可视化器使用）

### 3. 命令行工具

`server/rvdl.cmd` 将 `rvdl` 映射到编译后的 `server/out/cli.js`：

```bash
rvdl run <script.rvdl>                                  # 运行脚本
rvdl ir <script.rvdl>                                   # 仅输出 LLVM IR 到 stdout（不连接 daemon）
rvdl compile <x32|x64|arm64|android> <-e|-m> <script.rvdl> [-o <out>]  # 编译为原生二进制（-e 可执行 / -m 混合库；默认输出到源文件同目录）
rvdl verify <script.rvdl>                               # 静态验证契约（requires/ensures/invariant）
rvdl verify <script.rvdl> --smt [out.smt2]              # 导出 SMT-LIB 供外部求解器（Z3/cvc5）判定
rvdl serve <model.rvdlm> [--port <port>]                # 启动 HTTP 推理服务（默认 9080）
rvdl migrate <file> --from <lang> [-o out.rvdl]         # 现有 QPL 源码迁移为 .rvdl（openqasm2/3、qsharp、quil、silq）
rvdl build apk <script.rvdl> [--release]                # 构建 Android APK（详见「Android APK 构建」）
```

### 4. 实时量子可视化器

可视化器通过 socket 连接守护进程（`localhost:50052`），实时读取真实 QVM 状态：

```bash
# 终端 1：启动守护进程（内部运行 5-qubit 演示线路）
./runtime --daemon

# 终端 2：启动可视化器
./qvm_visualizer
```

六窗口界面详见 [🖥️ QVM 可视化器](#-qvm-可视化器)。

### 5. Web 推理界面

```bash
cd chatWebUI
npm install
npm run dev          # 开发模式
npm run build        # 生产构建
```

界面默认连接 `http://localhost:9080/v1/chat/completions`，需先通过 `rvdl serve` 启动推理服务。

### 6. SE 编辑器

类似 Python IDLE 的轻量 rvdl 桌面编辑器，复用语言服务器的编译管线：

```bash
cd SE
npm install
npm start          # 编译并启动 Electron 编辑器
```

运行脚本前请先启动守护进程 `./runtime --daemon`。编辑器支持打开/保存 `.rvdl` 文件、语法高亮（CodeMirror）、实时诊断与一键运行。

### 7. Android APK 构建

Ravendrel 支持把 `.rvdl` 项目交叉编译为 **Android APK**（ARM64），安装到 Android 设备即可运行：

```bash
rvdl build apk examples/FivemBell.rvdl           # debug 包
rvdl build apk examples/FivemBell.rvdl --release # release 包（已配置签名）
```

产物输出到 `runtime/android/apk/app/build/outputs/apk/{debug,release}/app-{debug,release}.apk`。

**架构**：`.rvdl` 源码在桌面端 `rvdl compile android` AOT 编译为 `librvdl_main.so`（aarch64-linux-android），与轻量运行时 `librvdl_rt.so`（QVM 核心，不含 LLVM JIT）一并打入 APK，由 Java JNI 壳（`com.ravendrel.MainActivity`）加载并调用 `rvdl_main`。

**前置**：

| 依赖                     | 说明                                                                              |
| ---------------------- | ------------------------------------------------------------------------------- |
| Android SDK / NDK      | `sdkmanager` / `$ANDROID_HOME` 可用，NDK 26.x（`runtime/android/CMakeLists.txt` 约定） |
| Gradle 8.5 + JDK 17/21 | Gradle 构建 APK 用（`scripts/build-apk.sh` 内自动设置 `JAVA_HOME`）                       |
| Kokkos（Android 版）      | 交叉编译到 aarch64（`runtime/android/CMakeLists.txt` 的 `find_package(Kokkos)`）        |

**release 签名**：首次需生成 keystore（`keytool -genkeypair ... -keystore release.keystore -alias ravendrel`），并在 `runtime/android/apk/keystore.properties` 填写 `storeFile/storePassword/keyAlias/keyPassword`。`release.keystore` 与 `keystore.properties` 含私钥，已被 `.gitignore` 排除。

相关源码：`runtime/android/runtime_android.cpp`（轻量运行时 + JNI）、`runtime/android/CMakeLists.txt`（交叉编译）、`runtime/android/apk/`（Gradle 工程）、`scripts/build-apk.sh`（一键构建脚本）。

### 8. rvdlRSP 仿真平台

量子机器人仿真平台作为 runtime 的新 target 构建（需先完成 [构建前必读](#-构建前必读) 的路径配置）：

```bash
cd runtime
cmake --build build --target rvdlRSP                    # 仿真平台主程序
cmake --build build --target rvdlRSP_rl_physics_demo    # 量子 RL 端到端示例（需 -DRVDLRSP_BUILD_EXAMPLES=ON）
cmake --build build --target rvdlRSP_tests              # 单元测试
ctest -R rvdlRSP_tests                                  # 运行测试（87 个用例 / 24 个套件）
```

详细架构见 [🤖 rvdlRSP 仿真平台](#-rvdlrsp-仿真平台)。

***

## 🧩 语言参考

### 类型系统

`int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64` `float` `double` `complex64` `complex128` `string` `char` `Qubit` `QObject` `QModel` `QReservoir`（另有 `auto` / `let` 类型推导、`cap<T>` 能力指针、`arr<T, N>` 定长数组、`fn<ret(params)>` 函数指针、`fixed` 编译期常量、`null` 空值）

### 变量声明（一行多定义）

支持单行声明多个同类型变量，后声明的变量可引用先声明的变量：

```rvdl
int32 a = 1, b = 2, c = a + b;   // a=1, b=2, c=3
let x = "hi", y = "yo";          // let/auto 类型推导同样支持
```

### 控制流

`let` `auto` `int` `new` `return` `if` `else` `while` `for` `break` `continue` `route`（`path` / `fallback`）`spin` `spawn` `entangle`

### 内置函数

| 函数                                                                     | 签名                                                   | 返回           | 说明                                                 |
| ---------------------------------------------------------------------- | ---------------------------------------------------- | ------------ | -------------------------------------------------- |
| `alloc`                                                                | `alloc()`                                            | `Qubit`      | 分配一个量子比特                                           |
| `measure`                                                              | `measure(<Qubit>)`                                   | `int`        | 测量量子比特并坍缩                                          |
| `basis_state`                                                          | `basis_state(double, double, int)`                   | `QObject`    | 在任意基（布洛赫方向 θ,φ）下构建量子态                              |
| 量子门                                                                    | `x` `h` `rz` `cnot` `toffoli` `swap` `qft` `braid`   | —            | 内置量子门操作                                            |
| 受控门                                                                    | `cx` `ch` `crz` `cswap` `c_toffoli`                  | —            | 受控门（可逆编织 `@[steer]` 自动合成）                          |
| 量子测量                                                                   | `measure_x` / `measure_y`                            | `int`        | X / Y 基测量                                          |
| `encode_text`                                                          | `encode_text(string)`                                | `QObject`    | 文本编码为量子态                                           |
| `encode_image`                                                         | `encode_image(string)`                               | `QObject`    | 图像编码为量子态                                           |
| `rvdl_encode_string`                                                   | `rvdl_encode_string(string)`                         | `QObject`    | 字符串 → 量子对象                                         |
| `rvdl_decode_string`                                                   | `rvdl_decode_string(QObject)`                        | `string`     | 量子对象 → 字符串                                         |
| `qlm_load`                                                             | `qlm_load(string)`                                   | `QModel`     | 加载 QLM 模型（`.rvdlm`）                                |
| `qlm_forward`                                                          | `qlm_forward(QModel, QObject)`                       | `void`       | QLM 前向推理                                           |
| `qlm_invoke`                                                           | `qlm_invoke(QObject, int, double)`                   | `QModel`     | 训练变分量子电路                                           |
| `mind_read`                                                            | `mind_read(string)`                                  | `QObject`    | 读取脑信号并编码为量子态（`stream/spike/lfp/eeg/sensor`）        |
| `mind_train`                                                           | `mind_train(QObject, int, double)`                   | `void`       | 用脑信号训练 QLM 并导出                                     |
| `mind_feedback`                                                        | `mind_feedback(QObject)`                             | `void`       | 测量脑状态，闭合神经反馈闭环                                     |
| `veda_qlm_train`                                                       | `veda_qlm_train(QObject, int, double)`               | `void`       | 调用 VedaROS QLM 训练                                  |
| `qchain_wallet`                                                        | `qchain_wallet()`                                    | `string`     | 创建后量子钱包，返回量子安全地址                                   |
| `qchain_mint`                                                          | `qchain_mint(string, uint64)`                        | `void`       | 铸币到指定地址                                            |
| `qchain_transfer`                                                      | `qchain_transfer(string, string, uint64)`            | `int32`      | 地址间转账                                              |
| `qchain_balance`                                                       | `qchain_balance(string)`                             | `uint64`     | 查询余额                                               |
| `qchain_mine`                                                          | `qchain_mine()`                                      | `int32`      | 量子 PoW 出块，返回链高度                                    |
| `qchain_height`                                                        | `qchain_height()`                                    | `int32`      | 链高度                                                |
| `qchain_verify`                                                        | `qchain_verify()`                                    | `int32`      | 验链（哈希链 + 默克尔根自洽）                                   |
| `qchain_qkd`                                                           | `qchain_qkd(int32)`                                  | `string`     | BB84 量子密钥分发，返回共享密钥                                 |
| `qchain_qdba`                                                          | `qchain_qdba(int32)`                                 | `int32`      | 量子拜占庭共识（GHZ 态 QDBA）                                |
| `qchain_coin_mint`                                                     | `qchain_coin_mint(int32)`                            | `QObject`    | 铸不可克隆量子钞票（Wiesner 量子货币）                            |
| `qchain_coin_verify`                                                   | `qchain_coin_verify(QObject)`                        | `int32`      | 验证量子钞票                                             |
| `qchain_sha3`                                                          | `qchain_sha3(string)`                                | `string`     | SHA3-256 哈希（返回 hex）                                |
| `qchain_hmac`                                                          | `qchain_hmac(string, string)`                        | `string`     | HMAC-SHA256（返回 hex）                                |
| `qchain_hash_unicode`                                                  | `qchain_hash_unicode(string)`                        | `string`     | 语义哈希：汉字→拼音→SHA-256                                 |
| `qchain_sign`                                                          | `qchain_sign(string)`                                | `string`     | 后量子签名（ML-DSA-65，全局密钥，返回 hex）                       |
| `qchain_sign_verify`                                                   | `qchain_sign_verify(string, string)`                 | `int32`      | 验证后量子签名                                            |
| `qchain_sign_pubkey`                                                   | `qchain_sign_pubkey()`                               | `string`     | 全局签名公钥（hex）                                        |
| `qchain_mlkem_encaps`                                                  | `qchain_mlkem_encaps(string)`                        | `string`     | ML-KEM-768 封装（返回 `ct:ss` hex）                      |
| `qchain_mlkem_decaps`                                                  | `qchain_mlkem_decaps(string, string)`                | `string`     | ML-KEM-768 解封（返回共享密钥 hex）                          |
| `qchain_causal_verify`                                                 | `qchain_causal_verify()`                             | `int32`      | 因果哨兵校验（防超时空）                                       |
| `qchain_cipher_encrypt`                                                | `qchain_cipher_encrypt(uint64, string)`              | `string`     | 时空加密（OFB，返回 hex）                                   |
| `qchain_cipher_decrypt`                                                | `qchain_cipher_decrypt(uint64, string)`              | `string`     | 时空解密（返回文本）                                         |
| `qrc_new`                                                              | `qrc_new(int32, int32)`                              | `QReservoir` | 创建量子储备池（qubits, layers）                            |
| `qrc_train`                                                            | `qrc_train(QReservoir, int32, double)`               | `void`       | 训练线性读出（无贫瘠高原）                                      |
| `qrc_release`                                                          | `qrc_release(QReservoir)`                            | `void`       | 释放储备池                                              |
| `qrc_probe`                                                            | `qrc_probe(QReservoir, QObject)`                     | `QObject`    | 测量态编码输入，返回 ⟨Z⟩ 特征                                  |
| `qrc_predict`                                                          | `qrc_predict(QReservoir, QObject)`                   | `QObject`    | 返回读出预测（qubit 编码）                                   |
| `qgate_h` / `qgate_x` / `qgate_rz` / `qgate_cnot`                      | `(QObject, int[, int/double])`                       | `void`       | QObject 层量子门（作用于第 i 个 qubit，借用不消费）                 |
| `qgate_cnot_pair`                                                      | `qgate_cnot_pair(QObject, int, QObject, int)`        | `void`       | 跨对象受控非门（POVM 弱测量）                                  |
| `qexpect_z`                                                            | `qexpect_z(QObject, int)`                            | `double`     | 非破坏期望读取 ⟨Z⟩（软测量，不坍缩）                               |
| `qmeasure`                                                             | `qmeasure(QObject, int)`                             | `int32`      | 部分坍缩测量（测第 i 个 qubit，其余存活）                          |
| `qobj_num_qubits`                                                      | `qobj_num_qubits(QObject)`                           | `int32`      | 查询 QObject 的 qubit 数                               |
| `qattention`                                                           | `qattention(QObject, QObject)`                       | `double`     | SWAP test 态重叠度 \|⟨ψ\_q\|ψ\_k⟩\|²                   |
| `qstate_entropy`                                                       | `qstate_entropy(QObject)`                            | `double`     | 约化密度矩阵 von Neumann 熵                               |
| `qstate_fidelity`                                                      | `qstate_fidelity(QObject, QObject)`                  | `double`     | 两量子态保真度 F(ρ,σ)                                     |
| `dla_dim`                                                              | `dla_dim(string, int32)`                             | `int32`      | 动力学李代数维数（可训练性诊断）                                   |
| `shannon4` / `shannon8`                                                | `(int32, ...)`                                       | `double`     | 测量熵（4/8 态香农熵）                                      |
| `retrocausal_imax` / `_idoe` / `_q_capacity` / `_c_capacity` / `_gain` | `(int32 kind, double p)`                             | `double`     | 逆因果容量：max-information / Doeblin / 渐近量子·经典容量 / 回程增益 |
| `retrocausal_q_one_shot` / `retrocausal_deformed`                      | `(int32 kind, double p, double x)`                   | `double`     | 单次容量（含 ε）/ q-变形容量                                  |
| `retrocausal_ctc_q_capacity` / `_c_capacity` / `_gain`                 | `(int32 n, double theta, double mu2, double lambda)` | `double`     | 快子 KK 双空间嘈杂 CTC 通信容量（n 个 DD 额外维）                   |
| `retrocausal_ctc_dephasing`                                            | `(double theta)`                                     | `double`     | Wilson line 退相噪声映射（额外维破坏 ±n 模式对称）                  |
| `geodesic_distance`                                                    | `(QObject, QObject)`                                 | `double`     | Fubini-Study 测地线距离 `arccos\|⟨a\|b⟩\|`（曲面体投影）       |
| `inversion` / `hyperbolic_metric`                                      | `(double)`                                           | `double`     | T-对偶反转 `R→1/R` / Poincaré 球度规 `4/(1-\|x\|²)²`      |
| `hyperbolic_distance`                                                  | `(double, double)`                                   | `double`     | Poincaré 双曲距离                                      |

### 内置类

`DiracState` `BellState` `QuantumRegister`（通过 `new` 实例化，支持 `.measure()` 等方法成员访问）；`QReservoir` 量子储备池通过 `qrc_new` 创建

### 模块系统（`.rvdli`）

支持模块的声明与导入导出，可打包为 `.rvdli` 模块（RVDLI 格式）供运行时动态加载与调用：

```rvdl
// 定义并导出模块
mod math {
    pub int32 add(int32 a, int32 b) {
        return a + b;
    }
    export int32 square(int32 x) {
        return x * x;
    }
}
```

```rvdl
// 导入 .rvdli 模块并调用
import math from "./math.rvdli";
requires io.network;      // 声明所需权限
let result = math.add(1, 2);
```

- 关键字：`mod`（模块）、`use`（路径导入）、`pub`（公开）、`import` + `from`（导入 `.rvdli`）、`export`（导出）、`requires`（权限声明）
- `.rvdli` 采用 **QOBF v2 加密二进制**：由语言服务器（`server/src/mmi.ts`）做「二进制序列化 + ChaCha20 流加密 + HMAC 防篡改」后打包，运行时（`qhal/Qcrypt.hpp` + `qhal/MMI.hpp`）验 HMAC → 解密 → 二进制解析 → JIT，通过 C ABI（`RuntimeApi.h` 的 `rvdl_runtime_*_mmi`）加载与调用（密钥 = HKDF(内嵌盐 ‖ 模块名)，与加密 DLL 同定位）

### 类型定义（form / trait / impl）

声明式类型系统：

| 关键字        | 说明                                     |
| ---------- | -------------------------------------- |
| `form`     | 定义数据结构，支持继承（`inherits`）与 `rank` 秩      |
| `trait`    | 定义可共享的行为接口                             |
| `impl`     | 为类型实现 trait（`impl <trait> for <type>`） |
| `template` | 泛型声明                                   |
| `flavor`   | 枚举（`enum` 的量子化命名）                      |
| `fuse`     | 模式匹配（`match` 的量子化命名，`_` 通配）            |
| `self`     | 方法接收者（`self` / `&self`）                |

### 系统级编程与安全性

rvdl 支持裸机 / 内核编程：`cap<T>` 能力指针、`unsafe { ... }` 危险操作块、`native("hlt")` 内联汇编、`asm { }` 裸汇编块（多指令序列，配合 `@[naked]` 裸函数）、`read_cr3`/`rdmsr`/`wrmsr`/`cpuid` 读寄存器内建、`volatile_load`/`volatile_store` volatile 内存访问、`outb`/`inb` 端口 I/O、`sync_*` 原子操作、`rvdl_gc_alloc`/`rvdl_gc_free` 内核堆、`addr` 取函数地址、`rvdl_sys_call`/`rvdl_sys_log` 系统调用 ABI 与 `rvdl_qms_gap`/`rvdl_mix_bound`/`rvdl_qms_conc` QMS 数值内核；`@[packed]` 精确字节布局结构体、函数指针 `fn<...>` 间接调用、定长数组 `arr<T,N>`、位域与复数类型。

编译管线内置 **MIR 中间表示**、**Polonius 风格借用检查**（量子线性类型 QLT：no-cloning + 恰好消费一次）、**Q-Digest 静态竞争检测**（`spawn`/`entangle` 构造）与 **VCGen 最弱前置条件演算**（`requires`/`ensures`/`invariant` 契约验证）。

### 入口函数（多维标签函数）

程序入口不再单一，而是用 `@layer` 标签把执行过程映射到多维拓扑空间：

```rvdl
@layer(time=0, thread=0, coord=(0,0))
int32 producer() { return 42; }

@layer(time=0, thread=1, coord=(0,1))   // 与 producer 平行（异 coord / 异 thread）
int32 observer() { return 7; }

@layer(time=1, thread=0, coord=(0,0))   // 与 producer 叠加（同 coord、time+1）
int32 consumer() { return 0; }
```

- `time`：锚点时间（叠加链时序；子函数可省略，运行时继承调用者时钟 + Δt）
- `thread`：逻辑线程（同线程串行、异线程并行）
- `coord`：N 维运行层坐标（平行/叠加推导依据）
- `cost` / `deadline`：块自身执行成本（影响父块逻辑时钟推进）/ 时间束缚上限
- `dual`：`dual=1` 标记该块位于 T-对偶反转空间——同 coord 且标记 dual 的块之间推导 `inversion` 边

两个可选机制挂在 `@layer(...)` 上（**不写则完全惰性**，既有语义与诊断逐字节不变）：

```rvdl
@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), track=(expect_z, entropy))
int32 producer() { return 42; }

@layer(time=1, thread=0, coord=(1,0), track=(expect_z))
int32 consumer() { return 0; }
```

- **穿梭** `shuttle=(c,…)`：按 L1 码距定权 `w = exp(−d_L1/λ)`，在共享点前后双向渗透进关联层。
  该核**严格为正、不设截断阈值**（与注意力 softmax 根本不同）——哪怕关联度极小，对端仍留在调度计划内。
  线格式同时下发 `weight` 与 `logWeight`：`d/λ > 745` 时线性权重双精度**整零**，路由一律落到 `logWeight`，
  **数值零不等于语义截断**。五种可切换策略：蝶式（默认，深度 `⌈log₂span⌉`）/ 环式（深度 `n`，大消息下门数更省而可能反超）/
  树式（`⌈log₂(n+1)⌉`）/ Grover 扩散（`1` 步，需 2ⁿ 工作寄存器）/ **c001 蝶点**（`1` 步，零附加资源：全体关联层的主要特征
  压缩成一个**蝶点**再辐射，多个蝶点经拓扑边互联成「DNA 链」多层架构）；默认 `shuttle_strategy=-1`（`auto`）由运行期启发式选型，
  **决策写入运行诊断，不静默**。`shuttle_rounds=0`（auto）时轮次**按所选策略**解析 —— 蝶式跟格点跨度走、环式跟对端数走，
  两者无固定大小关系，故不能统一套一个式子（否则 `E-SHU007` 的 deadline 检查会系统性低估环式代价）。
- **追踪** `track=(obs,…)`：在块的功能周期 `[time, time+cost)` 内把可观测量转成经典影子槽、复制、再恢复原态。
  **单次软克隆在物理上不可能**，故采用三级保真阶梯（L0 `qnd` 精确 / L1 `shadow` 统计 / L2 `relabel` 自由），
  **不做统一精确承诺**；非零 `track_mode` **覆盖** auto 选级。影子槽只承载经典标量（`static_assert` 禁止 `QObject`），
  结构上不可能是量子态副本——这正是保 QLT 线性不变量 `E-Q001`（不可克隆）/ `E-Q002`（线性泄漏）的方式。

完整规格见 [穿梭与追踪](docs/rvdl-shuttle-tracking.md)。

块间关系自动推导为五类拓扑边：同 coord 相邻 time → `stack`（叠加）、异 coord → `parallel`（平行）、coord 维度不同 → `projection`（曲面体投影）、同 coord + `dual` → `inversion`（反转）、同 coord 异 thread → `crossing`（交叉）；子函数还带调用传播延迟（锚点 = 父锚点 + 累计 Δt）。编译期用 `E-TOP005`（叠加链坐标空槽）/ `E-TOP006`（传播延迟超 deadline）/ `E-TOP008`（`@layer` 未知键，原为静默丢弃）/ `E-TOP009`（`time`·`thread`·`cost`·`deadline` 取负；`coord` 是格点地址，允许为负）报错；通过校验后，含 `@layer` 的模块会生成调度表 `@rvdl_topology_json` 与入口 `@rvdl_topology_entry`，运行时 `rvdl_runtime_run_topology` 按 `time` 分层、同层按 coord 的 L1 码距并行调度（阈值 `RVDL_TOPOLOGY_MIN_DISTANCE`）。

无标签的顶层脚本仍回退到脚本模式（隐式包裹为 `rvdl_main`）。详细语法与标签体系见 [rvdl 语言手册](docs/rvdl-language-manual.md)。

### 示例

```rvdl
// 示例 1：Bell 态统计（examples/FivemBell.rvdl）
int32 rvdl_main() {
    int32 iterations = 50000;
    int32 total_ones = 0;
    auto massive_reg = new QuantumRegister(15);

    int32 count = 0;
    while (count < iterations) {
        auto bell_pair = new BellState();
        total_ones = total_ones + bell_pair.measure();
        count = count + 1;
    }
    return total_ones;
}
```

```rvdl
// 示例 2：脑控编程（examples/mind_controlled.rvdl）
let brain_state = mind_read("eeg");     // 脑电 → 量子态
let bits = brain_state.measure;         // 意念驱动的测量
mind_train(brain_state, 200, 0.01);     // 大脑训练 QLM
mind_feedback(brain_state);             // 神经反馈
```

```rvdl
// 示例 3：VedaROS QLM 训练（examples/veda_qlm.rvdl）
let brain = mind_read("eeg");           // 脑电 → 量子态
let model = qlm_invoke(brain, 10, 0.01); // QLM 调用
model.export("brain_model.rvdlm");        // 导出模型
veda_qlm_train(brain, 200, 0.01);       // VedaROS QLM 训练
```

***

## 🛰️ QCOS — 可启动内核（用 RVDL 编写裸机内核）

> QCOS 内核隶属正式项目 **[QuarkOS](https://github.com/LukaKrajina/QuarkOS)**，本文所述 `runtime/qcos/` 与 `qcos_core` 是其内核源码在本仓库的落地。完整系统请见 QuarkOS 仓库。

Ravendrel 支持用 rvdl 语言编写可启动的裸机内核：`runtime/qcos/` 是一套 freestanding 内核引导源码树，`qcos_core` 提供无 LLVM / Kokkos / libstdc++ 依赖的内核核心库。

| 组件      | 位置                                               | 说明                                                                                             |
| ------- | ------------------------------------------------ | ---------------------------------------------------------------------------------------------- |
| 引导汇编    | `runtime/qcos/boot_x86_32.S` / `boot_x86_64.S`   | Multiboot2 引导 + 长模式切换                                                                          |
| 内核入口    | `runtime/qcos/kernel_main.cpp`                   | 串口输出 + 停机                                                                                      |
| 桥接      | `runtime/qcos/rvdl_shim.cpp` / `rvdl_shim32.cpp` | `kernel_main` 桥接 + QCOS syscall ABI（`rvdl_sys_call` / `rvdl_sys_log` / `rvdl_gc_alloc` 等，串口输出） |
| 系统调用    | `runtime/qcos/qcos_syscall.hpp`                  | QCOS syscall ABI 辅助（syscall 号 / 串口 / TSC 时钟 / itoa）                                            |
| 链接脚本    | `runtime/qcos/linker_x86_64.ld`                  | 内核 ELF 链接                                                                                      |
| GRUB 配置 | `runtime/qcos/grub.cfg` / `grub_standalone.cfg`  | Multiboot 引导配置                                                                                 |
| 内核核心库   | `runtime/src/qcos_core.cpp`（`qcos_core` 静态库）     | DragonPmm 物理内存 / Vmm / Spinlock / Sched / Serial / Qms                                         |
| 冒烟测试    | `runtime/src/qcos_smoke.cpp`（`qcos_smoke`）       | 宿主机校验内核头文件逻辑                                                                                   |

### 构建可启动内核

```bash
# 纯 RVDL 内核 → 可启动 ELF（RVDL → LLVM IR → freestanding object → 链接 boot stub）
.\scripts\build-qcos-kernel.ps1 -Source examples\qcos_kernel.rvdl

# 打包为可启动 ISO（需 GRUB 2 grub-mkrescue + xorriso）
.\scripts\build-qcos-iso.ps1

# 或用 grub-mkstandalone 生成 QEMU -kernel 直启镜像
.\scripts\build-grub-img.ps1
```

```bash
# QEMU 启动（串口观察内核输出）
qemu-system-x86_64 -cdrom runtime/build/qcos.iso -display none -serial stdio
```

示例：`qcos_hello.rvdl`（syscall ABI + QMS 数值内核）、`qcos_kernel.rvdl`（串口 + 位图分配器 + IDT 打包）、`qcos_quantum_service.rvdl`（量子服务下沉）、`rvdl_bitmap.rvdl`（位图分配器）、`rvdl_buddy.rvdl`（伙伴系统）、`rvdl_idt.rvdl`（IDT 条目打包）。

***

## ⚙️ 架构

```mermaid
flowchart LR
    A[.rvdl 源文件] --> B[Lexer 词法分析]
    B --> C[Parser 语法分析]
    C --> D[Semantic Analyzer 语义检查]
    D --> E[IR Generator 生成 LLVM IR]
    E --> F[TCP Daemon :50052]
    F --> G[LLVM ORC JIT]
    G --> H{硬件探测}
    H -->|在线| I[SuperconductingBackend 真实量子机]
    H -->|离线| J[QVM 本地模拟器]
    H --> V[VisualizationService 演示线路]
    V --> K[GET_SNAPSHOT]
    K --> L[qvm_visualizer 可视化器]
```

**编译管线**：`Lexer → Parser → SemanticAnalyzer → IRGenerator → LLVM IR → JIT Daemon (port 50052) → 执行`

**VSCode 运行流程**：编辑器 ▶ 按钮 → `ravendrel/runCode` 通知 → 语言服务器生成 LLVM IR → 连接 daemon（`:50052`，未运行则自动 `spawn runtime --daemon`）发 `COMPILE`/`EXECUTE` 帧执行 → 输出回显至「Ravendrel Console」。侧边栏经 `GET_SNAPSHOT` 读同一 daemon 的共享 QVM 快照，实时观察量子态。

**实时可视化数据流**：守护进程内的 `VisualizationService` 在真实 `qhal::QVM` 上演化 5-qubit 演示线路，将态矢量 / 门事件 / 测量结果 / 对象列表序列化为快照，经 `GET_SNAPSHOT` 命令提供给 `qvm_visualizer`。

***

## 🎮 rvdlRSP — 量子机器人仿真平台

`rvdlRSP/` 是一个量子机器人仿真平台（C++20，作为 runtime 的新 target），复用 `qhal`（QM/QVM）、`vedaRos`、`numqk`。

| 子系统        | 位置                   | 说明                                                                      |
| ---------- | -------------------- | ----------------------------------------------------------------------- |
| 物理内核 qpc   | `include/qpc/`       | 刚体动力学 + 碰撞检测 + 约束求解（参考 AlphaPHY），量子接口委托 QM/QVM                          |
| Vulkan 渲染  | `include/render/`    | 3D 渲染 + PBR（Cook-Torrance）+ 纹理采样 + OBJ/glTF/glb 加载 + 内置 PNG/JPEG 解码     |
| 蓝图编辑       | `include/blueprint/` | 行为树 + Material/Substrate 节点图 + Nuklear 可视化编辑器                           |
| QPU/QPL 设计 | `include/qpu/`       | 量子处理器与量子编程层设计器                                                          |
| 机器人电路      | `include/circuit/`   | 机器人电路设计与连接                                                              |
| PCG 框架     | `include/pcg/`       | 种子驱动程序化内容生成                                                             |
| 控制与 RL     | `include/control/`   | 量子 RL Agent + 端到端训练 pipeline + 真实物理环境 + 脑意识介入 + qbNs 桥接 + 义肢/义眼 + 场意识控制 |
| 硬件抽象       | `include/hardware/`  | 执行器 / 生物信号（EMG/EEG）/ 安全控制器 / 故障检测 / 可观测性 / HIL 在环测试                     |
| QCDRC 遥操作  | `include/qcdrc/`     | RGB 相机采样 + 全身动捕 + IK/动作映射 + 行为克隆 + 真实设备接入（OpenCV/ExternalMocap）         |

统一入口：`#include "rvdlRSP.hpp"`。

### 🦾 义肢 / 义眼（脑意识 + 量子 RL）

`include/control/prosthetic.hpp` / `prosthetic_driver.hpp` 提供脑意识控制的义肢与义眼，复用 `QuantumRLAgent`（量子探索）与 `ConsciousnessController`（脑量子波 → 兴奋度调制）：

| 组件                                                       | 说明                                   |
| -------------------------------------------------------- | ------------------------------------ |
| `ProstheticLimb`                                         | 义肢：脑意识「意图」→ 目标关节角，量子 RL 学习屈伸/抓握补偿    |
| `BionicEye`                                              | 义眼：脑意识控制注视方向（pan/tilt），量子 RL 学习追踪补偿  |
| `BionicEyeCamera`                                        | 义眼相机：实现 `ICamera`，输出随注视方向变化的真实 RGB 帧 |
| `ProstheticRobotDriver`                                  | 义肢 → Robot 刚体驱动（关节角映射为骨骼朝向）          |
| `ProstheticEnvironment` / `ProstheticPhysicsEnvironment` | 接入 `RLPipeline` 的端到端量子 RL 环境         |

安全与临床合规详见 [SAFETY.md](rvdlRSP/SAFETY.md)（ISO 14971 风险分析 / FMEA）与 [CLINICAL.md](rvdlRSP/CLINICAL.md)（FDA/CE 临床评估方案）。

### 🖥️ rvdlRSP Qt GUI

除命令行主程序外，`rvdlRSP_gui` 提供了一个基于 **Qt 6 Widgets + QVulkanWindow** 的桌面仿真界面（需安装 Qt 6，详见 [🛠️ 依赖与构建](#-依赖与构建)）：

| 组件        | 位置                                 | 说明                                                 |
| --------- | ---------------------------------- | -------------------------------------------------- |
| 仿真内核      | `gui/src/simulation_host.hpp/.cpp` | `SimulationHost`：物理步进 / 遥操作 / RL 训练 / QVM / 意识与脑机桥 |
| Qt 主窗口    | `gui/qt/main_window.cpp`           | QTabWidget 面板布局                                    |
| Vulkan 视口 | `gui/qt/vulkan_viewport.cpp`       | QVulkanWindow 真 3D 渲染视口                            |
| 面板        | `gui/qt/panels.cpp`                | World Outliner / Details 等面板与实体选中联动                |

构建时通过 `windeployqt` 自动部署 Qt 运行时 DLL，并在构建后自动代码签名（见 [🔐 代码签名与 WDAC](#-代码签名与-wdac)）。

***

## ✍️ SE — 轻量 rvdl 编辑器

`SE/` 是一个轻量 rvdl 桌面编辑器（Electron + CodeMirror 6）。

- **复用编译管线**：通过 `rootDir: ".."` 直接引用 `server/src` 的 lexer / parser / semantic / ir，零代码重复
- **文件操作**：打开 / 保存 `.rvdl` 文件（Electron 原生对话框）
- **运行**：生成 LLVM IR → 连接 daemon（`localhost:50052`）执行，输出回显至 Shell 窗口
- **编辑器**：CodeMirror 6 语法高亮 + 实时诊断 + dirty 状态跟踪

***

## 🤖 VedaROS — 量子机器人操作系统

`runtime/include/vedaRos/` 实现了一个深度集成量子计算的分布式机器人操作系统。它以 **QDDP（Quantum-classical Decentralized Distributed Protocol，量子-经典去中心化协议）** 取代 DDS，支持与 QbNS 脑量子波通信，并提供类 rclcpp 的客户端库。

| 模块     | 文件                                  | 说明                                                      |
| ------ | ----------------------------------- | ------------------------------------------------------- |
| 类型系统   | `core/types.hpp`                    | 类型安全序列化、量子-经典双通道负载                                      |
| 通信协议   | `core/quantum_transport.hpp`        | QDDP 去中心化节点通信（`Endpoint` / `QMessage`）                  |
| 客户端库   | `core/node.hpp`                     | 类 rclcpp 的 `Node`：发布/订阅/服务/动作/执行器                       |
| 自定义语言  | `core/rvdl_lang.hpp`                | rvdl 语言定义意识/思想/决策/消息/服务/动作；编译期为真实量子设备生成门序列、为 QVM 生成 C++ |
| 脑波桥接   | `bridge/brain_wave_bridge.hpp`      | 将 QbNS 脑量子波桥接至 VedaROS 节点网络                             |
| 坐标变换树  | `algorithm/tf_tree.hpp`             | 创新量子-经典坐标变换树                                            |
| 行为树导航  | `algorithm/behavior_tree.hpp`       | 多并行行为树导航路径规划                                            |
| 纠缠规划   | `algorithm/entangled_planner.hpp`   | 完全纠缠意识的运动规划与操作                                          |
| 流匹配规划  | `algorithm/meanflow_planner.hpp`    | 平均流规划器                                                  |
| 统一损失   | `core/unified_loss.hpp`             | 统一损失框架                                                  |
| 硬件抽象   | `hardware/hardware_abstraction.hpp` | 硬件抽象层 + 固定频率控制循环                                        |
| 电机/传感器 | `hardware/actuator_sensor.hpp`      | 统一电机/传感器接口（增量编码器采样）                                     |
| QLM 集成 | `quantum/qlm.hpp`                   | VedaROS QLM 封装 + `rvdl_veda_qlm_train` ABI              |

统一入口：`#include "vedaRos/vedaRos.hpp"`。

***

## 🧠 QbNS — 量子脑网络

`runtime/include/qbNs/` 实现面向脑机接口（BMI）的量子混合架构，支持四种模态（非侵入式 / 侵入式 / 无线 / 量子传感器）。

| 组件     | 文件                           | 说明                                                                                          |
| ------ | ---------------------------- | ------------------------------------------------------------------------------------------- |
| 神经编码器  | `Transducer.hpp`             | 将神经信号（`NeuralStream` / `SpikeTrain` / `LFP` / `EEGSpectrum` / `QuantumSensorReading`）编码为量子态 |
| 脉冲信念传播 | `SpikeBeliefPropagation.hpp` | 脉冲信号的信念传播                                                                                   |
| 分布式网络  | `rmx.hpp`                    | `Rmx`：脑节点 / QC 节点注册、自适应路由、实时控制回路                                                            |
| 脑量子波   | `qbw.hpp`                    | `BrainQuantumWave`：检索量子对象构造量子流，建立脑量子波分布链接                                                   |
| 顶层接口   | `qbNs.hpp`                   | `QbNS`：统一混合架构表层                                                                             |
| C 桥接   | `qbNSBridge.hpp`             | `rvdl_mind_read` / `rvdl_mind_train` / `rvdl_mind_feedback` 的 ABI 导出                        |

***

## 🖥️ QVM 可视化器

`qvm_visualizer` 是一个基于 Vulkan + GLFW + Nuklear 的实时量子可视化应用，通过 socket 读取守护进程的真实量子状态。

六个观察窗口：

| 窗口                  | 观察内容                               |
| ------------------- | ---------------------------------- |
| Quantum Circuit     | 量子线路 / 门序列                         |
| State Vector        | 各基态振幅（实/虚）+ 概率柱状图（≤8 qubit 全量展示）   |
| Bloch Sphere        | 单比特 Bloch 球投影（θ/φ）                 |
| Quantum Objects     | 活量子对象列表（类型 + qubit id）             |
| Measurement History | 测量结果时间序列 / 分布                      |
| Metrics             | FPS、帧时间、连接状态、generation、present 模式 |

**性能优化**：多帧在飞（per-frame `VkFence` 替代 `vkQueueWaitIdle`）、`VK_PRESENT_MODE_MAILBOX` 低延迟呈现、快照代数计数避免冗余拷贝、delta-time 节流。

**共享协议**：`runtime/include/gui/protocol.hpp` 定义了 daemon 与可视化器之间零依赖的 `StateSnapshot` 行协议（`GET_SNAPSHOT` → `END_SNAPSHOT`）。

***

## 🔌 推理服务 API

`rvdl serve <model.rvdlm>` 提供 OpenAI 兼容接口：

| 方法     | 路径                     | 说明                          |
| ------ | ---------------------- | --------------------------- |
| `GET`  | `/v1/models`           | 模型列表                        |
| `POST` | `/v1/chat/completions` | 对话补全（支持 `stream: true` SSE） |
| `POST` | `/v1/embeddings`       | 文本嵌入                        |

***

## 🔐 代码签名与 WDAC

`scripts/` 提供 Windows 下的代码签名与 WDAC（Windows Defender Application Control）部署脚本，用于在启用 WDAC 的主机上放行自行编译的可执行文件：

| 文件                                            | 说明                                                                 |
| --------------------------------------------- | ------------------------------------------------------------------ |
| `scripts/sign.ps1`                            | 使用自签名证书（`scripts/certs/ravendrel-codesign.pfx`）调用 `signtool` 对目标签名 |
| `scripts/deploy-wdac.ps1`                     | 部署 WDAC 补充策略（`scripts/wdac/supplemental.cip`），需管理员权限               |
| `scripts/deploy-wdac-fix.ps1`                 | 修复脚本（配合 WDAC 策略调整）                                                 |
| `scripts/build-linux.sh` / `install-linux.sh` | Linux 下的构建与安装脚本                                                    |

`rvdlRSP_gui` 在 `windeployqt` 部署 Qt 运行时后会自动执行 `sign.ps1`（`rvdlRSP/CMakeLists.txt` 已配置）。

> 证书与 WDAC 策略文件仅用于本机开发环境的签名/放行，请勿将私钥（`.pfx`）纳入分发。

***

## 🛠️ 依赖与构建

### 依赖清单

| 依赖                              | 用途                               | 必需           | 获取方式                         |
| ------------------------------- | -------------------------------- | ------------ | ---------------------------- |
| LLVM                            | 运行时（JIT / AOT）                   | ✅ 必需         | vcpkg / MSYS2 / 官方预编译二进制     |
| Kokkos                          | 运行时（数值/张量后端）                     | ✅ 必需         | 源码编译（可选 CUDA 后端）             |
| CUDA Toolkit                    | Kokkos 的 GPU 后端                  | ⚠️ 可选        | NVIDIA 官方安装                  |
| 摩尔线程 MUSA SDK                   | MUSA GPU 后端（类 CUDA，mcc + musa）   | ⚠️ 可选        | 摩尔线程官方安装 + 设置 `MUSA_HOME`    |
| Vulkan SDK                      | `qvm_visualizer` 可视化器            | ⚠️ 仅可视化器     | LunarG 官方安装                  |
| GLFW + Nuklear                  | 可视化器                             | ⚠️ 仅可视化器     | 已在 `vendor/` 提供              |
| Node.js + TypeScript            | VSCode 扩展 / 语言服务器                | ✅ 必需         | 官网安装                         |
| Electron + CodeMirror + esbuild | `SE` 编辑器                         | ⚠️ 仅编辑器      | `npm install`（SE 目录）         |
| OpenCV                          | `rvdlRSP` 真实相机（可选）               | ⚠️ 可选        | 官方安装 + `RVDLRSP_USE_OPENCV`  |
| Qt 6                            | `rvdlRSP_gui` 桌面仿真界面             | ⚠️ 可选（仅 GUI） | 官方安装 + `Qt6_DIR` 指向 Qt 安装    |
| Android SDK / NDK               | Android APK 构建（`rvdl build apk`） | ⚠️ 仅 Android | `sdkmanager` / 官方安装，NDK 26.x |
| Gradle 8.5 + JDK 17/21          | Android APK 打包                   | ⚠️ 仅 Android | 官方安装                         |
| Go                              | `installer/`（ravendrelup）        | ⚠️ 仅安装器      | 官网安装                         |

### LLVM（必需）

任选其一：

```bash
# 方式 A：vcpkg
vcpkg install llvm

# 方式 B：MSYS2（UCRT64）
pacman -S mingw-w64-ucrt-x86_64-llvm

# 方式 C：LLVM 官方预编译二进制
# 下载 https://github.com/llvm/llvm-project/releases 的 Windows 安装包
```

配置后需确保 `find_package(LLVM REQUIRED CONFIG)` 能找到 `LLVMConfig.cmake`（通过 `LLVM_DIR` 环境变量或 CMake 参数传入）。

### Kokkos（必需）

从源码编译（默认启用 CPU 线程后端；如有 NVIDIA GPU 可同时启用 CUDA 后端）：

```bash
git clone https://github.com/kokkos/kokkos.git
cd kokkos
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DKokkos_ENABLE_THREADS=ON \
  -DKokkos_ENABLE_CUDA=ON \                          # 可选：无 GPU 时去掉此行
  -DCMAKE_INSTALL_PREFIX=C:/Libraries/kokkos        # 改成你的安装路径
cmake --build build --target install
```

> 编译完成后，将 `CMakeLists.txt` 中的 `Kokkos_DIR` 指向 `<安装路径>/lib/cmake/Kokkos`，并将 include 路径改为 `<安装路径>/include`。

### CUDA Toolkit（可选）

仅当启用 Kokkos 的 CUDA 后端时需要。从 [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit) 下载安装，然后：

- 将 `CUDAToolkit_ROOT` 指向安装目录（如 `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.9`）
- 同步更新 `.clangd` 与 `c_cpp_properties.json` 中的 CUDA include 路径

### Vulkan SDK（仅可视化器）

从 [LunarG Vulkan SDK](https://vulkan.lunarg.com/) 下载安装。`find_package(Vulkan REQUIRED)` 会通过 `VULKAN_SDK` 环境变量定位。

### Qt 6（仅 rvdlRSP\_gui，可选）

从 [Qt 官网](https://www.qt.io/download) 安装 Qt 6（含 Widgets / Gui 组件），然后将 `rvdlRSP/CMakeLists.txt` 中的 `Qt6_DIR` 指向 `<Qt 安装目录>/lib/cmake/Qt6`（本仓库示例为 `C:/Qt/6.11.2/msvc2022_64`）。未安装 Qt 时 `rvdlRSP_gui` 会自动跳过，其余目标不受影响。

### 配置与编译

```bash
cd runtime
cmake -B build -S . -G "Ninja" \
  -DCMAKE_CXX_COMPILER="<你的 clang-cl / g++ / clang++ 路径>" \
  -DCMAKE_TOOLCHAIN_FILE="<你的 vcpkg 路径>/scripts/buildsystems/vcpkg.cmake" \
  -DLLVM_DIR="<你的 LLVM cmake 目录>" \
  -DKokkos_DIR="<你的 Kokkos 安装>/lib/cmake/Kokkos" \
  -DCUDAToolkit_ROOT="<你的 CUDA 安装目录>"
cmake --build build --config Release --parallel
```

> 以上 `-D` 参数仅为示例，请替换为你本机的真实路径。若通过命令行 `-D` 传参，可覆盖 `CMakeLists.txt` 中的硬编码值，无需改动源码文件。

### VSCode 扩展

- Node.js、npm
- `vscode-languageclient` / `vscode-languageserver` / `vscode-languageserver-textdocument`
- TypeScript

```bash
npm install
npm run compile
```

### 安装器

- Go 1.26+（`golang.org/x/sys`）

***

## 📚 文档

| 文档                                                         | 说明                                                                                                                        |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| [rvdl 语言手册](docs/rvdl-language-manual.md)                  | rvdl 语言完整参考：类型、控制流、函数与契约、量子操作、模块系统（`.rvdli`）、类型定义（form/trait/impl）、静态验证                                                   |
| [穿梭与追踪](docs/rvdl-shuttle-tracking.md)                     | `@layer` 的两个可选机制：关联度核 `w = exp(−d_L1/λ)`（无截断阈值）与渗透载荷通道、五策略代价表（含 c001 蝶点与 DNA 链多层架构）与 auto 启发式、三级保真阶梯与影子槽、诊断码全集、运行期不变量、线格式 |
| [rvdl 迁移手册](docs/rvdl-migration-manual.md)                 | 现有量子编程语言（OpenQASM 2/3、Q#、Quil、Silq）→ rvdl 的友好迁移：门映射表、`rvdl migrate` 用法、各语言迁移示例与限制说明                                       |
| [量子机器人仿真平台手册](docs/rsp-manual.md)                          | rvdlRSP 平台完整参考：物理内核、渲染、蓝图、量子 RL、义肢/义眼、QCDRC 遥操作、安全与临床合规                                                                   |
| [rvdl 量子学习手册](docs/rvdl-quantum-learning-manual.md)        | QLM / QML / Numqk 量子机器学习：变分电路训练、parameter-shift 反向、流匹配、脑机接口、推理 API                                                        |
| [TQNF 拓扑量子学习手册](docs/rvdl-topological-quantum-learning.md) | TQNF 拓扑量子神经场范式：三大支柱（拓扑/表征/耗散）与 `@layer` 标签对应、张量原语映射、量子深度学习含义、测量熵（混沌丰富度）                                                   |
| [qchain 量子区块链手册](docs/rvdl-qchain-manual.md)               | qchain 量子加密与量子区块链：后量子密码、QKD、QDBA、量子货币、可编程量子代币、rvdl 内置函数与 daemon C ABI                                                     |

***

## 📄 License

本项目以 [MIT 许可证](LICENSE) 开源，版权所有 © 2026 OpenMasons。

> 项目中引用的第三方库遵循其各自的许可证，详见 [THIRD\_PARTY\_NOTICES.md](THIRD_PARTY_NOTICES.md)。

