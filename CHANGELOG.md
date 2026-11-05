# Changelog

本项目的所有重要变更都会记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [1.0.0]

### Added

- **量子时间缩微调度（QτS / τ scaling）落地 runtime**（新增 `runtime/include/qml/TauScale.hpp` + `TauAbi.hpp`）。数学来源：何庭波主任的《A time scaling theory for multi-layer electronic systems》（ChinaXiv:202605.00224v2，韬定律）的四条公式/机制，联想延申到量子 AI 层：分层复合（τ = f(τ_state, τ_gate, τ_measure, τ_comm)，量化管线四层时间常数）、代际法则（τ_{n+1} = τ_n/α，α 领域相关：移动 1.3 / 自动驾驶 1.5 / AI 10）、LogicFolding 折叠与齿轮比（串行深度 L 折进 T 个并行 tier 后关键路径 ⌈L/T⌉，g = 跨层边/层内边，论文阈值 3）、时间影子价格路由（净价值 v − λ·c 的门控 + 拉格朗日二分预算路由 `budget_route`）。内核 `numqk::tau` 提供 `compose` / `generational` / `anneal` / `fold_depth` / `gear_ratio` / `route_cost` / `route_price` / `critical_path`（Kahn 拓扑序 DP）/ `budget_route`。
  - **语言内建 6 个**（全 double 标量，模式同 MoeAbi）：`tau_alpha(preset)`、`tau_generational(tau, alpha)`、`tau_fold_depth(L, T)`、`tau_gear_ratio(cross, intra)`、`tau_route_cost(depth, gear, t_gate, t_comm)`、`tau_route_price(value, cost, lambda)`。
  - 接线完整：`semantic.ts`（签名检查）→ `mir.ts`（`BUILTIN_RET_TY`）→ `ir.ts`（`declare` + `scalarMathFns`）→ `SandboxJIT.hpp`（`add`）→ `JIT.hpp`（`HostApiMap`）→ `TauAbi.hpp`（C ABI，遵循 MoeAbi 模式）。加上 `hover-docs.ts` 悬停文档、`server.ts` 补全项、`rvdl.tmLanguage.json` 语法高亮。
  - 测试：`runtime/tests/tau_scale_test.cpp`（55 项性质断言：复合/代际/α 预设/折叠边界/齿轮比零守卫/关键路径含环安全/影子价格/预算路由可行性与单调性），已注册 CMake 目标 `tau_scale_test`，MSVC 下编译运行全过。
  - 消费方：Chimera 架构 v3.0 新增第 30 个范式集群「时间缩微（τ）」（`Chimera/src/chimera_tau.rvdl`，6 组件，情绪时间预算 / 折叠调度 / 影子价格路由），与既有组件及 Jarvis 模型版本间兼容（纯新增，不改既有签名）。

- **`nqcmath` 与 `expermath` 可在 `.rvdl` 语言调用**（新增 `runtime/include/qml/QMathAbi.hpp`，5 个语言内建）。这是「升级 rvdl 语言」的落点：把上一条 `nqcmath`（普通数学 + 并行层）与 `expermath`（量子数学）暴露为语言层可调用的函数，而非只活在 C++ 头文件里。新增内建：
  - **expermath 量子态量度**（`QObject` → `double`，走 `qml::rho_matrix_of` 把约化密度矩阵从 `numqk::CTensor` 桥到 `expermath` 的 `MatrixX<Complex>`）：`qstate_purity`（Tr(ρ²)）、`qstate_linear_entropy`（1−Tr(ρ²)）、`qstate_renyi_entropy(q, alpha)`（Rényi-α，α=1 退化为 von Neumann）。它们与既有的 `qstate_entropy`/`qstate_fidelity`（走 `ComplexTensor` 的 CTensor 路径）**并存**，各有各的数值内核 —— 这正是「expermath 被 rvdl 调用」的独立落点。
  - **nqcmath 向量运算**（`arr<double,N>` → `double`，数组 decay 成 `double*` 后走 `nqcmath::Parallel::parallel_reduce` 常驻线程池 + 原子游标归约）：`math_dot`（点积）、`math_norm`（L2 范数）。
  - 接线完整：`semantic.ts`（签名检查）→ `mir.ts`（`BUILTIN_RET_TY`）→ `ir.ts`（`declare` + 数组 GEP decay 发射）→ `MirModuleBuilder.hpp`（`builtinSigs`）→ `SandboxJIT.hpp`（`add`）→ `JIT.hpp`（`HostApiMap`）→ `QMathAbi.hpp`（C ABI，遵循 SoftLogicAbi 的「extern "C" 声明 + `#if RVDL_RT_BUILD` 非 inline 定义」模式，由 `runtime_api.cpp` 单点 include）。顺带补齐 `hover-docs.ts` 文档、`server.ts` 补全项、`rvdl.tmLanguage.json` 语法高亮的 `qstate_`/`math_dot|math_norm`。测试：`ir.test.ts` 加 3 项（declare 签名 / double 数组 GEP decay / QObject 调用的发射）、`numqk_math_test.cpp` 加 Rényi-2 熵与 α→1 退化两项。

- **LF2C（无损特征快速上下文压缩）落地量子 AI**（`runtime/include/qlm/LF2B.hpp`、`LF2P.hpp`、`LF2C2.hpp`）。三处原本是**空壳**：`LF2B.hpp` 里一个空类 `BlockChunker {}`（且它引用的 `Matrix<Scalar,R,C>` 当时根本不存在）、`LF2P.hpp` 完全是空文件、`LF2C2.hpp` 只有一段中文设计意图。现在三层齐备：
  - **LF2B（块层）**：`BlockChunker` 把 token 上下文切块并抽**邻域特征**（定长 16：8 个比特密度、均值、标准差、首尾 token、归一化熵、与左右邻块的均值差、邻域平均）。邻域而不只是块自身：只有块自身的信息撑不起「下一块是什么」的预测。
  - **LF2P（池层）**：`LosslessPool` 存**原文块** + 特征表 + **动态指针**（`ChunkPtr` = 池 id + 块号 + 特征偏移，三个整数访问上下文，不复制原文；带池 id 是为了**陈旧检测** —— 换池后旧指针必须失效，而不是静默指到别的块上）。`sumChunkloss` 是「只凭特征能重建多少」：用**岭回归解码器**（闭式解、无随机性 ⇒ 同样特征永远给同样损失；且解码器是度量工具不是模型 —— 百万参数的解码器能把任何特征「记住」，那 loss 就没意义了）。
  - **LF2C2（协调者）**：多子线路 —— 每块一条量子子线路（邻域特征 → DCT-II 投影 → RY 旋转 → CNOT 梯纠缠 → ⟨Z⟩ 读出 = 量子特征），逐块入池。子线路之间靠「测量 + 条件 X」复位隔开（稠密态矢量的内存是 2^n，另开一组工作 qubit 就是翻倍，那不叫压缩）。
  - **集成到量子 AI 的上下文窗口**（本仓最该用的地方）：QLM 的上下文原来硬编码成 **16 qubit = 2 个 token**（`QLM.hpp` 的 `context.window = "2_Tokens"`，推理侧 `qml/Inference.hpp` 取生成串最后两个 token 用 `apply_x` 打进 16 qubit）。生成串无界增长而窗口只有 2 个 token —— 中间全丢、2 个 token 也撑不起长程依赖。现在**同样的 16 个 qubit 覆盖 32 个 token 的历史**（压缩比 16×，qubit 层面），被压掉的那段不再是「丢弃」而是「进了特征」。
  - **训练与推理走同一个函数** `qlm::lf2c2::encode_context`：两侧编码必须一致，否则模型是在训练分布之外被推理（train/test skew），输出是垃圾而且**不报错**。收敛到一个函数里，skew 就从「约定」变成「结构上不可能」。
  - 损失侧接进 `vedaRos::core::AdaptiveLossWeights`：新增第 8 项 `λ_lf2c`（= `sumChunkloss`），压缩器与主任务联合训练。
  - `runtime/tests/lf2c_test.cpp`（已注册 CMake）：断言的是**性质** —— 恢复的原文**逐字节**相同（无损是构造性的）、跨池指针被判无效、特征无信息时 `sumChunkloss > 0`、块全相同时 `≈ 0`、qubit 压缩比 = 16、特征表恰好填满上下文 qubit（窗口给小不会报错，只是**一半的上下文 qubit 拿不到特征** —— 状态没崩、loss 也在降，就是白扔一半算力）。

- **`nqcmath` 完成：普通数学库+ 高并发并行层**（`runtime/include/numqk/nqcmath.hpp`）。
  - `Matrix<Scalar,R,C>`（定长、栈上、**维度进类型** ⇒ 形状不匹配在编译期就被抓住）、`MatrixX<Scalar>`（动态）、`Vector<T>`；行主序，与 `numqk::Tensor` 一致。
  - `Parallel`：**常驻线程池**（进程内单例、惰性创建）+ **原子游标动态块**调度（静态切分下最慢的块决定整帧耗时）+ **缓存友好的分块 matmul**（i-k-j 序，64×64×32 分块）。教科书式的「每次调用 spawn 一批线程再 join」在 QLM 那种每帧都算的场景里是纯开销（一次 spawn ≈ 20-50µs，而 144×144 的矩阵乘只要几微秒）—— QLM 的量子几何张量正是 p×p（p≈144），此前 `Tensor::matmul` 是单线程朴素三重循环。
  - 归约的**可复现性**：块数固定为常量（不随线程数变），块内串行、块间按序合并 ⇒ 结果与机器核数无关、逐位可复现（浮点加法不满足结合律，「并行求和」与「串行求和」的末位必然可能不同 —— 这一点写进了头文件，而不是假装没有）。
  - 顺带修掉旧 `parallel_elementwise` 的三处错：`vector(n)` 预分配后又 `emplace_back` ⇒ 线程数变 2n、**前 n 个是空线程**（join 空线程是 UB）；分块起点 `t*chunk + (t==0?0:t)` 的 off-by-one；`op` 只给 index、行列要调用方自己再算。
- **`expermath` 完成：自研高性能量子数学库**（`runtime/include/numqk/expermath.hpp`，此前是**空命名空间**）。不依赖 QVM —— 全部作用在振幅缓冲上，于是可被单元测试直接验证（QVM 的态要跑电路才有）。算子与依据：
  - 偏迹 / 约化密度矩阵 / von Neumann 熵 / Rényi 熵 / 纯度 / 线性熵 / 二分纠缠熵（偏迹的谱熵）。
  - 重叠保真度（量子核的核函数）与迹距离 —— Huang et al., *Power of data in quantum machine learning*, Nature Communications 12, 2631 (2021)。
  - Pauli 串作用与期望、Pauli 分解的哈密顿量期望、量子 Fisher 信息（F_Q = 4 Var(G)）。
  - **复 Hermitian Jacobi 特征分解**（含相位消解与特征向量累乘）—— 熵要谱、QFI 的 Petz 谱形式要特征向量；实对称的 Jacobi 不能用（复数情形 2×2 块要多一个相位）。
  - **电路生成**（数学层出「计划」、QVM 去执行）：QFT 电路（Cleve & Watrous, arXiv:quant-ph/9507287）、Trotter/Suzuki 一阶与二阶电路（Suzuki 1990；Shao et al., arXiv:1801.00862）、基态振幅编码。
  - QFT 的约定（受控相位的角度公式、H 的执行顺序、比特反转的位置）是**数值对照闭式解**定下来的，不是照抄某本书的图：`|0…0⟩` 上所有候选方案都给出均匀叠加（那个输入的所有相位都是 0），只有拿一般输入去对照 `QFT|j⟩ = (1/√2^n) Σ_k ω^{jk}|k⟩` 才分得出对错。
- **`numqk_math_test`**（`runtime/tests/numqk_math_test.cpp`，35 项断言，已注册 CMake 目标）：全部是**已知答案**测试 —— Bell 态偏迹必须是 I/2、最大纠缠二分熵必须是 1 比特、QFT 必须对上闭式解、H = Z 在 |0⟩ 上期望必须是 1。并行 matmul 与朴素实现逐元素比对、并行 for 每个下标恰好访问一次、归约与串行求和一致。

### Fixed

- **`Numqk.hpp` 里 `nqcmath::MatrixXf` 是空类、`expermath` 是空命名空间** —— 见上（两处均已补齐为完整实现）。
- **`expermath` 的 Pauli 作用中 Z 会翻转比特**（实现期自查发现）：Z 只该给相位 `(-1)^b`，不翻转。写成翻转的话 `⟨Z⟩` 全错，但 `⟨X⟩` 仍然对 —— 错得很「像那么回事」。

- **迁移器 MVP 缺口补齐**（`rvdl migrate`，四个此前静默出错或丢弃的缺口，均有回归测试钉住）：
  - **OpenQASM 3 for 循环步长**：`[lo:hi:step]` 三段式的**中间数是步长**（规范语义），曾经的实现读成 lo:hi 并丢弃步长——`for i in [0:2:6]` 静默迁出 `i = 0,1` 两步的错误电路。负步长（`[3:-1:0]` 倒序）同样支持（词法层把 `-` 独立成 token，范围解析显式消费符号）。
  - **Q# 多 operation**：曾经的实现只迁**第一个** operation——「helper 在前 + main 在后」的常见结构会静默丢掉 main，迁出完全错误的电路。现在第一遍登记全部 operation 的签名与 body 区间，调用图定入口（不被调用的 operation），helper 在调用点**内联**（形参绑定实参 qubit，被调 operation 自己的 `use` 分配新 qubit——与 Q# 语义一致）。递归环**指名道姓地报错**（环无法摊平），深度上限 32。
  - **Q# if/else 与 while**：曾经的实现把 if 整段跳过（静默丢失经典控制流）。现在条件经优先级解析（or > and > not > 比较 > 初等）翻译为 rvdl 布尔式：`One/Zero/true/false` → `1/0`，经典名 → `c<bit>`，`and/or/not` → `&&/||/!`，包成 `IfOp`/`WhileOp` 交 emitter；`else if` 链整体作为 alternate。函数调用形态（`Length(qs)` 等）明确报错，不产出无效 rvdl。
  - **Quil DEFGATE 矩阵体**：矩阵数据在缩进续行上，曾经的实现只跳到行尾，矩阵行被当作门调用解析。现在按「遇到更浅缩进即结束」吃掉矩阵体；调用 DEFGATE 定义的门 → 指名道姓的拒绝（矩阵门无 rvdl 对应物），不再静默跳过。
- **Fox 复合控件**（`widget/composite.{hpp,cpp}`，全部只由 primitives + 显示列表构成，不碰后端）：**菜单栏/下拉**（`menu_title`/`menu_popup`/`menu_item`，互斥浮层、悬停切换、点击外部/Esc 关闭、选中即关）、**下拉组合框**（`combo`，chevron、选中项勾选、越界 selected 钳位）、**可选列表项**（`selectable`）、**可滚动分组区**（`group_begin`/`group_end`，边框 + 标题 + 裁剪 + 滚动条）、**数字属性行**（`property_int`/`property_float`，标签 + 可拖值盒 + 聚焦后方向键微调）。C ABI 同步（`fox_menu_*`/`fox_combo`/`fox_selectable`/`fox_group_*`/`fox_property_*`，CSV 列表经自有存储拆分，不写调用方缓冲）。配套 `runtime/tests/fox_widgets_test.cpp`（33 项断言，无窗口/无 GPU 驱动 Context + 合成输入）。
- **Fox 软换行**（`text/soft_wrap.hpp` + `text_area(..., wrap=true)`）：长行拆成**视觉行**——词优先（断在最后一个容得下的空格/制表符之后），无空格可依（CJK）按码点硬断，单个超宽码点独占一行并合法溢出（好过丢字）。编辑模型不变（TextBuffer 仍只认 `\n`，软换行是布局层属性）；命中测试、光标定位、Up/Down/PageUp/PageDown（像素粘性列）、Home/End（视觉行边缘）、选区带、滚动条比例全部按视觉行工作。行表按（widget id、文本指纹、宽度、字体）缓存——指纹是 FNV-1a，~1ns/字节，比它守卫的字形度量便宜两个数量级。配套 `runtime/tests/fox_wrap_test.cpp`（24 项断言：行段连续无隙、覆盖全行、拼回原文、断点在空格后、不劈多字节码点、退化宽度、确定性）。
- **`cgfx_polyline` rvdl 原语**（折线作为**一条路径**提交）：`FoxHost::stroke_polyline` → `classic_route::polyline` → `rvdl_cgfx_polyline(_a)`（Nuklear 兜底走 `nk_stroke_polyline`）→ IR/MIR/JIT/SandboxJIT 符号表全线接线。逐段调 `cgfx_line` 会在每个接缝各画各的帽（夹角处缺口/重叠）——这正是勾选框曾经用 16 个圆点的同一条理由。曲线/图表/状态机迁移线都吃这个差别。
- **Vk mipmap 全量上传**：`make_image` 接受层数（按 `1+floor(log2(min(w,h)))` 钳制），`upload_texture` 逐级 `VkBufferImageCopy`（每级自己的 mipLevel/bufferOffset/extent，extent 逐级减半最小 1x1，与 `build_mips` 一致），屏障的 `levelCount` 覆盖全部层级，image view 同样；sampler 开 `mipmapMode=LINEAR` + `maxLod=16`。纹理的层数变化（`create_texture` ↔ `create_texture_mipped`）触发图像重建——拿 1 级图像拷 5 级数据，vkCmdCopyBufferToImage 只会静默忽略越界区域。这关闭了 sampler 处记载的 DOCUMENTED GAP：PNG 自带的 mip 链此前在 GPU 上被整个丢弃，强缩小下 GPU 比 CPU rasteriser 走样更重。

### Fixed

- **`InputState::k_index` 越界读（命名键全部静默失效）**：`k_index` 曾对 >= 128 的键返回 -1，于是 `key_press(Key::Left)` 求值成 `keys_pressed[-1]`——越界读数组**前一个字节**（恰好是 `keys_down` 末尾一个从不被写入的槽位，恒为 false）。症状是方向键 / Home / End / PageUp / PageDown / Backspace / Delete / Enter / Escape 在**所有**文本控件里静默失效，而守卫 `k_index(k) < KEY_SLOTS` 对 -1 恒真，拦不住它。文本测试只测 TextBuffer 模型、不经按键路径，所以这个 bug 活着且无人知晓。现在槽位即键值（ASCII 0..127 与命名键 0x100..0x11C 共用同一索引空间，KEY_SLOTS 存在的全部理由），守卫补 `>= 0`。由 `fox_widgets_test` 的 [1] 组钉住。
- **`text_area` 点击命中忽略水平滚动**：`hit_at` 用 `mx - (r.x + pad_x)` 而绘制用 `origin.x = r.x + pad_x - st.scroll_x`——横向滚过的行上点击，落点系统性偏左。现在命中测试同样减去 `scroll_x`（软换行后只剩「单个不可断片段比盒子还宽」这一种需要横向滚的情形）。

### Changed

- **硬编码网络值改环境变量可配**：RSP 脚本编辑器的 daemon 探测（`RVDL_DAEMON_HOST` / `RVDL_DAEMON_PORT`，默认回环 + 50052）、SE runner 的 `DAEMON_HOST`（`RVDL_DAEMON_HOST`）、`rvdl serve` 端口（`RVDL_SERVE_PORT`，优先级：`--port` > 环境变量 > 默认 9080）。容器化/脚本化部署（无法改命令行）此前只能改默认值，而那会影响所有用户。

- **穿梭第五策略：c001 式（蝶点，`shuttle_strategy=4`）**。把蝶式「逐位翻转跳过去」改造成「**压缩成一个点再辐射**」：全体关联层各上传主要特征摘要（关联度加权的经典标量，即既有渗透载荷通道），汇入一个显式**蝶点**；蝶点持有全体层特征总览，再按相位语义向全体辐射。三条性质：
  - **深度恒 1 轮**（汇聚与辐射在同一轮的双侧相位内完成），与格点跨度 `span`、对端数全部无关 —— 比蝶式（`O(log span)`）省，比 Grover（`O(1)` 但需 2ⁿ 工作寄存器）实用：**O(1) 深度 + 零附加资源**。
  - **蝶点 digest 显式物化**：运行期把蝶点内容（全体对端的关联度加权摘要）打进 `[Shuttle] c001 蝶点 digest: …` 诊断行，`ShuttleRuntimeStats` 新增 `c001_points` 计数。「压缩成点」不是修辞，是日志里看得见的量。
  - **DNA 链多层架构**：多个穿梭群各有一个蝶点，蝶点之间经既有五类拓扑边互联成**蝶点链**（主链），层↔蝶点的连接即**横档（碱基对）**——每个关联层经自己的蝶点与全体关联层配对，形成双螺旋式多层结构。`phase=2` 时每个蝶点同时是上游的汇与下游的源（两条互补链）。该结构**不新增 `@layer` 键**，由多蝶点群 emergent。
  - 落地与既有纪律对齐：编译期 `strategyDepth()` 与运行期 `depth_of()` 同式（`case 4 → 1`），`E-SHU005` 取值域扩至 `{-1..4}`；`shuttle_topology_parity.cpp` 钉「解析深度 ↔ 真实展开」对账，`shuttle_plan_benchmark.cpp` 加 c001 列（深度恒 1、边数 = 对端数）；auto 启发式新增分支（span 大且无 ancilla → c001）。示例 `examples/c001_dna_chain.rvdl`（多蝶点群 + 蝶点链 + 自检「远端对端 1 轮即达」）。完整规格见 [docs/rvdl-shuttle-tracking.md](docs/rvdl-shuttle-tracking.md) §3.7。
- **`E-TRK002`（追踪源在块作用域内不可解析）落地**。该诊断此前只存在于文档与 CHANGELOG，代码 0 处实现。现在 `topology.ts` 的 `buildTrackingScopes` 做静态检查：声明了需量子源的 track 可观测量，但函数体（含参数）不存在任何量子源（无 `Qubit`/`QObject` 参数、无 `new QuantumRegister/DiracState/BellState`、无 `alloc()`）→ 报 `E-TRK002`。补 `tracking.test.ts` 用例。
- **`.rvdlm` ↔ safetensors / gguf 双向容器转换补齐**（`runtime/include/qml/ModelInterop.hpp`）。此前只有导出方向（`export_rvdlm_to_*` + daemon `EXPORT_*` 文本命令），反向导入完全缺失，且 daemon 的 EXPORT 文本命令对 CLI 实际不可达（`protocol.ts` 无对应命令，CLI 只走二进制帧）。现在：
  - `import_safetensors_to_rvdlm<T>` / `import_gguf_to_rvdlm<T>`（容器 → `.rvdlm`；T 与容器 dtype 对账，不一致即报错）+ `import_*_to_rvdlm_auto`（类型按**容器 dtype** 自动分发 —— 容器记录 dtype、`.rvdlm` 不记录，两侧信息不对称决定语法不对称：导出必须显式给类型，导入不必）。
  - `gguf_meta_to_rvdlm`：GGUF KV 段 → `.rvdlm` 元数据的反向映射（`general.name`→`name`、`general.architecture`→`architecture`、`general.alignment` 与其余 `general.*` 丢弃）。round-trip 契约是**不丢失的单调超集**：原键值全部完好，唯一可能多出的是 `architecture`（导出侧为兼容 llama.cpp 补的默认值，反向映射无从区分「补的」与「写的」）。没有这层映射，`rvdlm → gguf → rvdlm` 会静默丢掉 name/architecture 并多出三个格式键。
  - daemon 文本命令 `IMPORT_SAFETENSORS` / `IMPORT_GGUF`；二进制协议 `CMD_EXPORT_MODEL = 0x0E` / `CMD_IMPORT_MODEL = 0x0F`（`protocol.ts` 同步）；CLI 面 `rvdl export <safetensors|gguf> <in.rvdlm> <out> [tensor] [f32|f64|i32|i64|u8]` 与 `rvdl import <safetensors|gguf> <in> <out.rvdlm> [tensor]`。
  - 两个已知边界诚实报错而非猜：**BOOL 容器 → `.rvdlm` 不支持**（`std::vector<bool>` 是位压缩特化、没有 `data()`，`numqk::Tensor<bool>` 先天不可用 —— 先转成 U8/I32 再导入）；GGUF 量化类型（Q4_0 等）读取即报错。
  - 测试：`model_interop_test.cpp` 新增 [7] 反向导入节（round-trip 语义相等、auto dtype 分发、类型对账拒绝、张量名不匹配列出可用名、多张量容器缺省取第一个/指名取指定、未知 ggml 类型两侧都拒绝）；`model_interop_crosscheck.py` 新增反向验证（Python 独立解析容器 → 导出 `.rvdlm` 预期值 → 按 QKM1 规范独立解析产物核对）——「导出对」与「导入对」分别被证明，否则 round-trip 可能两头一起错。

### Changed

- **穿梭策略枚举扩至五种**：`ShuttleStrategy` 新增 `C001Point: 4`（c001 蝶点）。`shuttle_strategy` 取值域 `{-1, 0, 1, 2, 3, 4}`；编辑器悬停（`hover-docs.ts` 的 `LAYER_ENUM_VALUES`）与语法高亮（`rvdl.tmLanguage.json` 的枚举取值匹配）同步扩展。
- **`W-SHU001` 从 error 通道改为 warning 通道**。此前 `SemanticAnalyzer.errors` 无 severity，`server.ts` / `cli.ts` 见 `errors.length > 0` 即 abort —— 一个「Warning」把程序毙了，文档承诺的「退化为经典影子混合」永远走不到。现在 `TopologyError` 带 `severity`，`runTopology` 把 `W-*` 推入 `analyzer.warnings`：LSP 显示为 Warning、`rvdl run` / `compile` 不 abort（照常执行并在控制台打印告警），运行期仍按文档退化。`tracking.test.ts` 的 W-SHU001 断言同步改通道。
- **GodHand 模块更名为 Bro**（`runtime/src/GodHand{Ui,Str,Syntax}.cpp` → `Bro{...}.cpp`，`runtime/include/qhal/GodHand{Ui,Syntax,Str}.hpp` → `Bro{...}.hpp`；`CMakeLists.txt`、`qicon.cpp`、`render_context.cpp` 日志前缀 `[GodHand]→[Bro]`、`font_loader.h` 注释同步）。**C ABI 前缀 `gh_*` 保持不变**（外部宿主 `godhand_shell` 注册 `gh_text` / `gh_slot_*` 等符号，改 ABI 会断兼容；仓内无 `.rvdl` 示例引用 `gh_*`）。
- **`DAEMON_PORT` 收敛为单一来源**（`server/src/protocol.ts` 导出）。此前 50052 在 7 个文件各写一遍（`server.ts` / `cli.ts` / `apiRouter.ts` / `SE/src/runner.ts` / `client/src/ravendrelSidebar.ts` / `runtime/include/gui/src/main_gui.cpp` / `RSP/include/editor/rvdl_editor.hpp`）——改一处漏一处的后果是「有的组件连得上、有的连不上」，而两半各自都看不出错。TS 侧全部改为 import；VS Code 客户端（tsconfig `rootDir` 限制无法 import server 源码）与两个 C++ 宿主保留本地常量，注释指向 `protocol.ts`。
- **版本号单一来源**：`scripts/package-release-linux.sh` 从 `package.json` 读版本（曾写死 `0.6.6`，与 `package.json` 的 `0.3.3` 各自漂移，发布目录名对不上实际版本且无任何报错）；`cli.ts` 的 MMI 模块头版本随工具链版本（曾写死 `'1.0.0'`，三处互不相同）。
- **字体绝对路径收敛为具名常量**（新增 `runtime/include/gui/src/font_search_paths.hpp`）。此前 qgui 的 `font_loader.cpp` 与 fox 的 `text/font.cpp` 各写一份清单——内容与优先级都不同（各自演化，直接合并会改变多字体机器上的字体选中，那是渲染度量变化而非纯重构）。现在路径字符串是单一维护点，使用方按自己的优先级引用名字：「加路径漏一处」的隐患（症状是某平台缺字 = 豆腐块，而缺字**不报错**）从结构上消失。

### Fixed

- **`Transmitter.cpp` 不再用 `rand()` 伪造网络发现**。`startP2PScan` / `startQuantumNetworkScan` 此前用随机数生成假 IP（`192.168.1.x` / `10.0.0.x`）、假端口（8888+/7777+）、假延迟/保真度，却呈现为真实扫描结果——用户在 UI 上看到的是一个不存在的「网络」。现在这些节点一律带 `NetworkNode::simulated = true`，UI 明示 `(simulated)`，扫描启动时打印诚实告警。宁可显示得少，不能显示得假。
- **`MirModuleBuilder` 的两处空指针占位改为报错**。form 结构化构造（`NewObject` 的兜底分支）与无字段信息的 `Member` 访问此前 `return ConstantPointerNull`——后续代码对着 null 继续生成 IR，模块能 `verifyModule` 通过、能进 JIT，直到运行期段错误。现在抛 `std::runtime_error`（daemon 回 `RESPONSE: ERROR - MIR compile failed: …`），与文件内其余「unsupported …」分支的惯例一致：把「没做完」变成看得见的错误，比一个静默的 null 好。
- **QPU 主机地址不再硬编码私网 IP**。`SuperconductingBackend` 与 `runtime_api.cpp` 的默认值曾是作者局域网的 `192.168.1.100:50051`——那是一台特定机器的地址，对其它环境毫无意义，还会无谓探测别人的局域网。现在经 `RVDL_QPU_HOST` / `RVDL_QPU_PORT` / `RVDL_QPU_ENDPOINT` 配置，默认回环地址：没有配置就没有真实硬件，探测必然失败 → 上层回落本地 QVM（已有诚实日志）。
- **`ITemperatureSensor` 增加 `simulated()` 标记**。`CryostatThermometer`（占位实现：基准温度 + 高斯噪声）此前与真实驱动在日志里一模一样，而模拟读数的任何结论都不能拿去调硬件。现在接口默认 `false`、占位实现覆写为 `true`，`QM` 启动时对模拟传感器打印一行明示。`meanflow_planner` 的恒等端点预测器已有 `has_predictor()` 显式标记，保持不变。
- **RSP 插件扫描空目录不再静默**。`scan_plugin_directory` 在目录不存在或为空时直接 `return`——用户点了「扫描插件」却毫无反馈，无法区分「没扫到」和「扫了但什么都没发生」，而前者才是常态（`plugins/` 默认不存在）。现在在状态栏显示一行说明。

- **`@layer` 的两个可选机制：穿梭（shuttle）与追踪（track）**。二者都是 `@layer(...)` 内的**新键**（而非独立顶层标签），且**默认完全惰性**——不写则 `@layer(time=0, thread=0, coord=(0))` 的语义与诊断结果逐字节不变（既有 `system-dev` / `syslevel` 全部 ~40 个 fixture 仍断言 `errors` 深等于 `[]`）。完整规格见 [docs/rvdl-shuttle-tracking.md](docs/rvdl-shuttle-tracking.md)。

  **穿梭** `shuttle=(c,…)`：按 L1 码距定权的关联度核 `w = exp(−d_L1/λ)`，在共享点前后双向渗透进关联层（U-Net / DenseNet 式跳连、AdS/CFT 双侧可恢复、Christandl 完美态传态的经典类比）。三条设计判据：
  - **核严格为正、不设截断阈值**（「哪怕关联度极小也有效」）——与注意力 softmax 根本不同，对端永不因距离被丢弃。线格式同时下发 `weight` 与 `logWeight`：`d/λ ≳ 746` 时线性权重在双精度下溢为 0，此时路由一律落到 `logWeight`，**数值零不等于语义截断**。
  - **四种可切换策略**：蝶式（`0`，默认，`O(log N)`；研究已证实其带宽项与环式相同，故大消息 / 层级集群下环式可能反超，`shuttle_strategy` 必须是可调旋钮而非写死）/ 环式（`1`，`O(N)`）/ 树式（`2`）/ Grover 扩散（`3`，`O(1)` 深度，需 2ⁿ 工作寄存器，缺则报 `W-TRK001` 并降级）。默认 `shuttle_strategy=-1`（`auto`）由运行期启发式（对端数 / 载荷 / ancilla）选型，**决策写入运行诊断，不静默**。
  - **相位相对目标层**（`shuttle_phase`，易错点）：`0` = 目标层**入口**（目标把关联当输入看到）、`1` = 目标层**出口**（写回作为返回腿）、`2` = 双侧（默认）。跨窗口传递走**渗透载荷**（经典关联标量）通道——源与目标的功能周期通常并不重叠（`sense@t0` → `relay@t1`），直接读源影子会发现窗口已关闭，而延长影子生命周期又会违反「影子槽随功能周期释放」的不变量。

  **追踪** `track=(obs,…)`：在块的功能周期 `[time, time+cost)` 内四段式 `EXTRACT → COPY → RESTORE → 释放`。
  - **单次软克隆在任意未知态上物理不可能**（测 `Z` 得 `b` 后测量后态为 `|b⟩`，逆推只能得回 `|0⟩`，相位已不可恢复），故采用**三级保真阶梯**而非统一精确承诺：**L0** `qnd`（Pauli 族，Peierls 定理，保真度 = 1，O(1)）、**L1** `shadow`（任意可观测量，经典阴影 Huang-Kueng-Preskill，按 O(3^(k/2)/ε) 采样收敛）、**L2** `relabel`（已坍缩经典值，自由 O(1)）。`track_mode=0`（默认）按可观测量的族自动选级。
  - **非零 `track_mode` 强制覆盖 auto 选级**（`2`→L1、`3`→L2、`1` 对非 Pauli 族报 `E-TRK003`）。强制必须真的改变级别，否则该键就是哑参数；这一点由编译期与运行期两侧测试共同钉住（二者曾漂移过：编译期只把 `mode` 写进线格式却仍下发 auto 级别，而运行期按 `mode` 强制——两端各自「自洽」，接起来是错的，且不会有任何报错）。
  - **影子槽只承载经典标量**（`ShadowEntry` 上 `static_assert` 要求平凡可复制 / 标准布局 / 非多态），结构上不可能是 `QObject`——这正是保 QLT 线性不变量 `E-Q001`（不可克隆）/ `E-Q002`（线性泄漏）的方式。
- **新诊断**：`E-SHU001`–`E-SHU008`（目标 coord 无匹配块 / 无可用关联对端 / `λ ≤ 0` / `phase ∉ {0,1,2}` / `strategy ∉ {−1..3}` / `rounds < 0` / 轮次超 `deadline` / 对端与本块同 coord 同 time）、`E-TRK001`–`E-TRK004`（空 `track` 或 `track_slots < 1` / 追踪源不可解析 / 未知可观测量 / `mode ∉ {0..3}`）、`W-SHU001`（声明 `shuttle` 但本块无 `@track` → 相干路径无影子供给，退化为经典影子混合）、`W-TRK001`（Grover 缺工作寄存器 → 降级为策略 0）。
- **`E-TOP008`（`@layer` 未知键）**：`parseLayerTag` 的 `else if` 链没有 `else` 分支，未知键（如拼错的 `thread2=1`）此前被**静默丢弃**。新机制引入 8 个键后，这个静默失败会变成真实的用户困惑来源，故改为报错。
- **`E-TOP009`（`time` / `thread` / `cost` / `deadline` 取负）**：两个机制需要 `shuttle_strategy=-1`（文档规定的默认值）这类负值，而词法层把负号独立成 `Minus` token、旧的 `parseLayerTag` 一律 `eat(Number)`——于是负号会抛出**无错误码的 `Parser Error` 崩溃**。解析层因此统一接受有符号数值（`dual=true` / `false` 同样从崩溃改为可解析），语义层再用 `E-TOP009` 拒绝无语义的负数。`coord` **允许为负**：coord 是格点**地址**而非计数，L1 距离对负地址同样良定义。
- **编辑器集成**：`@layer(` 后的**上下文补全**只给键名（不再混入关键字与内建函数，`coord=(` 内也不再误弹键名）；悬停到任一键名或枚举取值即显示该键的语义与取值域（如 `shuttle_strategy=3` 悬停到 `3`）；`rvdl.tmLanguage.json` 新增 `@layer` 区域着色（`begin/end` 而非单条 `match`，以正确处理 `coord=(0,0)` 这类嵌套括号；枚举取值与普通数字分色）。键表单点维护于 `hover-docs.ts`，并由 `layer-context.test.ts` / `grammar.test.ts` 守卫其与编译器键表的一致性。
- **测试**：新增 135 项（285 → 420）。`shuttle.test.ts`（73 项：解析形态、关联核、`logWeight`/`weight`、下溢、**逐策略解析深度**、轮次/相位/槽位、全部 `E-SHU*`/`E-TRK*`/`E-TOP008`/`E-TOP009` 诊断、惰性、MIR 下沉、正交性）、`tracking.test.ts`（23 项）、`layer-context.test.ts`（26 项）、`grammar.test.ts`（13 项，含极小 TextMate 扫描器以验证「列表取值的闭合括号不被外层 end 抢走」）。C++ 侧新增两项守卫：`runtime/tests/shuttle_topology_parity.cpp`（TS↔C++ 推导一致性，含**解析深度 ↔ 真实展开**的对账 —— 只照镜子不核对真身，镜子里的错会原样通过）、`runtime/tests/shuttle_tracking_runtime.cpp`（用真实编译器线格式驱动运行期）。
- **示例**：`examples/shuttle_relay.rvdl`（穿梭：三档距离含一个**故意越过下溢墙**的远端节点，展示「线性权重整零但对端仍在计划内」；自检对数空间的可加性与越墙后的严格可分辨性）、`examples/tracking_shadow.rvdl`（追踪：三级阶梯并置 + `track_mode` 强制升降级；自检 L0 非破坏访问器可重复、L2 读出后态仍可用、L1 的统计级熵，并以注释给出「单次软克隆不可能」的编译期形态 —— `E-Q001` 拒绝「读一次再读一次」）。
- **基准**：`runtime/benchmarks/shuttle_plan_benchmark.cpp`（34 项断言：四策略代价表 N=4…1024 的深度与边数、auto 启发式各分支与决策可见性、无截断核的对数可分辨性、整零阈值二分、三级阶梯与采样代价、L1 收敛下界单调性）。**正是这个基准发现了上面的轮次缺陷** —— 文档声称的 O(log N) / O(N) / O(1) 与代码实际展开的轮次数对不上。
- **文档**：新增专题 [docs/rvdl-shuttle-tracking.md](docs/rvdl-shuttle-tracking.md)（关联核与渗透载荷、四策略诚实代价表、auto 启发式、三级保真阶梯与影子槽、诊断码全集、运行期不变量、线格式）；语言手册 §8.1 补两个机制的键表与 §8.1.1，§8.5.1 补 `@layer` 键参考（含相位语义与「强制即强制」），§19.1 补键名（注明**不是关键字**），§19.2 补 `E-TOP008`/`E-TOP009`/Shuttle/Tracking 诊断行。

- **fox 像素级加速器 `runtime/include/utils/Pa.hpp`**：四条机制，**全部保持逐位一致**（硬约束，也是唯一能自动验证的约束）——`DamageTracker`（每 tile 内容哈希不变 ⇒ 该 tile 像素不会变，跳过整段光栅化）、`drop_occluded`（被后继**不透明**实例内缩矩形整块盖住的实例直接删除）、`core_span` + `fill_row`/`blend_row`（圆角矩形内部 `alpha` 恒为 1，跳过 SDF 求值与混合）、`build_shade_rate_map`（GPU 侧 16×16 rate map，`FOX_VRS=1` + `VK_KHR_fragment_shading_rate` 双门控，默认关）。前三条只对 CPU 后端生效（它持有跨帧缓冲），遮挡剔除去在实例构建阶段、**两个后端同时受益**。
  - **开关即参考实现**：`CpuBackend::set_accel(false)` 走逐像素路径（等价于旧的 `FOX_NO_PA`，但无需重编译）；`stats()`/`damage()` 暴露命中率、脏区计数与**分段时间**（`ns_track`/`ns_clear`/`ns_draw`）。没有这三个数，「整屏都在动时为什么没有加速」只能靠猜。
  - **实测**（1000×680 × 120 帧，基准 `pa_pixel_benchmark`）：静态帧 **24.2×**（跳过 99.2% 像素、80.9% 像素走核心区）、局部动画 **17.9×**、整屏动画 **1.10×**（跳过 59.3% 但哈希/分块成本吃掉收益 —— 分段时间证明时间花在剩下的 41% 像素上）；遮挡剔除把 2020 个实例删到 340（83.2%），像素逐字节不变。完整数字与两个被抓到的 bug 见 [docs/gfx-acceleration.md](docs/gfx-acceleration.md) §2.1。
- **`pa_parity_test`（逐字节对照）**：同一帧、同一显示列表，加速开/关各渲一遍，**逐字节比较**。它抓到两个真 bug：(1) 核心区推导多减了一个半径（参考 SDF 的 `alpha == 1` 解是 `|p| ≤ b − 0.5`，半径被约掉，按「内缩 r」写会少画一圈像素）；(2) 「脏 tile 容量不够就重画整个剪辑矩形」的兜底 —— 干净 tile 没被清成背景色，重画半透明边缘等于把已合成像素再混一次，**不可逆**（实测差 1 个色阶，只在「脏 tile 数恰好超容量」的帧出现）。修法是按 tile 总数分配缓冲、取消兜底：不允许存在「保守但不对」的路径。另含 25 组小数坐标随机几何（边界是核心区推导唯一会错的地方）。
- **`fox_metrics_test`（字形基线不变量）**：钉住「每个字形都画在基线下方 4~7 px、每字高度还不一样」这个活了很久的 bug（`bitmap_top = face_->ascent + lgy`：ascent 不属于基线相对量，符号也反了）。人眼对整体 3 px 下移几乎免疫，所以这里只检查可量的东西：无降部字形底边落在基线上（±1 px）、有降部字形确实低于基线、`bitmap_top > 0`、度量稳定（重复询问逐位相同）、**atlas 代际号随首次栅格化单调增长**（damage 跟踪就是靠它发现「显示列表没变、像素却变了」）。同时给 `FontManager` 补 `source_of(FontId)` —— 失败时第一件想知道的事永远是「到底开的哪个文件」（候选列表有十几个路径）。
- **`pa_pixel_benchmark`**：加速器基准，三个场景（静态 / 局部动画 / 整屏动画）+ 遮挡剔除对照 + 分段时间表，并已进 `run_benchmarks` 元目标。
- **`utils/A2a.hpp`（Android 特化层）**：header-only，**不 include Vulkan/fox 头**（输出的是策略值，含 Vulkan 枚举的数值常量，由后端自己翻译 —— 于是它能在没有 Vulkan 的机器上被测）。五块：热状态（只算 SoC zone + 整摄氏度归一）、大小核拓扑（`cpu_capacity` 优先、`cpufreq` 兜底、按比值 > 1.15 分簇、`sched_setaffinity` 用的范围压缩）、TBR 附件策略（`LOAD_OP_CLEAR`/`STORE_OP_DONT_CARE`/`transient_depth`/`memoryless`，要回读时颜色必须 STORE）、质量档与降级顺序、宿主上报（显示旋转 / display timing / GPU 信息）。
  - **降级顺序是设计的一部分**：先降玻璃模糊 → 再降阴影 → **最后才降帧率**（测试钉死 `fps < 60 ⟹ 模糊与阴影已全关`）。帧率是用户最直接感知的东西，不能第一个被牺牲。
  - **只做可判定的事**：需要 JNI/扩展枚举才知道的东西（显示旋转、是否支持 `VK_GOOGLE_display_timing`、GPU 是不是 TBR）一律由宿主 `report_*` 报进来；A2a 不猜（猜错的旋转会画出倒过来的画面，而且不会有任何报错）。GPU 分类是唯一允许的启发式，返回值里带 `confident` 标记 —— 猜错只是策略次优，不会画错。
  - **测试不需要真机**：所有解析都接受可注入的 sysfs 根目录，`a2a_test` 用 fixture 覆盖真实形状（多簇、capacity 抖动、读失败、整摄氏度、**只有电池 zone**、非法旋转角）。"只在真机上可测"等于"永远不被测"。非 Android 上 `available()` 恒 false、`current_quality()` 是中性值（没有证据 ≠ 设备很热）。
- **`.rvdlm` → `.safetensors` / `.gguf` 导出**（`runtime/include/qml/ModelInterop.hpp`，header-only，无第三方依赖）：`save_*`/`load_*` 双向 + `export_rvdlm_to_safetensors<T>`/`export_rvdlm_to_gguf<T>` 直通；命令面 `EXPORT_SAFETENSORS` / `EXPORT_GGUF <in.rvdlm> <out> [张量名] [f32|f64|i32|i64|u8]`（**类型必须显式给**：`.rvdlm` 的头部不记录元素类型，把 f64 的 8 字节按 f32 重组不会报错，只会得到"看起来像张量"的垃圾；应答里回显实际使用的类型）。几条硬约束：写出**逐字节确定**（元数据是有序 vector，导出前排序 —— unordered_map 的迭代顺序会让同一份权重导出两次得到两个文件）；**不能无损映射就报错**（u8/bool → GGUF 直接失败，ggml 表里确实没有该类型；读到未知 dtype / ggml 量化类型同样失败，不假装支持）；`load_*` 做完整边界校验（偏移越界、切片长度与 shape×dtype 不符、alignment 非 2 的幂、非 v3 一律拒绝）。GGUF 里最容易写错的是**维度逆序**（`ne[0]` 是最内层）；`general.alignment` 必须先写且是 2 的幂。
- **三份实现互相对账的验证**（这是重点，不是附带）：生产代码的 `load_*`、C++ 测试内**不调用生产代码**的最小读取器、以及 Python 的 `struct`+`json` 第三份实现（`runtime/tests/model_interop_crosscheck.py`）都要对同一批字节给出相同结论。自己写自己读只能证明自洽 —— `ne[0]` 写反、`data_offsets` 基准写成文件起点、JSON 长度差 8 字节，这些错误都能在闭环里原样通过。也正因为有独立读取器，`{"weight"…}` 多写一个引号这种错误在第一次运行就被抓出来。
- **docs/gfx-acceleration.md §3.1**：导出格式的字段级说明与**已知限制**（数据段按主机字节序，只在小端主机上逐字节正确 —— 写下来而不是绕过去）。
- **fox 取代 Nuklear：运行时判定 + 运行时回退**（`runtime/include/qhal/FoxHost.hpp`，路由在 `runtime/src/ClassicRoute.hpp` + 重写的 `ClassicGui.cpp` / `ClassicGfx.cpp`）。判定**必须**是运行时的：驱动缺失、扩展缺失、窗口创建失败都只在运行时才知道，「编译期二选一」在原理上就不成立。四条失败门（`fox_create` 返回 null / `fox_init_backend_vulkan` 返回 0 / `fox_backend_ready` 为 0 / 运行中提交失败累计到阈值）全部落在 Nuklear 上，且**每条都打印原因** —— 静默回退是这一项最想避免的东西（用户会以为自己在看 fox）。`BackendPolicy` 是**纯状态机**（单向往后、失败后不再重试 fox），`FoxHost` 只依赖 `fox.h`（C ABI，无 Vulkan 类型），窗口是 `WindowFactory` 函数指针接缝 —— 于是整套判定能在**无窗口、无 Vulkan** 的机器上被测：`runtime/tests/fox_host_test.cpp` 用假 fox API 逐帧驱动，覆盖正常路径、三条 init 失败门各自落到 Nuklear 并给出不同原因、运行中失败累计到阈值后**下一帧**切换、以及失败后不再重试。A2a 质量档每 60 帧对一次表并下发 `fox_set_quality()`，**每次变化打印一行**。
- **fox 图元补完：椭圆 / 三角形 / 描边折线 / 双色渐变**（`draw/display_list.hpp` 的 `push_ellipse` / `push_triangle` / `push_path` / `push_gradient`，实例展开在 `draw/instance.cpp`，SDF 在 `backend/shaders.frag` + SPIR-V 同步重生成，C 参考在 `backend/cpu_backend.cpp`，C ABI `fox_path`）。椭圆用精确 SDF `k1*(k1-1)/k2`（矩形近似在小尺寸下明显失真，而且失真方向随半径变化，无法用"看起来差不多"糊过去）；三角形顶点复用折线的顶点缓冲，**绕向无关**（符号取自面积而不是顶点顺序）；折线的圆帽 + 圆角连接让勾选框从「16 个圆点」变成一个 3 点折线（展开后 2 个实例），也从「一串圆点」变成真正的折线。太小 / 退化输入在 **push 时**拒绝（<1px 的椭圆、零面积三角形、零长段跳过）—— 拒绝发生在共享显示列表上，两条后端看到的是同一条命令流，不会在边界上画出不同的像素。
- **PNG 解码 + mipmap 链 + 三线性采样，全部自写**（`runtime/include/ciUd/fox/image/png.{hpp,cpp}`）：inflate（stored / fixed / dynamic 三种块类型 —— 覆盖与否由 `InflateStats` **计数**，不是"我觉得覆盖了"）、CRC32（每个 chunk 都验）+ Adler-32、五种行滤波器、色彩类型 0/2/3/4/6（位深 1..16，含子字节打包、tRNS 键、调色板逐色 alpha、16 位通道取高位）、mip 链（**预乘空间**的 box 平均 —— 直通平均会让半透明边缘在缩小时发黑）、双线性/三线性采样与 `mip_lod`。**不引 stb_image**：fox 的立身之本就是「删掉 Nuklear 后仍然能构建」，再塞一份第三方解码器等于把"自有"变成"又藏了一份"。接线：`Context::create_texture_png` / `create_texture_mipped` / `sample_rgba_lod`（无 mip 的纹理仍走原来的最近邻 —— 既有像素一个都不动），CPU 光栅器在 `PT_IMAGE` 上按实例算一次 LOD，C ABI `fox_texture_create_png`。**Adam7 交错明确报错拒绝**（不静默解错半张图）。
- **`fox_png_test`（66 项）+ 向量生成器**（`runtime/tests/gen_png_vectors.py` → `runtime/tests/fox_png_vectors.hpp`，生成器入仓以便复核；几十 KB 的明文不复制进头文件，C++ 侧用同一公式重新生成）。判据全部可判定：zlib 自己产出的流解回原字节；三种块类型的计数都 > 0；非法流**必须失败在正确的原因上**（断言的是错误文本 —— 只断言"失败"的解码器会把"什么都解不出来"当成通过）；`zlib_store()` 与 `zlib.compress(data, 0)` **逐字节相同**（写入方向也钉住）；LZ77 的 **32768 窗口边界是手写**的固定 Huffman 流（zlib 不会为了测试恰好用上距离 32768，也永远产不出越界的距离 —— 而"距离 32769"在 DEFLATE 里根本表示不出来，真正的越界是"只输出了 1 字节却引用距离 2"；两个手写流都先喂给 Python 的 zlib 验证）。mip / 采样按**行为**断言：texel 中心 = 原值、高 lod = 整图平均、uv 越界 = 钳边缘、小数 lod = 两级双线性的线性插值。
- **`pa_parity_test` 场景 7（mipmap 缩小的像素对拍）**：4x4 不透明四色块缩到 1 像素 —— 带 mip ⇒ 整图平均 `(128,128,128,255)`（缩小不再走样），不带 mip ⇒ 最近邻；两条都要求**加速开/关逐字节相同**。场景 6 另加新图元（椭圆/三角形/折线）的对拍与像素断言（椭圆中心实色、AABB 角落仍是背景、三角形绕向无关、折线中点被描边、大图形不能充当遮挡盖面）。

### Changed
- **fox 正确性对 CSV/文档之外的两处性能陷阱的记录**：脏 tile 必须按「行带 × 跨度」合并后再画（按 tile 画会把同一行拆成 4 段来回跳 x，cache 局部性崩掉，每像素成本是线性扫描的 2.3 倍 —— 于是「跳过 59% 像素」一点都不快）；实例内容哈希按**字**而非按字节（96 字节实例 × 每帧上千个 × 登记+比较两遍，按字节 FNV 的时间足够吃掉 damage 的全部收益）。

### Changed

- **穿梭 auto 轮次的解析式订正（按策略分别解析）**：轮次此前一律套用蝶式公式 `⌈log₂ span⌉`，而环式的实际轮数是 `n`、树式是 `⌈log₂(n+1)⌉`、Grover 是 `1`。轮次既是**执行量**又是 `E-SHU007` 的 **deadline 诊断量**，统一套一个式子会让环式 / 树式的代价被**系统性低估**（`n` 稍大就差一个数量级），而运行期的展开又各按各的算 —— 两端各自「自洽」，接起来是错的，且不会有任何报错。订正后由编译期 `strategyDepth()` 与运行期 `depth_of()` **同式**给出，`E-SHU007` 的代价与实际执行的轮次数逐点相等。注意**不存在普适深度排序**：1024 个对端挤在 `span=2` 时蝶式 1 轮、环式 1024 轮；2 个对端相距 1000 时蝶式 10 轮、环式 2 轮。
- **穿梭 auto 轮次的跨度公式订正**：由 `∏ᵢ(coord_max[i] + 1)` 改为 `∏ᵢ(coord_max[i] − min(0, coord_min[i]) + 1)`（各维取格点真实跨度）。coord 允许为负后，原式对全负地址会得到 `span = 1` → `k = 0`，穿梭**静默地一轮都不做**。全部 coord ≥ 0 且最小值为 0 时本式退化为原式，既有行为不变。计算取自 `blocks` 而非 `shape.coord`（后者保持 `coord_max + 1` 以免破坏线格式兼容）。
- **退化跨度不再静默零轮**：`span ≤ 1`（全部块同 coord）时蝶式解析为 `max(1, ·)` = 1 轮。声明了对端就意味着至少一次交换，解析为 0 轮会让整个机制静默失效。
- **树式展开由 O(N) 层订正为 `⌈log₂(n+1)⌉` 层**：原实现每轮新增一条链路，层数等于对端数（与文档的 O(log N) 声称不符），且末几轮会反复指向同一个对端。现按 `2^d−1 ≥ n` 分层，**最后一层强制覆盖全体剩余对端** —— 若调用方给的轮次少于所需层数，未覆盖的对端会**从计划里消失**，违反「对端永不因代价被丢弃」。
- **环式展开的轮次数订正为 `n`**（原为 `n`，但循环条件写成 `hop + 1 < n + 1 && hop < n` 的等价重复形式），并补上此前缺失的 `edge.source` 字段（链式读取该字段的调用方会拿到空串）。
- **Grover → 蝶式降级改为重规划**：`W-TRK001` 触发时此前只把策略标签改成 `0`，留下「标签写着蝶式、计划里只有 1 轮」的静默不一致 —— 1 轮蝶式只翻 `coord` 第 0 位，绝大多数格点收不到渗透。现按蝶式**重新规划**并在日志中打印实际轮次数。
- **下溢墙的记载精确化**：`exp(−x)` 的双精度**整零**阈值实测为 `x ≈ 745.13`（二分夹逼），与文档记的 «≈746» 一致；但需与 `ln(DBL_MAX) ≈ 709.78` 区分 —— 后者是**正规数**下界，越过它只是进入次正规区（`w ≈ 5.6e-309` 仍非零），两者相差 35，混用会给出错误的「下溢」判断。

### Fixed
- **`push_path` 的 `cmd.rect` 恒为 `{0,0,0,0}`**（HEAD 里的注释写着 "Bounds computed in backend"，但**后端从未重算**）：`build_instances()` 会丢掉所有非正尺寸矩形的命令，于是**每一条 path 命令都在到达光栅器之前就被扔掉了** —— 描边折线"存在"了很久、勾选框却一直在用 16 个圆点糊一个对勾，根因就在这一行。修法是在 push 时算 AABB（含描边半宽与圆帽外扩），因为 damage 跟踪 / 裁剪 / 遮挡剔除全部以 `cmd.rect` 为输入 —— 它不能是"以后再说"的字段。`pa_parity_test` 场景 6 的"段中点必须是描边色"就是钉这条的。
- **`Pa.hpp::DamageTracker::dirty_tiles_for` 交出的矩形只夹了 x、没夹 y0**：脏 tile 被当成"实例要遍历的矩形"交给光栅器时，实例**顶边上方的半格**没有被排除，于是那些像素也会被着色。有覆盖率测试的图元（圆角矩形 / 椭圆 / 三角形 / 文字 / 玻璃）在形状外写不进去，所以一直没暴露；**图片没有覆盖率测试**（矩形内逐像素采样纹理），会向上渗出最多 15 行。旧场景的图标顶边恰好是透明像素（写了等于没写），换成不透明纹理后当场被场景 7 的逐字节对拍抓住 —— 修法是把 y0 与 x 对称地夹到请求矩形。这是"加速器宣称不改变一个像素"这条硬约束第三次抓到真缺陷（前两次：核心区多减一个半径；容量兜底重画干净 tile）。
- **`vk_backend.cpp` 使用了 MSVC 专有的 `fopen_s`**（annex-K，libc++ / glibc 里不存在）：改为可移植的 `std::fopen`。此前 Linux / Android 构建直接编译失败。
- **`fox.h` 缺 `<stddef.h>`**：`size_t` 依赖 `<stdint.h>` 的间接包含，换标准库或换包含顺序就编不过 —— C 头文件必须自己包含自己用到的类型。
- **`fox.cpp` 缺 `<cstring>`**：`memcpy` / `strlen` 的声明不该靠传递包含。
- **`shaders.frag`：`sdPath` 调用少传一个实参**（4 个 vs 5 参签名，缺 `join` 风格）。glslc 直接拒绝编译；顺带修掉 SPIR-V 生成脚本的两个缺陷（缩进按**字**而不是按行重复、CRLF 被写两次），现在 bash 与 PowerShell 两个孪生脚本产出的头文件**逐字节一致**，`spirv-val` 干净，与旧头文件的差异恰好只有 header 注释新增的那一行。
- **独立 CPU 构建（`-Wall -Wextra`）重新达到零警告**：`widget/primitives.cpp` 的 `text_area` 占位参数语义与 `text_field` 对齐（`!buffer || capacity <= 0` 时画占位文本而不是空转），`slider` 里一个从不被读取的 `bool changed` 删除。

## [0.9.3] (vsx 0.3.3)

### Added

- **`@[coherence]` 多通道退相干建模**：`computeNoise()` 原只能返回单一噪声通道，被迫把不同物理来源折叠成一个强度。现返回**通道列表**，由 `emitNoiseIfNeeded()` 依次注入：T1（能量弛豫）→ 振幅阻尼（channel 2），T2 中的纯退相部分 → 相位阻尼（channel 1），按严格分解 `1/T_φ = 1/T2 − 1/(2·T1)` 各自用自身时间常数定强度。实测 `@[coherence(100,50)]` 产出 `i32 2 / 0.010000` 与 `i32 1 / 0.015000`（= 1/50 − 1/200），与物理公式吻合。`@[noise]` 路径仍为单通道，行为不变。
- **morph 宏的编辑器支持**：`hover-docs.ts` 新增 `morph` 中英双语悬停文档（作用 / 语法 / 元变量七种类 / 三条硬约束 / 示例）；`syntaxes/rvdl.tmLanguage.json` 新增 `morph` 关键字高亮与宏元变量 `$name`、字符串化 `#$name` 的高亮规则；`server.ts` 关键字表补入 `morph`（补全与语义高亮）。
- **编辑器高亮细分（各有各的颜色）**：语义 token legend 由 7 类扩为 13 类（`keyword` / `type` / `class` / `function` / `macro` / `method` / `decorator` / `constant` / `enum` / `number` / `string` / `operator` / `variable`），并按类别着色：控制流与声明关键字、经典类型 vs 量子类型（`Qubit` 等走 `class`）、内建函数、量子门（`h`/`x`/`cnot`… 走 `method`）、`morph` 宏（`macro`）、注解标签 `@layer`/`@[gate]`（`decorator`）、`flavor`（`enum`）、`true`/`false`/`null`（`constant`）。量子门在词法层已不是关键字，故补按名称识别，使调用点也能正确着色。
- **智能感知补全（前缀过滤 + 相关度排序）**：补全此前一次性返回全部条目、无过滤，依赖客户端兜底。现按光标前标识符前缀（输入 `all` 即得 `alloc` 等）在服务端过滤，并按「精确匹配 > 前缀匹配 > 子串匹配」排序（同档按长度短者优先）；新增**量子门**类别补全，并为函数/门补 `insertText`（自动带 `(`），注解标签改用 `Property` 图标以便与关键字区分；无前缀时限量 200 条避免弹出卡顿。
- **Linux/WSL 侧交叉验证**：本轮 C++ 改动（qchain 密码学 KEM 工厂与开关、签名工厂去静默降级、时空加密改用 ML-KEM-768、区间约束的仿射与变量-变量传播、端点预测器告警、内核 idle 线程）已在 WSL (Ubuntu, clang++ 21) 下以 `-fsyntax-only` 全部通过。依赖 Kokkos 的部分（`utils/Ga.hpp` 等）未能在 Linux 侧验证——WSL 复用的是 Windows 版 Kokkos（其 `CUDAToolkit_ROOT` 为 Windows 路径），需在 WSL 内独立安装 Linux 版 Kokkos 后方可完整构建。
- **测试与文档回填**：新增 15 项测试——SSA 支配树（4 项，含「分支内定义、汇合块使用」这一文本顺序检查会漏报的用例，以及 entry 定义 / 循环回边 / 函数参数三类**不应误报**的用例）、`E-TOP007` 触发与「维度差为 1 仍属合法投影」（3 项）、QRC `qrc_new` 三参形态与 `qrc_train_ex` 签名/类型检查（8 项）。全量测试 255 → 270。语言手册 §12.10 补 `qrc_new(qubits, layers, out_dim)` 第三参与 `qrc_train_ex`（真实训练数据；`inputs`/`targets` 为扁平 `double` 缓冲，类型可用 `cap<double>` 或 `arr<double, N>`），并明确 `qrc_train` 跑合成正弦仅属自检、非通用训练入口；§19.2 错误表补 `E-TOP007`，并订正 `E-TOP002`（维度不一致检查此前已放宽为 no-op）。
- **morph 态射宏系统（全局 · 卫生即规范不变性 · 宏即用户定义的内建）**：新增元编程机制 `morph`，在编译期把「语法模式」变换为「代码」，是对 `#define` 文本宏的范式替代，三条硬约束：① **全局**——仅顶层声明，与内建函数同命名空间，不允许局部宏，展开不依赖局部作用域（与程序组合可交换）；② **卫生 = 规范不变性**——模板引入的绑定（`let` / `fn` 名 / 参数 / 类型声明）在每次展开时 α-重命名取新原子，杜绝 `#define SWAP` 式变量捕获；③ **不用 `#define`**——展开发生在词法层、parse 之前，产物重新进入类型 / 借用 / 拓扑检查。一期元变量：`expr` / `stmt` / `items` / `type` / `ident` / `literal` / `tt`；支持 `$(...)*` / `$(...)+` 重复生成、`#$name` 字符串化、限深递归（上限 64）；调用统一 `name!(...)`。实现落在 `server/src/morph.ts`（token 树构建 → 模式/模板解析 → 匹配/实例化 → 不动点展开），配合 `lexer.ts`（新增 `morph` 关键字、`$` / `#` token、`Lexer.fromTokens`）与 `parser.ts`（`parse()` 词法层接入，零侵入 AST）。`morph.test.ts` 覆盖卫生 / 全局 / 重复 / 种类报错 / 递归 / 字符串化共 12 项。

### Fixed

- **少参调用 `qrc_train_ex` / `qrc_new` 导致 IR 生成崩溃**：新加的 codegen 直接索引 `arguments[3..7]`，实参不足时抛 `TypeError: Cannot read properties of undefined` 中断整个编译（语义层虽会另报 `Signature Error`，但 IR 层不应崩溃）。现对缺失实参填占位值继续生成，并补 `assert.doesNotThrow` 防回归测试。

## [0.9.2] (vsx 0.3.2)

### Added

- **侧边栏编译目标新增 `android`**：`client/src/ravendrelSidebar.ts` 的 `COMPILE_TARGETS` 增加 `android`（`aarch64-linux-android`，带 NDK 提示 tooltip、专用 `device-mobile` 图标，并标注各目标的 LLVM triple）；`extension.ts` 的 `ravendrel.setCompileTarget` 增加目标合法性校验（避免命令面板调用把 `undefined` 写进 `workspaceState`）；选中 Android 执行 AOT 编译时，控制台会提示所需 NDK 交叉编译器与 `RVDL_ANDROID_CLANGXX` / `RVDL_ANDROID_SYSROOT` / `RVDL_ANDROID_RT_DIR` 环境变量，并指引用 `rvdl build apk` 打包 APK。
- **一键构建 Android APK（`ravendrel.buildApk`）**：侧边栏「操作」新增 **Build APK (Android)**（单击直接出 debug 包；从命令面板调用时弹出 Debug/Release 选择），编辑器标题栏也加入该按钮。服务端按 `RVDL_ROOT` → 工作区根 → cwd/自身向上 定位 `scripts/build-apk.sh` 并 `spawn bash` 执行（与 CLI `rvdl build apk` 同一编排），stdout/stderr 实时回显到 Ravendrel Console，耗时与结果写入性能面板。Windows 上优先使用 Git Bash（`%LOCALAPPDATA%\Microsoft\WindowsApps\bash.exe` 是 WSL 启动器，无法解析 `D:\...` 形式的脚本路径），可用 `RVDL_BASH` 覆盖；未找到脚本或缺少 bash 时给出明确指引。

### Fixed

- **AOT 编译目标不生效（侧边栏编译目标形同虚设）**：`server.ts` 发送的 `CMD_AOT_COMPILE` 载荷多写了一个 `compile` 前缀（`compile <arch> <name>`），而后端按 `<arch> <mode> <name>` 解析，于是 `arch` 被当成 `"compile"`、`mode` 被当成目标名——无论侧边栏选 x32/x64/arm64/android，实际都按宿主默认 triple 编译。现在按 `<arch> -e <name>` 发送。
- **Windows 宿主上的 Android 目标补齐**：`Compiler.hpp` 的 `_WIN32` 分支此前缺少 `aarch64-linux-android` triple 映射（会静默回退到宿主 triple），且输出后缀名硬套 `.dll`/`.exe`；现 Android 目标在 Windows 宿主上同样产出 ELF（`-m` → `.so`，`-e` 无后缀），并在找不到 NDK 链接器时给出可操作提示。
- **CUDA 设备通道下 `rvdl_qrc_*` 未声明（曾阻塞 Windows 运行时构建）**：`SandboxJIT.hpp` 无条件绑定 QRC 符号，但其声明只在 `qml/QrcAbi.hpp` 里、且依赖包含顺序（`runtime_api.cpp` 先包含 `MMI.hpp`）。现在 `SandboxJIT.hpp` 自行包含该 ABI 头，头文件自洽。
- **Windows 资源编译失败（`build.bat` 固定 RC 编译器）**：clang-cl 工具链下 CMake 会生成 `cmake_llvm_rc` 包装规则；若它自动探测到 Windows SDK 的 `rc.exe`，会把 clang 风格参数喂给 rc.exe 导致失败。`build.bat` 现显式指定 `-DCMAKE_RC_COMPILER=<LLVM>/llvm-rc.exe`。

## [0.9.1] (vsx 0.3.1)

### Added

- **量子非欧几里德曲面体几何原语**：语言层新增 `geodesic_distance(QObject, QObject)` / `inversion(double)` / `hyperbolic_metric(double)` / `hyperbolic_distance(double, double)` 四个内建；新增 C ABI `runtime/include/qml/GeodesicAbi.hpp`——`rvdl_geodesic_distance` 用 SWAP-test 重叠给出 Fubini-Study 测地线距离 `arccos|⟨a|b⟩|`（复用 `rvdl_qattention`），`rvdl_inversion` 实现 T-对偶反转 `R → 1/R`，`rvdl_hyperbolic_metric` / `rvdl_hyperbolic_distance` 给出 Poincaré 球度规 `4/(1-|x|²)²` 与双曲距离。`server/src/mir.ts` 把 `geodesic_distance` 纳入 `QOBJ_BORROW_FNS`（借用 QObject 而非消费，避免 QLT 误报 E-Q001）。
- **量子通道逆因果容量（回程能力）**：语言层新增 7 个内建——`retrocausal_imax(kind, p)` / `retrocausal_idoe(kind, p)` / `retrocausal_q_capacity(kind, p)` / `retrocausal_c_capacity(kind, p)` / `retrocausal_q_one_shot(kind, p, eps)` / `retrocausal_gain(kind, p)` / `retrocausal_deformed(kind, p, x)`；新增 C ABI `qml/RetrocausalAbi.hpp` 与内核 `runtime/include/spacetime/RetrocausalCapacity.hpp`：Choi 矩阵 `choi_matrix`、max-information / Doeblin information（`retrocausal_info`）、正则化 Doeblin `idoe_regularized`、前向 Holevo 基准 `forward_classical_capacity`、渐近与单次 q/c 容量、q-变形容量 `q_retrocausal_capacity`、后选择放大协议 `amplified_teleportation`、容量流 `retrocausal_flow` / `retrocausal_flow_perturbed`。预置信道 `kind`：`0`=去极化、`1`=退相、`2`=比特翻转、`3`=振幅阻尼、`4`=幺正 Hadamard、`5`=比特-相位翻转。
- **快子 KK 双空间嘈杂 CTC 通信（n 个 DD 维度）**：语言层新增 `retrocausal_ctc_q_capacity(n, theta, mu2, lambda)` / `retrocausal_ctc_c_capacity(...)` / `retrocausal_ctc_gain(...)` / `retrocausal_ctc_dephasing(theta)`；新增 C ABI `qml/RetrocausalCTCAbi.hpp` 与内核 `spacetime/RetrocausalCTC.hpp`——`KaluzaKleinND`（n 个额外维 S¹ 的 KK 动量、模式质量平方、快子质量平方与模式不对称）、`TachyonCTCNoise`（几何 → 噪声映射：退相 `asym/(1+asym)`、去极化 `n/(n+1)`、振幅阻尼 `μ²/(μ²+λ)`）、Kraus 串行组合 `tachyon_ctc_channel` 与 `MultiTopologyRetrocausal`（逐维度容量与容量-维度曲线）。`@layer(coord=(c₀,…,c_{n−1}))` 的 coord 维度即额外维数 n。
- **`@layer(..., dual=1)` 对偶标记与三类新拓扑边**：`dual` 标记 T-对偶反转空间；拓扑推导新增 `projection`（coord 维度不同 → 曲面体投影）、`inversion`（同 coord + `dual=1`）、`crossing`（同 coord 异 thread）三类边（`ast.ts` 的 `TopologyEdge`）。
- **拓扑调度入口（编译期 → 运行时）**：含 `@layer` 的模块额外生成调度表常量 `@rvdl_topology_json` 与入口 `define i32 @rvdl_topology_entry()`，后者调用新运行时接口 `rvdl_runtime_run_topology(i8*)`——按 `time` 分层，同层按 coord 的 L1 码距并行调度（虚拟线程 → 残差并发 → 码距逻辑 qubit 布局），阈值由环境变量 `RVDL_TOPOLOGY_MIN_DISTANCE` 调整。
- **新拓扑诊断**：`E-TOP005`（叠加链坐标空槽 gap）、`E-TOP006`（调用传播延迟超出 `deadline`）；新增调用传播边 `TopologyCallEdge`（`startAt` / `deltaT`），子函数锚点 = 父锚点 + 累计延迟（`cost` 之和）。
- **SoftLogic / 神经 / QMS 原语 ABI 化**：新增 `runtime/include/qml/SoftLogicAbi.hpp`，把 `rvdl_surrogate` / `rvdl_tanh_quantize` / `rvdl_lif_step` / `rvdl_mellowmax2` / `rvdl_logsumexp2` / `rvdl_boltzmann2` / `rvdl_tnorm_luk|prod|godel` / `rvdl_polymer_weight` / `rvdl_polymer_mix_bound` / `rvdl_qms_gap` / `rvdl_mix_bound` / `rvdl_qms_conc` 统一为 `extern "C"` 导出。
- **SandboxJIT 无条件符号绑定扩展**：geodesic / retrocausal / CTC / 软逻辑 / QMS 原语全部无条件绑定，`.rvdli` 沙箱内可直接执行这些推理原语。
- **文档补齐**：语言手册新增 §12.12（量子通道逆因果容量）、§12.13（快子 KK 双空间嘈杂 CTC 通信）、§12.14（非欧几里德曲面体几何），§8.1 补充 `dual` 与三类新拓扑边、拓扑调度说明，§19.1 关键字表补入全部 `retrocausal_*` / `retrocausal_ctc_*`；悬停文档 `hover-docs.ts` 补入 `retrocausal_*` / `retrocausal_ctc_*` 中英双语说明。

### Changed

- **`E-TOP002` 放宽**：坐标维度一致性检查改为 no-op——曲面体允许不同块使用不同 coord 维度，改为推导 `projection` 边（`validateCoordDims` 不再报错）。
- **SoftLogic / QMS 原语定义位置迁移**：由 `qhal/JIT.hpp` 内的 inline 定义迁移到 `qml/SoftLogicAbi.hpp` 的「`extern "C"` 声明 + `#if RVDL_RT_BUILD` 单点定义」，并由 `runtime/src/runtime_api.cpp` 统一 include。
- **MIR body 增加调度元数据**：`MirBody` 新增 `layer?: {time?, thread, coord}`，随 body JSON 序列化下发（供 MIR 下沉器与沙箱使用）。
- **VSIX 单包双平台 + 双平台开箱即用**：`bin/` 按平台分子目录（`win32-x64/`、`linux-x64/`），一个 vsix 同时携带 Windows 与 Linux 运行时，安装后按 `process.platform` 自动选择（`server.ts` 的 `resolveRuntime()`，并在 Linux/macOS 上按需 `chmod 0755`，因 zip 不保留可执行位）。两条运行时链路都做到**自包含**：
  - **Windows**：随包携带 `libomp.dll`、`vulkan-1.dll` 与 MSVC 运行库（`msvcp140`/`vcruntime140*`，app-local 部署），不再要求用户安装 VC++ 运行库或 Vulkan 运行时。
  - **Linux**：LLVM 改为**静态链接**（自建精简静态 LLVM，仅 X86/NVPTX/AArch64 后端），Kokkos 亦静态（PIC）并入 `librvdl_rt.so`；随包携带 `libvulkan.so.1` / `libglfw.so.3` / `libomp.so.5`。不再要求目标机安装 LLVM / Vulkan SDK / GLFW / OpenMP 运行时。
- **Linux 旧发行版兼容（新增 `runtime/src/glibc_compat.cpp`）**：在新发行版（如 Ubuntu 26.04 / glibc 2.43）上链接会把 `fmod`/`fmodf`/`acosf`/`sqrtf`/`__isoc23_strto*` 绑定到 `GLIBC_2.38/2.43` 的新符号版本，导致旧发行版报 `version 'GLIBC_2.38' not found` 而**无法加载**。新增垫片在本 DSO 内就地定义这些新名字（内部经 `.symver` 转调 `@GLIBC_2.2.5` 旧实现；`arc4random` 家族用 `getrandom(2)`/`/dev/urandom` 自行实现），使 `librvdl_rt.so` / `runtime` 的符号需求上限从 `GLIBC_2.43` 降到 `GLIBC_2.34`；同时以 `-static-libstdc++` 消除 `GLIBCXX_3.4.3x` 依赖（libgcc 保持动态，避免静态展开器引入 `_dl_find_object`）。

### Fixed

- **LLVM ORC JIT 符号解析失败**：`rvdl_surrogate` / `rvdl_polymer_*` / `rvdl_qms_*` 等原语此前以非 inline 定义散落在被多处 include 的 `JIT.hpp` 中，导致 JIT 报 `Symbols not found`；改为单点定义后修复。
## [0.9.0] (vsx 0.3.0)

### Added

- **Android 平台支持**：新增 `rvdl compile android`（等价 `arm64-android`）AOT 编译目标——`Compiler.hpp` 增加 `aarch64-linux-android` target triple，链接阶段优先用 NDK 交叉编译器（`aarch64-linux-android21/24/26-clang++`，经 `RVDL_ANDROID_CLANGXX` 环境变量或 PATH 定位）；新增 `rvdl build apk <file.rvdl> [--release]` 命令（`cli.ts` 定位 `scripts/build-apk.sh` 并 `spawnSync` 一键构建）。
- **轻量 Android 运行时**：新增 `runtime/android/`——`runtime_android.cpp`（QVM 核心 + JNI 桥 `com.ravendrel.RavendrelRuntime`，无 LLVM JIT，Qubit 层 / QObject 层 / 对象创建 / JNI 自检 `nativeBellMeasure`）、`CMakeLists.txt`（aarch64 交叉编译）、`apk/`（Gradle 工程）；`.rvdl` 在桌面端 `rvdl compile android` 预编译为 `librvdl_main.so`（aarch64-linux-android），与轻量运行时 `librvdl_rt.so` 一并打入 APK，由 Java JNI 壳加载调用 `rvdl_main`；`scripts/build-apk.sh` 一键编排「交叉编译运行时 → AOT 编译 → 收集 .so → Gradle 打包」。
- **Android bionic 适配**：`Ga.hpp` 的线程亲和性设置跳过 Android（bionic 无 `pthread_setaffinity_np`，glibc 特有）。
- **SandboxJIT TQNF 符号无条件绑定**：`rvdl_qgate_*` / `rvdl_qexpect_z` / `rvdl_qmeasure` / `rvdl_qobj_num_qubits` / `rvdl_qattention` / `rvdl_qstate_*` / `rvdl_dla_dim` / `rvdl_shannon*` / `rvdl_encode_*` 无条件绑定，使 `.rvdli` 的 export 函数可在 MMI 沙箱内执行量子推理（与 lattice / cgfx 同等对待）。

### Fixed

- **QVM 后端 qubit 回收深层 bug**：修复 qubit id 分配 / 复用与 `num_qubits` 收缩的深层缺陷——全局分配器 `next_available_qubit` 由「纯单调计数 + `rvdl_release_object` 盲目 `fetch_sub`」改为「单调高水位 + 空闲 id 复用栈」（`rvdl_alloc_qubit_id` / `rvdl_free_qubit_id`，互斥保护 + 去重）；`release_qubit` 在回收前复位到 |0⟩（测量坍缩 + 条件翻转）、置 `is_qubit_allocated` 后剥离尾部已释放 qubit 收缩 `num_qubits`（Dense `resize(2^n)` / MPS `drop_qubits` / Polyhedral 逐层剥离 `root->left`）；`IQuantumBackend` 新增 `allocate_qubit(id)` 接口；`rvdl_release_object` 消除对 `QOBJ_DATA_SHARED` 对象的双重 release（先销毁 `shared_ptr` 由其析构统一 release，再回收 id）。
- **daemon 多连接与帧解析修复**：`runtime/src/main.cpp` 的 socket 守护进程从单连接改为 `select` 多连接调度，帧头 / payload 分段读取（处理 TCP 粘包 / 半包），新增单帧 64MB 防御上限。
- **`RVDL_NO_GPU` 环境变量**：`RVDL_NO_GPU=1/true/yes` 强制禁用 GPU 探测（诊断 / 无 GPU 环境 / 规避驱动问题），`0/false/no/off` 或未设置时正常探测。
- **IR 全局变量对齐修复**：`server/src/ir.ts` 全局变量的 `align` 由硬编码 `4` 改为 `typeAlign(llvmType)`，修复非 4 字节对齐类型（如 `double` / `uint64`）的 ABI 对齐错误。
- **拓扑收集修复**：`server/src/topology.ts` 对含 `export` 函数的 MMI 模块跳过拓扑收集与 coord 占用检查，避免组件函数之间 coord 相同的「假冲突」（如 `perceive` 与 `chaos_drift` 均 coord=0）。

## [0.8.0] (vsx 0.2.2)

### Added

- **量子语言迁移框架（`rvdl migrate`）**：新增 `server/src/migrate/` 目录——统一电路中间表示 `circuit-ir.ts`、门归一/分解 `gates.ts`（qelib1 标准门库 → rvdl 原生门）、`.rvdl` 发射器 `emitter.ts`，以及五个源语言前端：`qasm2.ts`（OpenQASM 2.0）、`qasm3.ts`（OpenQASM 3.0，含 `qubit[N]`/`for` 循环展开/自定义 gate）、`qsharp.ts`（Q#，含 `use`/`using`/`M`/`Reset`）、`quil.ts`（Rigetti Quil）、`silq.ts`（ETH Silq）；CLI 新增 `rvdl migrate <file> --from <lang> [-o out.rvdl]` 命令（支持按内容自动探测源语言）。迁移产物为可读、可编辑、可编译的 `.rvdl` 源码，且通过现有编译管线端到端校验。
- **Qubit 层原生门补充**：新增 `y` / `z` / `s` / `t` / `rx` / `ry` 六个原生门（对齐 OpenQASM `qelib1.inc`），贯穿 lexer/semantic/ir/gate-synth/mir/hover-docs 与 C++ 运行时（`IQuantumBackend` 默认实现 + `JIT`/`SandboxJIT`/`MirModuleBuilder` 的 QIR 内建导出）；`y`/`z` 自逆、`rx`/`ry`/`rz` 取负角、`s`/`t` 展开为 `rz` 取负角，纳入可逆编织（`@[undo]`/`@[steer]`）取逆规则。
- **迁移手册**：`docs/rvdl-migration-manual.md`（门映射表、`rvdl migrate` 用法、各语言迁移示例、限制与降级说明）。
- **`reset` 测后条件翻转语义**：含「中间 reset」（测后复用 qubit）的电路自动切换到 QObject 层——`QuantumRegister` + `qgate_h/x/rz/cnot` + `qmeasure`（部分坍缩）+ 条件翻转（`int32 _r = qmeasure(q, i); if (_r == 1) { qgate_x(q, i); }`），与 Born 规则坍缩语义一致；新增 `gates.ts` 的 `lowerToBasic`（完整门集 → h/x/rz/cnot 四门分解，覆盖 y/z/s/t/rx/ry/swap/cz/cy/crz）。
- **经典控制流符号化**：OpenQASM 3 经典变量（`int[32]`→`int32`、`bool`→`int32`、`float[64]`→`double`）、经典赋值、`while` 循环、复杂布尔 `if` 条件（`&&`/`||`/`!`/比较/else）符号化转译为 rvdl 对应结构（`CircuitIR` 扩展 `ClassicalVar` / `while` / `classicalAssign` / 复杂条件 `IfOp`）。
- **VS Code 右键菜单迁移**：`package.json` 注册 `ravendrel.migrateScript` 命令 + `editor/context`/`explorer/context` 右键菜单（`.qasm`/`.qs`/`.quil`/`.slq`）；`extension.ts` 命令转发、`server.ts` 新增 `ravendrel/migrateCode` 通知（读文件 → 自动探测 → 迁移 → 写同名 `.rvdl`，结果回显 Ravendrel Console）。
- **VS Code 侧边栏（仿 Flutter DevTools）**：新增 `client/src/ravendrelSidebar.ts` 与 Activity Bar 容器 `ravendrel-sidebar`，四个视图——**编译目标**（TreeView，x32/x64/arm64 切换，workspaceState 持久化 + 编译透传）、**操作**（TreeView，运行/编译/构建/迁移快捷按钮）、**性能监测**（WebviewView，编译/执行耗时历史折线图，`globalState` 跨会话持久化 + `view/title` 清空按钮）、**量子对象与比特**（WebviewView，daemon `GET_SNAPSHOT` 实时快照：多 qubit **约化密度矩阵 Bloch 球** 3D 鼠标旋转 + 态矢量概率柱状图 + 量子对象 + 测量历史）。
- **执行改走 daemon 模式**：`server.ts` 执行由前台 `spawn` 改为连接 `localhost:50052` 发 `COMPILE`/`EXECUTE`/`AOT_COMPILE` 帧（新增 `pingDaemon`/`ensureDaemon`/`daemonRequest`，未运行则自动 `spawn runtime --daemon`）；用户代码量子操作作用在 daemon 共享 QVM，侧边栏与执行共享快照，并回传 `ravendrel/performance` 性能通知。

## [0.7.0] (vsx 0.2.1)

### Added

- **QRC 量子储备池**：`qrc_new(qubits, layers)` / `qrc_train(res, epochs, lr)` / `qrc_probe(res, data)` / `qrc_predict(res, data)` / `qrc_release(res)` 五个语言层内置函数 + `QReservoir` 类型，对应 C ABI `qml/QrcAbi.hpp`；合成正弦时序训练线性读出，验证「无贫瘠高原」的量子储备计算（`qrc_probe` 测态编码输入 → 返回 ⟨Z⟩ 特征、`qrc_predict` 返回读出预测）。
- **TQNF 拓扑量子神经场（语言层）**：QObject 层量子门 `qgate_h` / `qgate_x` / `qgate_rz` / `qgate_cnot`（直接作用于 QObject 内部 qubit，借用不消费）、跨对象受控门 `qgate_cnot_pair(a, ia, b, ib)`（供 POVM 弱测量）、非破坏期望读取 `qexpect_z`、部分坍缩测量 `qmeasure(qobj, index)`、qubit 数查询 `qobj_num_qubits`；量子态量度 `qstate_entropy`（von Neumann 熵）/ `qstate_fidelity`（保真度）、SWAP test 量子注意力 `qattention`（态重叠度 |⟨ψ_q|ψ_k⟩|²）、测量熵 `shannon4` / `shannon8`、可训练性诊断 `dla_dim`（动力学李代数维数）。
- **POVM 正定算子值测量**：`qgate_cnot_pair` 跨对象受控门（主 qubit 控制辅助 qubit）实现弱测量，主态不被完全坍缩。
- **多 qubit 变分 ansatz**：`qobj_num_qubits` 查询 QObject 的 qubit 数，供 ansatz 判定态维度对齐（感知态维度）。
- **真坍缩语义**：`qmeasure(qobj, i)` / `QObject.measure()` 实现**部分坍缩**（测第 i 个 qubit，其余 qubit 仍存活），与 C++ 侧 `rvdl_qmeasure` 对齐；`QOBJ_MEASURE_FNS` 用 Ref 借用（不消费整个 QObject），修复同一 QObject 测多个 qubit / 测后 move 返回被误判 E-Q001。
- **量子线性类型（QLT）补全 + 隐式 Drop**：`QuantumRegister` / `DiracState` / `BellState` 纳入 `LINEAR_TYPES`（此前遗漏了，重复 move 现在报 E-Q001）；具体量子类型在函数末尾**隐式 Drop**（C++ 侧析构自动 measure+release_qubit，IR 层 `emitScopeCleanup` 生成 `rvdl_release_object`），不再报 E-Q002 泄漏，仅 `Qubit` / `QObject` 裸类型强制显式消费。
- **可选 GPU（CUDA dlopen 延迟加载）**：运行时不再硬链 `libcuda.so.1` / `libcudart.so.*`，启动时 `dlopen`/`LoadLibrary` 探测 NVIDIA 驱动（`cuInit` + `cuDeviceGetCount`），有 GPU 走 Kokkos CUDA 后端、无 GPU 回退 CPU（OpenMP/Serial）；`Ga.hpp` 按 `rvdl::gpu_backend_available()` 运行时派发，Kokkos 以 `--allow-shlib-undefined` + 去掉 cuda_driver/cudart 硬链重编，lazy Kokkos（`libkokkoscore.so` / `libkokkoscontainers.so`）随 VSIX / 发行包分发。
- **`QObject.measure()` 消费语义修复**：`MemberExpression` 的 `measure()` 方法调用显式 `Consume` object，修复 QObject 裸类型 `.measure()` 被误判未消费（E-Q002）。

### Changed

- `.vscodeignore` 移除 `vendor/**`：VSIX 打包不再过滤 `vendor/`（GLFW / Nuklear / pinyin / stb 第三方库随包分发）；`.gitignore` 追加 `!vendor/**/*.{dll,lib,a}` 例外，避免预编译库被通配符忽略。
- 量子线性类型检查：具体量子类型（`QuantumRegister`/`DiracState`/`BellState`）函数末尾由「报 E-Q002 泄漏」改为「隐式 Drop」，`Qubit`/`QObject` 裸类型保持严格 no-cloning 约束。

## [0.6.6] (vsx 0.2.0)

### Added

- **一行多变量定义**：`int32 a = 1, b = 2, c = 3;` 单行声明多个同类型变量（新增 `VariableDeclarationList` AST 节点）；顶层显式类型多定义降为多个全局变量（`@a`/`@b`/`@c`）、`let`/`auto` 多定义作为脚本模式局部变量；语义 / IR / MIR / VCGen / 拓扑 / 文档大纲 / 定义跳转全链路支持，后声明的变量可引用先声明的变量（`int32 a = 1, b = a + 1;`）。
- **悬停提示详细文档（中英双语）**：新增 `server/src/hover-docs.ts`，为全部关键字、类型、内置函数、注解标签（`@layer` / `@[gate]` / `@[coherence]` 等）编写「作用 / 语法 / 参数 / 返回值 / 示例」说明；根据开发者系统语言（LSP Initialize 的 `locale`）自动切换中文 / 英文，类似 C++ 悬停 `std::cout` 的详细信息。

### Changed

- 悬停提示从泛化的「关键字 `x` / 标识符 `x`」升级为按「类型 → 内置函数 → 关键字」优先级查询的详细 Markdown 文档；门函数（`h`/`x`/`rz`/`cnot` 等词法上属标识符）与 `void` 等类型名也能正确展示说明。

## [0.6.5] (vsx 0.1.8)

### Added

- **读寄存器内建（x86 架构边界）**：`read_cr0`/`read_cr2`/`read_cr3`/`read_cr4`、`write_cr0`/`write_cr3`、`invlpg`、`rdmsr`、`wrmsr`、`cpuid`、`rdtsc`、`read_rflags`、`xgetbv`——把控制寄存器 / MSR / CPUID 叶读回 rvdl 变量（`uint64`），补齐此前 `native()` 是 void 副作用、只有输入操作数、读不回寄存器值的缺口。`rdmsr`/`rdtsc`/`xgetbv` 采用 `={eax},={edx}` 双输出再组合为 `uint64`（规避 x86-64 下 `=A` 约束已不表示 edx:eax 对的坑）；`cpuid` 用输出指针形态一次指令写回 4 个寄存器。
- **裸汇编块 `asm { }` + `@[naked]`**：多指令 / 多寄存器汇编序列（`asm { "..." "..." }`，不做 `${}` 占位符替换）；`@[naked]` 函数体只含 asm 块时发射为 `define ... naked { ... unreachable }` 无栈帧，覆盖中断存根「保存全部通用寄存器 + iretq」、调度器切栈、gdt 远返回改 CS 这类单条 side-effect 汇编表达不了的操作。
- **函数指针与间接调用**：`fn<ret(params)>` 类型（等价 `(params)->ret`，映射 `ret (params)*`），`&fn` 返回真函数指针类型、`cb(args)` 间接调用；支撑 blkdev 读写回调表 / irq 中断回调 / 线程入口 / QPU 后端虚表，取代「设备枚举 + route 静态分派」。`&fn` 仍可降为 `cap<uint8>`（bitcast）供 IDT / 中断表直接存函数地址。
- **`@[packed]` 精确字节布局**：无 vtable 指针、无填充、packed struct `<{ ... }>`、字段从偏移 0 起，精确映射 GPT 头 / 分区表项 / FAT32 BPB / ATA IDENTIFY 等磁盘硬件结构，取代「cap 指针 + 手动字节偏移」。
- **volatile 内存访问原语**：`volatile_load` / `volatile_store` → `load volatile` / `store volatile`，MMIO 轮询在 `-O2` 下不再被优化成死循环（此前只能借用语义不完全匹配的 `sync_load` 原子读）。
- **定长数组**：`arr<T, N>` 类型 + `[...]` 字面量 + `a[i]` 索引（`[N x T]` + GEP + load），全局常量表 `@X = constant [N x T]`；静态查找表（键盘扫描码表 / 异常名表）不再需要 route 几十条 path 模拟。
- **复数类型**：`complex64`（`{ float, float }`）/ `complex128`（`{ double, double }`）+ `+`/`-`/`*` 复数算术（`extractvalue` + 分量运算 + `insertvalue`）+ `complex`/`real`/`imag`/`cabs`/`conj` 内建，支撑量子态矢量运算。
- **位域**：`T field : N` 语法，连续同类位域打包进同一存储单元（LSB 优先、超宽自动开新单元），读 = `lshr` + `and` 掩码、写 = 读-改-写（load + 清位 + 移位 or + store），配合磁盘结构位标志。
- **完整 uint64 运算**：无符号整数走 `udiv`/`urem`/`ult`/`ugt`/`ule`/`uge`/`lshr`（符号性随 `LLVMValue.signed` 经符号表传播），整数提升按符号性用 `sext`/`zext`；64 位字面量保留原始文本（`> 2^53` 不丢精度），承载 48 位 LBA 与 GPT 64 位字段。
- **IR 校验器增强**：`irverify.ts` 先剥离双引号字符串内容再扫描 SSA 寄存器，避免内联汇编模板里的 `%cr3`/`%rsp` 被误判为未定义的 LLVM SSA 寄存器。

### Fixed

- 修复 `null` 被 `cap<T1> → cap<T2>` 指针重解释分支误 bitcast 的回归（`cap<int32> p = null` 恢复为 `store i32* null`）。

## [0.6.0] (vsx 0.1.6)

### Added

- **多维标签函数 `@layer`**：以注解式标签 `@layer(time, thread, coord[, cost, deadline])` 取代单一 `rvdl_main` 入口，把执行过程映射到「时间 × 线程 × 运行层坐标」的多维拓扑空间。编译期 `TopologyBuilder` 自动推导平行/叠加关系与调用传播延迟（子函数 = 父时钟 + Δt），运行时 `TopoScheduler` 按拓扑分层调度（同层并行、层间顺序）。
- **可逆编织门合成范式（Reversible Weaving）**：标签体系`@[gate]`（可组合门单元）、`@[undo]`（可逆对偶 U†：门序反转 + 逐门取逆、`@[steer]`（相干控制 Λ(U)：控制位导引目标门）、`@[unitary]`（酉性验证）、`@[measure]`（测量点标记）；`gate-synth.ts` 在 AST 层自动合成 `<name>_undo`/`<name>_steer`。
- **受控门运行时实现**：`cx`/`ch`/`crz`/`cswap`/`c_toffoli`/`cqft`/`cbraid` 受控门 QIR 内建 + `IQuantumBackend` 基础门分解（CRz/CH/Fredkin/C³X/受控 QFT/受控 braid，到全局相位）；`iqft` 逆 QFT 内建（`qft† = iqft`）。
- **量子物理特性标签**：`@[coherence(t1,t2)]`（相干时间）、`@[noise("model")]`（噪声模型）、`@[basis(X|Y|Z)]`（测量基）、`@[decoherence_free]`（无退相干子空间）、`@[error_correction("code")]`（纠错码）。
- **噪声注入通道**：`@[noise]`/`@[coherence]` 门后自动注入噪声，`IQuantumBackend::apply_noise` 分发到 QVM（dense_state Kraus 通道）与超导/离子阱/中性原子/光子各硬件后端（`IdealStateCore::apply_noise_channel` 统一分发）。
- **经典编译属性**：`@[inline]`/`@[noinline]`/`@[pure]`/`@[readonly]`/`@[cold]`/`@[hot]`/`@[noreturn]`/`@[export]`，映射到 LLVM 函数属性（`alwaysinline`/`readnone`/`cold`/`dllexport` 等）。
- **IR 下沉（MIR → LLVM C++ API）**：`MirModuleBuilder` 以 LLVM C++ API 从序列化 MIR 构建 Module，经 `verifyModule` 校验；内建符号表覆盖全部 `ir.ts` declare（80+ 符号）；`mir_module_test` 固化为 CMake target。下沉路径补全 `NewObject`（对象构造 → `rvdl_create_*`）、`Drop`/`Consume`（量子资源释放）、`@layer` 拓扑入口（`emitTopologyEntry` 生成调度表 + `rvdl_topology_entry`）、`Member` form 字段访问（MIR 携带 `forms` 定义 + `MirModuleBuilder` 结构类型 + 精确 `getelementptr`）；`cli.ts` 新增 `--mir` 开关走 `COMPILE_MIR` 下沉路径。
- **`spawn` 真实并发**：`spawn` 块编译为独立线程函数 `@rvdl_thread_N`，经 `rvdl_spawn` 内建以 `std::thread` 启动（detach），取代此前的串行内联降级；支持**闭包捕获**（`collectFreeVariables` + env 结构传递外层局部变量）；`mir.ts` 补上 `SpawnStatement` lowering 消除借用检查静默跳过。
- **lambda lowering**：`mir.ts` 补 `FunctionExpression`（lambda）lowering，lambda body 的量子操作纳入借用检查（消除「MIR lowering skipped」静默降级）；`irverify.ts` 识别 `%env`/`%closure.*` 闭包类型名与参数。
- **非破坏 `expectation_z`**：`IdealStateCore::expectation_z`（⟨Z⟩ = 1 - 2·P(1)，不坍缩态），超导/离子阱/中性原子三后端重写。
- **光子去极化**：`PhotonicBackend` 的 depolarizing 从「损耗+相位翻转」近似改为正确的 Pauli 通道（X/Y/Z 以 3p/4 概率）。
- **新示例**：`examples/reversible_weaving.rvdl`（可逆编织范式端到端 demo：`@[gate]`+`@[undo]`+`@[steer]`+`@[noise]`）。
- **VS Code 扩展打包（vsx 0.1.6）**：esbuild bundle client/server 单文件（vsix 从 487 文件 795 KB 降到 22 文件 212 KB）；runtime 产物（`runtime.exe` + `rvdl_rt.dll` + `libomp.dll` + `zlib.dll`）打包进 `bin/`，`resolveRuntime()` 优先探测扩展自带 runtime，**安装即运行/编译/构建 rvdl**（无需单独安装 runtime）。
- **运行/编译/构建命令**：`ravendrel.runScript`（运行）、`ravendrel.compileScript`（AOT 编译为原生二进制）、`ravendrel.buildScript`（生成 LLVM IR 到 `.ll`），注册到编辑器标题栏 + 快捷键（`Ctrl+Alt+N/C/B`）。
- **语法高亮与语义感知补全**：补全注解标签（`@layer`/`@[gate]`/`@[undo]`/`@[steer]`/`@[coherence]` 等）与受控门（`cx`/`ch`/`crz`/`cswap`/`c_toffoli`/`cqft`/`cbraid`/`iqft`）的 TextMate 高亮 + LSP 补全。

### Fixed

- 修复 `parseAttributes` 把连续 `@` 全当 `@[...]`，导致 `@[gate] @layer(...)` 混排崩溃。
- 修复 `irverify.ts` 函数参数（`%arg0`）未计入 `definedRegs`，导致带参函数误报 SSA violation。
- 修复 `rz` 签名检查（原误判为 1 参数，实际为 qubit + angle 2 参数）。
- 修复受控门不在 `GATE_FNS` 导致 MIR 层误判「消费」参数、`@[gate]` 函数误报 E-TOP001、顺序调用被 Q-Digest 误判为跨线程竞争。
- 修复 `section`/`naked` 与历史 `place`/`raw` 命名分裂（`@[section]` 现在正确生成 LLVM `section` 属性）。
- 修复 `ir.ts` 的 `declare i8* @malloc` 与 LLVM ORC JIT 内建 `malloc` 冲突（`invalid redefinition`），改用 `rvdl_gc_alloc`。
- 修复 `build.bat`：C/C++ 编译器统一 clang-cl（消除 MSVC 混合报错）、CRT 对齐 `/MD`（`LLVM_DIR` 指向 `/MD` 版 LLVM）、补 CUDA/zlib/zstd 路径与 `CMAKE_C_COMPILER`。
- 修复 `prepare-vsix-runtime.ps1` 的 `Join-Path` 三参数错误与中文注释编码问题（改纯 ASCII）。

### Changed

- 入口函数：`rvdl_main` 由 `@layer` 多维标签函数取代（无标签程序仍回退脚本模式）。

## [0.5.0] (vsx 0.1.1)

### Added

- **完整语法高亮**：重写 `ravendrel.tmLanguage.json`，覆盖全部 100+ 个关键字、量子类型（`Qubit`/`QObject`/`QModel` 等）、内置函数（`qchain_*`/`rvdl_*`/`sync_*`/`cgui_*`/`cgfx_*`/`qrc_*`）、数字（十进制/浮点/十六进制）与单双引号字符串。
- **语义能力（Language Server）**：新增语义高亮（semanticTokens）、悬停提示（hover）、文档大纲（documentSymbol）、定义跳转（definition），补全从写死的 7 项升级为全量关键字 + 类型 + 内置函数。
- **扩展图标**：RVDL 语言图标（亮紫/紫双主题）、文件图标主题（file icon theme，左侧工作区 `.rvdl` 文件图标）、应用图标（runtime / rvdlRSP，含多尺寸 `.ico` 与 512 PNG）。

### Fixed

- 修复语法高亮注释规则：由 SQL 风格 `--` 修正为 lexer 实际使用的 C 风格 `//`。
- 修复语言图标深/浅主题无区分的问题。

### Changed

- VS Code 扩展打包配置：补充 `publisher`/`icon`/`repository` 元数据与 `.vscodeignore` 精确控制打包内容，移除误装的 `sharp`/`png-to-ico` 依赖。

## [0.4.0] (vsx 0.1.0)

### Fixed

- **QCOS 内核 shim 补充 QCOS syscall ABI**：rvdl 语言更新后 `server/src/ir.ts` 声明了完整 syscall ABI，但 `runtime/qcos/` 下 `rvdl_shim.cpp` / `rvdl_shim32.cpp` 仅实现 `rvdl_gc_alloc` 与 QMS 三函数，缺失 `rvdl_gc_free` / `rvdl_sys_call` / `rvdl_sys_calld` / `rvdl_sys_log` / `rvdl_sys_logi` / `rvdl_sys_callp`，使新语言内核链接失败。现补齐上述符号与闭包堆分配所需的 `malloc`，并新增 freestanding 辅助层 `runtime/qcos/qcos_syscall.hpp`（syscall 号 / 错误码 / x86 32·64 位串口 / TSC 单调时钟 / itoa，语义与 hosted `qhal/QcosSyscall.hpp` 一致）。

## [0.3.0]

### Added

- **QCOS 可启动内核（freestanding）**：`runtime/qcos/` 提供用 RVDL 编写的裸机内核引导源码树——Multiboot2 引导汇编（`boot_x86_32.S` / `boot_x86_64.S`，含长模式切换）、内核入口 `kernel_main.cpp`、`kernel_main` 桥接 `rvdl_shim.cpp` / `rvdl_shim32.cpp`、链接脚本 `linker_x86_64.ld` 与 GRUB 配置（`grub.cfg` / `grub_standalone.cfg`）。QCOS 内核隶属正式项目 [QuarkOS](https://github.com/LukaKrajina/QuarkOS)。
- **内核核心库 `qcos_core`**：新增 freestanding 静态库（`runtime/src/qcos_core.cpp`），无 LLVM / Kokkos / libstdc++ 依赖，仅靠编译器内置；新增宿主机冒烟测试 `qcos_smoke`（`runtime/src/qcos_smoke.cpp`），校验 Pmm / Sched / Spinlock / LibcSubset 逻辑。
- **纯 RVDL 内核构建链**：`scripts/build-qcos-kernel.ps1`（RVDL 源码 → LLVM IR → freestanding object → 链接引导 stub → 可启动 ELF）、`scripts/build-qcos-iso.ps1`（grub-mkrescue + xorriso → 可引导 ISO）、`scripts/build-grub-img.ps1`（grub-mkstandalone → QEMU `-kernel` 直启镜像，含 multiboot 头 VBE 标志修补）。
- **Windows 自动化构建脚本**：`scripts/build-runtime-windows.ps1`（自动定位 VS / CMake / Ninja / clang / CUDA / Kokkos / zlib / zstd / DIA SDK 并配置构建 rvdl_rt）、`scripts/build-kokkos-windows.ps1`（Kokkos 源码构建安装 + nvidia-smi 计算能力探测 + CUDA/OpenMP 后端）。
- **clang++ Windows 构建支持**：`runtime/CMakeLists.txt` 由 `if(MSVC)` 改为 `if(WIN32)` 平台判断；LLVM 组件改用 `LLVM_TARGETS_TO_BUILD`（覆盖 `InitializeAll*` 系列宏引用的全部后端入口）；统一依赖 CRT 运行时（Kokkos/GLFW 的 /MT 与 /MD 冲突）；`/ENTRY:mainCRTStartup` 经 `-Xlinker` 传递以兼容 clang++。
- **条件分支 `if / else if / else`**：新增 `IfStatement` AST 节点与 parser / IR / MIR lowering（`else if` 链统一表示为「else 分支只含一个 IfStatement」）。
- **新示例**：`qcos_hello.rvdl`（syscall ABI + QMS 数值内核）、`qcos_kernel.rvdl`（串口 + 位图分配器 + IDT 打包的内核镜像）、`qcos_quantum_service.rvdl`（量子服务下沉到裸机）、`rvdl_idt.rvdl` / `rvdl_idt2.rvdl`（IDT 条目打包）、`rvdl_bitmap.rvdl`（位图分配器）、`rvdl_bitwise.rvdl`（位运算）、`rvdl_buddy.rvdl`（伙伴系统）、`rvdl_basis_state.rvdl`（任意基构建）、`if_branching.rvdl`（条件分支）。

## [0.2.0]

### Added

- **工程地基**：单元测试框架（每用例异常隔离 + JUnit XML 报告）、CI 流水线（Linux 矩阵 + 覆盖率）、统一分级日志、ASan/UBSan 构建选项、`.clang-format` 代码规范。
- **核心仿真补充**：真实 IK 求解器（雅可比 DLS + 关节角提取）、量子电路仿真（拓扑感知 SWAP 路由 + 退相干误差）、VedaROS 桥真实数据流、物理阻尼系数。
- **质量门禁**：代码覆盖率选项（`RVDLRSP_COVERAGE`）、物理稳定性回归测试（能量守恒/穿透/堆叠）、性能基准 `rvdlRSP_benchmark`。
- **Broadphase 接入**：BVH 宽相碰撞检测，`PhysicsKernel` 碰撞检测从 O(N²) 降到 O(N log N)（500 球体 step 提速约 7 倍）。
- **打包**：CPack 安装器（TGZ / DEB）+ 安装规则。
- **QCOS 驯龙系统 PMM**：物理内存管理器从 v1 位图分配器（`BitmapPmm`）升级为「驯龙系统」`DragonPmm`——伙伴系统 + 侵入式空闲链表 + TLSF order 位图索引 + per-CPU 页缓存，支持单核无锁快路径与多核公平票据锁（`TicketSpinlock`），并支持动态核心数（调用者提供 per-CPU 缓存数组）。
- **rvdl 语言新功能与新关键字**：系统级编程（`cap<T>` 能力指针 / `unsafe` / `native` 内联汇编 / `outb`·`inb` 端口 I/O / `sync_*` 原子操作 / `rvdl_gc_alloc`·`rvdl_gc_free` 内核堆 / `addr` 取地址 / `rvdl_sys_call` 等 syscall ABI）、量子化命名新范式（`route`/`path`/`fallback` 路由分支、`spin` 自旋循环、`fixed` 常量、`flavor` 枚举、`fuse` 模式匹配）、并发与纠缠（`spawn`/`entangle`）、QMS 数值内核（`rvdl_qms_gap`/`rvdl_mix_bound`/`rvdl_qms_conc`）、函数属性（`@[section]`/`@[naked]`）与堆分配构造 `make`；编译管线新增 MIR 中间表示、Polonius 风格借用检查（量子线性类型 QLT）、Q-Digest 静态竞争检测与 VCGen 最弱前置条件演算。
- **任意基构建量子态**：新增 `basis_state(theta, phi, value)` 内置函数与 `rvdl_create_basis_state` C ABI，用布洛赫方向 (θ, φ) 参数化任意基构建单比特量子态（与固定基的 `DiracState`/`BellState` 互补）；`IQuantumBackend` 新增 `apply_basis` 门（QVM 精确 2×2 酉矩阵 + Polyhedral_Graph 递归），并补全此前缺失的 `rvdl_create_DiracState` 实现。
- **国产化加速适配（龙芯 CPU + 摩尔线程 GPU）**：`qcos/Arch.hpp` 新增 LoongArch（LA64）架构检测、`idle 0` 停转与 `cpu_relax()` 放松原语（龙芯 `dbar 0`），`Spinlock.hpp` 改用 `cpu_relax()`；新增 `qhal/GpuBackend.hpp` 统一检测 CUDA / 摩尔线程 MUSA / HIP GPU 后端与 AVX / 龙芯 LSX / LASX 向量扩展；`CMakeLists.txt` 加入龙芯 CPU 识别与摩尔线程 MUSA（mcc）自动探测。

### Fixed

- 修复 `PhysicsKernel` 启用 Kokkos 并行积分时 `Kokkos::View` 在 `initialize()` 前构造导致的崩溃。
- 修复测试进程退出时 CUDA driver 卸载崩溃（`cudaErrorCudartUnloading`）。
- 修复球体/胶囊体与 AABB 碰撞法线方向不一致导致的穿透（tunneling）。

### Changed

- 物理阻尼从硬编码 `*0.999` 改为物理阻尼系数（`linear_damping`/`angular_damping`），与步长无关。
- 全模块日志从裸 `std::cout`/`std::cerr` 迁移到统一分级日志（`RVDLRSP_INFO` 等）。

## [0.1.0]

- 初始版本：量子机器人仿真平台，含物理引擎（qpc）、渲染（render）、量子电路（circuit/qpu）、遥操作（qcdrc）、强化学习（control）、硬件抽象（hardware）、VedaROS 桥（bridge）等子系统。
