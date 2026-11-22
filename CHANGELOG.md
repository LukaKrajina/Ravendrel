# Changelog

本项目的所有重要变更都会记录在此文件中。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

> 精简说明：本文件只列「改了什么」。设计动机、实验数据与完整规格见 `docs/` 下对应手册与源码注释。

## [1.0.1]

### Changed

- **许可证迁移至 Apache-2.0**：`LICENSE` 由 MIT 换成 Apache License 2.0（版权归 OpenMasons，含明确专利授权）；`package.json`、`README.md`、`README.en.md`、`THIRD_PARTY_NOTICES.md` 同步；新增 `NOTICE`。
- **脚本集中到 `scripts/` 分类目录**：`build/{windows,linux,android,qcos,check,wdac}/`、`package/{vsix,release}/`、`install/`、`generate/`、`sign/`、根级 `build.bat`/`build-inc.cmd`/`esbuild.mjs` 一并归档；新增 `scripts/README.md` 导览；全部路径引用（package.json、CMake、CI、源码、文档）同步更新。
- **文档改为参考手册风格**：`docs/` 下 9 份手册重写为「用途 → 语法/API → 示例」结构，体积压缩 46–78%；README 双语言版精简为项目入口页（能做什么 / 怎么构建 / 文档索引）。
- **图标重设计为渡鸦风格并归档**：VS Code 语言图标（`.rvdl` / `.qk`、明暗主题、侧边栏）全部重画为鹰角风格几何渡鸦；`concepts/` 原画抠成透明背景作为子系统图标；`icons/` 按 `language/`、`branding/`、`products/` 分类；所有引用同步。
- **仓库规范**：`.gitignore` 分组整理并补全构建/私有产物规则；新增 `CONTRIBUTING.md`、`CODE_OF_CONDUCT.md`、`SECURITY.md`、`GOVERNANCE.md`、`CITATION.cff`、`.editorconfig` 与 GitHub Issue/PR 模板；修正 README 中过时目录路径。

## [1.0.0]

### Added

- **量子时间缩微调度（QτS / τ scaling）**：`runtime/include/qml/TauScale.hpp` + `TauAbi.hpp`；内核 `numqk::tau` 提供 `compose`、`generational`、`anneal`、`fold_depth`、`gear_ratio`、`route_cost`、`route_price`、`critical_path`、`budget_route`。
- **6 个 τ 语言内建**：`tau_alpha`、`tau_generational`、`tau_fold_depth`、`tau_gear_ratio`、`tau_route_cost`、`tau_route_price`（全 double 标量，模式同 MoeAbi）；全链路接线 + 悬停/补全/高亮；测试 `tau_scale_test.cpp`（55 项）。
- **`nqcmath` 与 `expermath` 可在 `.rvdl` 调用**：`runtime/include/qml/QMathAbi.hpp` 暴露 5 个内建——`qstate_purity`、`qstate_linear_entropy`、`qstate_renyi_entropy`、`math_dot`、`math_norm`；`numqk_math_test` 35 项断言。
- **LF2C 无损上下文压缩落地量子 AI**：`runtime/include/qlm/{LF2B,LF2P,LF2C2}.hpp` 三层实现；16 qubit 覆盖 32 token（压缩比 16×）；训练/推理统一 `encode_context`；接入 `AdaptiveLossWeights`；`lf2c_test.cpp` 性质测试。
- **`nqcmath` 完成**：`runtime/include/numqk/nqcmath.hpp`——定长/动态矩阵、常驻线程池 + 原子游标调度、缓存友好分块 matmul，归约逐位可复现。
- **`expermath` 完成**：`runtime/include/numqk/expermath.hpp`——偏迹/约化密度矩阵/熵/纯度、保真度/迹距离、Pauli 期望/QFI、复 Hermitian Jacobi 特征分解、QFT/Trotter 电路计划；不依赖 QVM。
- **Fox 复合控件**：`widget/composite.{hpp,cpp}` 菜单栏/下拉/组合框/可选列表/滚动分组/数字属性行；`fox_widgets_test.cpp` 33 项。
- **Fox 软换行**：`text/soft_wrap.hpp` + `text_area(wrap=true)`，词优先/CJK 码点硬断，视觉行命中与光标；`fox_wrap_test.cpp` 24 项。
- **`cgfx_polyline` 原语**：折线作为单条路径提交（FoxRoute/Nuklear 兜底全线接线）。
- **Vk mipmap 全量上传**：`make_image` 接受层数、逐级 `VkBufferImageCopy`、sampler 线性 mipmap，关闭 PNG mip 链被丢弃的 GAP。
- **穿梭第五策略 c001 蝶点**：`shuttle_strategy=4`，O(1) 深度 + 零附加资源；蝶点 digest 物化、DNA 链多层架构；`examples/c001_dna_chain.rvdl`。
- **`.rvdlm` ↔ safetensors/gguf 双向转换**：`runtime/include/qml/ModelInterop.hpp` 补齐反向导入 + `import_*_auto` + `gguf_meta_to_rvdlm`；CLI `rvdl export` / `rvdl import`。

### Changed

- **穿梭策略枚举扩至五种**：新增 `C001Point: 4`，`shuttle_strategy` 取值域 `{-1..4}`，编辑器与高亮同步。
- **`W-SHU001` 改走 warning 通道**：`TopologyError` 带 severity，`rvdl run/compile` 不再因警告中止。
- **GodHand 模块更名为 Bro**：C++ 文件名与日志前缀改名，C ABI 前缀 `gh_*` 保持不变。
- **`DAEMON_PORT` 单一来源**：由 `server/src/protocol.ts` 导出，消除 7 处硬编码 50052。
- **版本号单一来源**：发布脚本与 MMI 模块头版本从 `package.json` 读取。
- **字体路径收敛**：新增 `runtime/include/gui/src/font_search_paths.hpp` 统一字体候选清单。
- **穿梭 auto 轮次按策略解析**：编译期 `strategyDepth()` 与运行期 `depth_of()` 同式，`E-SHU007` 代价与实际轮次一致。
- **跨度公式订正**：支持负 coord 的真实跨度；退化跨度 `span ≤ 1` 走 1 轮；树式 `⌈log₂(n+1)⌉` 层、环式 `n` 轮；Grover 降级改为重规划。
- **网络值环境变量可配**：`RVDL_DAEMON_HOST/PORT`、`RVDL_SERVE_PORT` 等替代硬编码。

### Fixed

- **输入命名键静默失效**：`InputState::k_index` >=128 返回 -1 导致方向键等在所有文本控件失效；槽位改为即键值并补 `>=0` 守卫。
- **`text_area` 点击忽略水平滚动**：命中测试同步减去 `scroll_x`。
- **迁移器 MVP 缺口**：OpenQASM 3 for 步长（`[lo:step:hi]`）、Q# 多 operation / if-else / while、Quil DEFGATE 矩阵体，均补回归测试。
- **`Transmitter.cpp` 假网络发现**：模拟节点加 `NetworkNode::simulated` 标记并显示 `(simulated)`。
- **`MirModuleBuilder` 空指针占位**：两处 `ConstantPointerNull` 改为抛错。
- **QPU 主机地址去私网硬编码**：改为 `RVDL_QPU_HOST/PORT/ENDPOINT` 配置。
- **温度传感器模拟标记**：`ITemperatureSensor::simulated()`，`QM` 对模拟传感器明示。
- **RSP 插件扫描空目录**：状态栏给出说明而非静默。
- **`@layer` 两个新机制**：穿梭（`shuttle`）与追踪（`track`），默认惰性；新增诊断 `E-SHU001..008`、`E-TRK001..004`、`E-TOP008/009`、`W-SHU001`、`W-TRK001`。
- **fox 像素加速器 `utils/Pa.hpp`**：脏 tile / 遮挡剔除 / SDF 核心跨度 / VRS rate map 四机制逐位一致；`CpuBackend::set_accel(false)` 参考路径；`pa_parity_test`、`pa_pixel_benchmark`。
- **`fox_metrics_test`**：钉住字形基线不变量；`FontManager::source_of()`。
- **`utils/A2a.hpp`**：Android 特化层（热状态、大小核、TBR 附件策略、降级顺序、宿主上报），非 Android 惰性，`a2a_test` 无需真机。
- **`.rvdlm` → safetensors/gguf 导出**：`ModelInterop.hpp` header-only；写出逐字节确定，不能无损映射即报错；三份独立实现对账。
- **fox 取代 Nuklear**：`FoxHost.hpp` 运行时判定 + 失败回退（每条失败门打印原因）；`BackendPolicy` 纯状态机；`fox_host_test.cpp`。
- **fox 图元补完**：椭圆（精确 SDF）、三角形（绕向无关）、描边折线、双色渐变；`fox_path` C ABI。
- **自写 PNG 解码 + mipmap + 三线性**：`ciUd/fox/image/png.{hpp,cpp}`，不引 stb_image；`fox_png_test`（66 项）+ 向量生成器。
- **fox 性能陷阱记录**：脏 tile 按行带合并、实例哈希按字而非按字节。

## [0.9.3]

### Added

- **`@[coherence]` 多通道退相干建模**：`computeNoise()` 返回通道列表，按 T1→振幅阻尼、纯退相→相位阻尼分解注入。
- **morph 宏编辑器支持**：中英悬停文档、TextMate 高亮、补全关键字。
- **语义高亮细分**：token legend 7→13 类，按类别着色（关键字/类型/量子门/morph/注解等）。
- **智能感知补全**：前缀过滤 + 相关度排序，新增量子门类，注解用 Property 图标。
- **Linux/WSL 交叉验证**：本轮 C++ 改动以 clang++ 21 `-fsyntax-only` 通过。
- **测试与文档回填**：新增 15 项（SSA 支配树 / E-TOP007 / QRC），全量 255→270；语言手册补 `qrc_new` 三参与 `qrc_train_ex`。
- **morph 态射宏系统**：`server/src/morph.ts`，全局 + 卫生（α-重命名）+ 非 `#define`；元变量 7 类、`$(...)*` 重复、`#$name` 字符串化、限深递归 64；`morph.test.ts` 12 项。

### Fixed

- **少参调用 `qrc_train_ex`/`qrc_new` 导致 IR 崩溃**：缺失实参填占位值，补 `assert.doesNotThrow`。

## [0.9.2]

### Added

- **侧边栏编译目标新增 `android`**：`aarch64-linux-android` + NDK 提示；目标合法性校验。
- **一键构建 Android APK（`ravendrel.buildApk`）**：侧边栏与标题栏按钮，实时回显 Console，Windows 优先 Git Bash。

### Fixed

- **AOT 编译目标不生效**：`CMD_AOT_COMPILE` 载荷去掉多余 `compile` 前缀。
- **Windows 宿主 Android 目标补齐**：triple 映射 + ELF 输出后缀。
- **CUDA 通道下 `rvdl_qrc_*` 未声明**：`SandboxJIT.hpp` 自包含 `QrcAbi.hpp`。
- **Windows 资源编译失败**：`build.bat` 显式指定 `llvm-rc.exe`。

## [0.9.1]

### Added

- **非欧几里德曲面体几何原语**：`geodesic_distance`、`inversion`、`hyperbolic_metric`、`hyperbolic_distance` + `qml/GeodesicAbi.hpp`。
- **量子通道逆因果容量（回程能力）**：7 个内建 `retrocausal_*` + `qml/RetrocausalAbi.hpp` + `spacetime/RetrocausalCapacity.hpp`（Choi 矩阵、max-info、Doeblin、q/c 容量、q-变形）。
- **快子 KK 双空间嘈杂 CTC**：`retrocausal_ctc_*` 4 个内建 + `RetrocausalCTC.hpp`（KK 额外维、快子噪声、多拓扑容量）。
- **`@layer(dual=1)` 与三类新拓扑边**：`projection`、`inversion`、`crossing`。
- **拓扑调度入口**：生成 `@rvdl_topology_json` + `@rvdl_topology_entry`，运行时 `rvdl_runtime_run_topology(i8*)` 按 time 分层、码距并行调度。
- **新拓扑诊断**：`E-TOP005`（空槽）、`E-TOP006`（超 deadline）、调用传播边 `TopologyCallEdge`。
- **SoftLogic/神经/QMS 原语 ABI 化**：`qml/SoftLogicAbi.hpp`，`extern "C"` 统一导出；SandboxJIT 无条件绑定。
- **文档补齐**：语言手册 §12.12–12.14、§8.1、`hover-docs.ts` 中英说明。

### Changed

- **`E-TOP002` 放宽**：维度一致性检查改为 no-op，改推导 `projection` 边。
- **原语定义迁移**：从 `qhal/JIT.hpp` inline 迁到 `SoftLogicAbi.hpp` 单点定义。
- **MIR body 增加调度元数据**：`MirBody.layer?: {time?, thread, coord}`。
- **VSIX 单包双平台**：`bin/{win32-x64,linux-x64}/`，按 `process.platform` 自动选择；LLVM/Kokkos 静态链接，自带运行库。
- **Linux 旧发行版兼容**：`runtime/src/glibc_compat.cpp` 把符号需求从 GLIBC 2.43 降到 2.34，`-static-libstdc++`。

### Fixed

- **LLVM ORC JIT 符号解析失败**：原语改单点定义。

## [0.9.0]

### Added

- **Android 平台支持**：`rvdl compile android` AOT 目标 + `rvdl build apk`。
- **轻量 Android 运行时**：`runtime/android/`（QVM + JNI，无 LLVM JIT）+ Gradle APK 工程，一键编排。
- **Android bionic 适配**：`Ga.hpp` 跳过 `pthread_setaffinity_np`。
- **SandboxJIT TQNF 符号无条件绑定**：`.rvdli` 沙箱可执行量子推理原语。

### Fixed

- **QVM qubit 回收深层 bug**：单调高水位 + 空闲 id 复用栈，回收前复位 |0⟩，收缩 num_qubits，消除双重 release。
- **daemon 多连接与帧解析**：单连接改 `select` 多连接 + 分段读取（粘包/半包）+ 64MB 单帧上限。
- **`RVDL_NO_GPU` 环境变量**：强制禁用 GPU 探测。
- **IR 全局变量对齐**：硬编码 4 改为 `typeAlign(llvmType)`。
- **拓扑收集修复**：含 `export` 的 MMI 模块跳过 coord 占用检查。

## [0.8.0]

### Added

- **量子语言迁移框架 `rvdl migrate`**：`server/src/migrate/`（CircuitIR + 门分解 + 发射器 + 五个前端 qasm2/qasm3/qsharp/quil/silq），`rvdl migrate <file> --from <lang> [-o out.rvdl]`，按内容自动探测。
- **Qubit 层原生门补充**：`y`/`z`/`s`/`t`/`rx`/`ry` 六门全链路，纳入可逆编织取逆规则。
- **迁移手册**：`docs/rvdl-migration-manual.md`。
- **`reset` 测后条件翻转语义**：含中间 reset 的电路切到 QObject 层；`gates.ts` 的 `lowerToBasic` 完整门集分解。
- **经典控制流符号化**：OpenQASM 3 经典变量/赋值/while/复杂布尔 if 转译为 rvdl。
- **VS Code 右键菜单迁移**：`ravendrel.migrateScript` 命令（.qasm/.qs/.quil/.slq）。
- **VS Code 侧边栏（仿 Flutter DevTools）**：编译目标、操作、性能监测、量子对象与比特四视图。
- **执行改走 daemon 模式**：connect `localhost:50052` 发 COMPILE/EXECUTE/AOT_COMPILE 帧，未运行自动 spawn。

## [0.7.0]

### Added

- **QRC 量子储备池**：5 个内建 + `QReservoir` 类型 + `qml/QrcAbi.hpp`，无贫瘠高原验证。
- **TQNF 拓扑量子神经场（语言层）**：QObject 门、POVM 弱测量、`qexpect_z`、`qmeasure`、`qattention`、`qstate_entropy/fidelity`、`shannon4/8`、`dla_dim`。
- **量子线性类型（QLT）补全 + 隐式 Drop**：具体量子类型纳入 LINEAR_TYPES，函数末尾隐式释放，仅裸 Qubit/QObject 强制显式消费。
- **可选 GPU（CUDA dlopen 延迟加载）**：运行时探测 NVIDIA，无 GPU 回退 CPU，lazy Kokkos 随包分发。

### Fixed

- **`QObject.measure()` 消费语义**：显式 Consume，修复裸类型 `.measure()` 误报 E-Q002。

### Changed

- `.vscodeignore` 移除 `vendor/**`，第三方库随包分发；`.gitignore` 加白名单例外。
- 具体量子类型由「报 E-Q002」改为「隐式 Drop」。

## [0.6.6]

### Added

- **一行多变量定义**：`int32 a = 1, b = 2;` 新增 `VariableDeclarationList` AST，全链路支持，后声明可引用先声明。
- **悬停提示详细文档（中英双语）**：`server/src/hover-docs.ts`，按系统语言切换，按「类型→内置函数→关键字」优先级查询。

## [0.6.5]

### Added

- **读寄存器内建（x86）**：`read_cr*`、`write_cr*`、`invlpg`、`rdmsr`/`wrmsr`、`cpuid`、`rdtsc`、`read_rflags`、`xgetbv`，读回 uint64。
- **裸汇编块 `asm { }` + `@[naked]`**：多指令序列、无栈帧函数，覆盖中断存根/切栈/远返回。
- **函数指针与间接调用**：`fn<ret(params)>` + `&fn` + `cb(args)`。
- **`@[packed]` 精确字节布局**：精确映射 GPT/FAT32/ATA 磁盘结构。
- **volatile 内存访问**：`volatile_load`/`volatile_store`，MMIO 轮询不被优化掉。
- **定长数组**：`arr<T,N>` + `[...]` + `a[i]`。
- **复数类型**：`complex64/128` + 算术 + `complex`/`real`/`imag`/`cabs`/`conj`。
- **位域**：`T field : N` 打包读写。
- **完整 uint64 运算**：无符号 div/rem/比较 + 符号性传播 + 64 位字面量精度。
- **IR 校验器增强**：剥离字符串内容再扫 SSA，避免 asm 模板 `%cr3` 误判。

### Fixed

- 修复 `null` 在 `cap<T1>→cap<T2>` 重解释分支被误 bitcast 的回归。

## [0.6.0]

### Added

- **多维标签函数 `@layer`**：以 `@layer(time, thread, coord[, cost, deadline])` 取代单一入口，编译期推导拓扑、运行时分层调度。
- **可逆编织门合成**：`@[gate]`/`@[undo]`/`@[steer]`/`@[unitary]`/`@[measure]`，`gate-synth.ts` 自动合成 U† 与 Λ(U)。
- **受控门运行时**：`cx`/`ch`/`crz`/`cswap`/`c_toffoli`/`cqft`/`cbraid` + `iqft`。
- **量子物理特性标签**：`@[coherence]`/`@[noise]`/`@[basis]`/`@[decoherence_free]`/`@[error_correction]`。
- **噪声注入通道**：门后自动注入噪声到 QVM 与四个硬件后端。
- **经典编译属性**：`@[inline]`/`@[noinline]`/`@[pure]`/`@[readonly]`/`@[cold]`/`@[hot]`/`@[noreturn]`/`@[export]`。
- **IR 下沉（MIR → LLVM C++ API）**：`MirModuleBuilder` + `verifyModule` + 80+ 内建符号表；`cli.ts --mir`。
- **`spawn` 真实并发**：编译为线程函数 + `rvdl_spawn`（std::thread detach），支持闭包捕获。
- **lambda lowering**：lambda body 量子操作纳入借用检查。
- **非破坏 `expectation_z`**：三个后端重写 ⟨Z⟩ = 1 − 2·P(1)。
- **光子去极化**：改为正确的 Pauli 通道（X/Y/Z 各 3p/4）。
- **新示例**：`examples/reversible_weaving.rvdl`。
- **VS Code 扩展打包**：esbuild 单文件 bundle，runtime 产物进 `bin/`，安装即运行；运行/编译/构建命令与快捷键（`Ctrl+Alt+N/C/B`）。
- **语法高亮与语义补全**：注解标签与受控门。

### Fixed

- 修复 `@[gate] @layer(...)` 混排解析崩溃、SSA 带参函数误报、`rz` 签名参数数、受控门借用/竞争误判、`section`/`naked` 命名分裂、`malloc` 与 JIT 冲突、`build.bat` 编译器/CRT 统一、`prepare-vsix-runtime.ps1` 的 Join-Path 与编码问题。

### Changed

- 入口：`rvdl_main` 由 `@layer` 标签函数取代（无标签回退脚本模式）。

## [0.5.0]

### Added

- **完整语法高亮**：重写 `ravendrel.tmLanguage.json`，覆盖 100+ 关键字/类型/内置函数/数字/字符串。
- **语义能力（Language Server）**：语义高亮、悬停、文档大纲、定义跳转、全量补全。
- **扩展图标**：语言图标（明暗双主题）、文件图标主题、应用图标（runtime / rvdlRSP，多尺寸 ico + 512 PNG）。

### Fixed

- 注释高亮由 `--` 改为 `//`；语言图标明暗主题区分。

### Changed

- 扩展打包元数据补齐（publisher/icon/repository），`.vscodeignore` 精确控制，移除误装依赖。

## [0.4.0]

### Fixed

- **QCOS 内核 shim 补 syscall ABI**：补齐 `rvdl_gc_free`/`rvdl_sys_*` 与 `malloc`，新增 `runtime/qcos/qcos_syscall.hpp`。

## [0.3.0]

### Added

- **QCOS 可启动内核**：`runtime/qcos/`（Multiboot2 引导、内核入口、链接脚本、GRUB 配置），隶属 [QuarkOS](https://github.com/LukaKrajina/QuarkOS)。
- **内核核心库 `qcos_core`**：freestanding 静态库 + 冒烟测试 `qcos_smoke`。
- **纯 RVDL 内核构建链**：`build-qcos-kernel.ps1`、`build-qcos-iso.ps1`、`build-grub-img.ps1`。
- **Windows 自动化构建脚本**：`build-runtime-windows.ps1`、`build-kokkos-windows.ps1`。
- **clang++ Windows 构建支持**：`if(MSVC)` 改 `if(WIN32)`，统一 CRT，`/ENTRY:mainCRTStartup`。
- **条件分支 `if / else if / else`**：新增 `IfStatement` AST 与全链路 lowering。
- **新示例**：QCOS 内核系列、`rvdl_idt`/`rvdl_bitmap`/`rvdl_buddy`/`rvdl_basis_state`/`if_branching` 等。

## [0.2.0]

### Added

- **工程地基**：单元测试框架、CI 流水线、统一分级日志、ASan/UBSan、`.clang-format`。
- **核心仿真补充**：IK 求解器、量子电路仿真、VedaROS 桥数据流、物理阻尼。
- **质量门禁**：覆盖率选项、物理稳定性回归、`rvdlRSP_benchmark`。
- **Broadphase 接入**：BVH 宽相，碰撞检测 O(N²)→O(N log N)。
- **打包**：CPack 安装器（TGZ/DEB）。
- **QCOS 驯龙系统 PMM**：`DragonPmm`（伙伴系统 + 空闲链表 + TLSF + per-CPU 缓存 + 票据锁）。
- **rvdl 语言新功能**：系统级编程（`cap<T>`/`unsafe`/`native`/端口 I/O/`sync_*`/堆/syscall）、量子化命名（`route`/`spin`/`fixed`/`flavor`/`fuse`）、并发（`spawn`/`entangle`）、QMS 内核、`@[section]`/`@[naked]`/`make`；新增 MIR、借用检查（QLT）、Q-Digest、VCGen。
- **任意基构建量子态**：`basis_state(theta, phi, value)` + `rvdl_create_basis_state` C ABI + `apply_basis` 门。
- **国产化适配**：LoongArch（LA64）检测与 `dbar 0`、摩尔线程 MUSA GPU 后端检测。

### Fixed

- 修复 Kokkos 并行积分时 View 构造崩溃、CUDA driver 卸载崩溃、碰撞法线方向导致的穿透。

### Changed

- 物理阻尼从硬编码 `*0.999` 改为步长无关阻尼系数；全模块日志迁移到统一分级日志。

## [0.1.0]

- 初始版本：量子机器人仿真平台，含物理引擎（qpc）、渲染（render）、量子电路（circuit/qpu）、遥操作（qcdrc）、强化学习（control）、硬件抽象（hardware）、VedaROS 桥（bridge）等子系统。