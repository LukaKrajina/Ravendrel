# 图形加速研究：Pa / A2a / Fox 补完 / Nuklear 退场

> 状态：设计已定稿，逐项落盘。本文是「先研究、再动手」的那份研究 —— 每一条设计
> 都标注了它来自哪个技术、以及**为什么在这个项目里成立**。
> 与 `docs/rvdl-shuttle-tracking.md` 同一套写法：结论、理由、以及被否掉的方案。

---

## 0. 五项任务与落盘顺序

| # | 任务 | 落点 |
|---|------|------|
| 1 | 像素级加速（降 CPU/GPU 占用） | `runtime/include/utils/Pa.hpp` |
| 2 | `.rvdlm` → `.safetensors` / `.gguf` | `runtime/include/qml/ModelInterop.hpp` |
| 3 | Android 特别优化 | `runtime/include/utils/A2a.hpp` |
| 4 | fox 取代 nuklear，fox 失败才回退 | `runtime/include/qhal/FoxHost.hpp` + `src/Classic{Gui,Gfx}.cpp` |
| 5 | 补完 fox | ellipse / 折线 / PNG+mipmap / 健康度 / C ABI |

顺序不能换：1 是 5 的地基（两者都要改 `cpu_backend.cpp`），4 依赖 5 的 C ABI 补齐。

---

## 1. 研究：业界正在用的四类像素级手段

### 1.1 分块与早期终止（tile binning + early termination）

- **VR-Pipe**（arXiv:2502.17078, 2025）指出当代 GPU（NVIDIA RTX / AMD Radeon /
  Intel Gen / ARM Mali）**全部**是 tile-based rendering 的变体：屏幕切成 tile，
  几何先分箱（bin），片元只在该 tile 内计算，且**支持 early termination**
  （tile 完成后剩余工作直接丢弃）。
- **GCC**（arXiv:2507.15300, 2025，3DGS 推理）把同一思想用在
  "preprocess-then-render" 两段式上：**早期剔除**（视锥外/不可见的高斯整段排除）
  与 **tile 内按需取数**（避免同一数据被不同 tile 反复加载）。
- **PowerVR 的 deferred** 更极端：**在可见性确定之前不做任何着色**（HSR）。

**取用**：CPU 后端是顺序扫描的路径，所以这三条都能直接用：
① 每 tile 的实例分箱（避免整帧遍历 16k 实例）；② **遮挡剔除**（某实例被一个
完全不透明、且完整覆盖它的后继实例盖住 → 整条实例可以直接从列表里删掉，GPU/CPU
同时受益）；③ 未变化的 tile **不重画**（damage tracking）。

**否掉的方案**：mesh shader / work graph。它们解决的是"十万级 draw call / 百万级
几何"的调度问题，而 Fox 一帧是**一条**管线 + 几千个实例，瓶颈在片元不在调度 ——
换来的复杂度（扩展检测、双路径、无法在此环境验证）一分钱回报都没有。

### 1.2 自适应着色率（VRS / NAS / DSR）

- NVIDIA **VRS**（Turing 起，D3D12/Vulkan 都有）：把屏幕切成 16×16 tile，
  **每个 tile 单独指定着色率**；**NAS**（Content/Motion Adaptive Shading）用
  上一帧的颜色方差 + 运动矢量生成 shading rate map，实测提升可达 15%。
- **DSR** ：对 tile 做 DCT 频域分析，平坦区域降采样，
  需要时再 SSAA。它明确说这是为 TBR 架构(移动端)设计的省电手段。
- NVIDIA 的官方提醒同样重要：**生成 shading rate map 的代价必须小于省下的代价**，
  否则就是纯粹变慢。

**取用**：`Pa.hpp` 生成 shading rate map，但**只用"已经知道"的信息**（实例类型、
是否文字/渐变/玻璃、覆盖率密度），不做额外的图像分析 —— 一张 rate map 是 O(tile)
的整数运算，远小于省下的片元。GPU 侧是否启用由扩展可用性与环境变量双重门控：
**没有 GPU 校验的代码不许静默生效**。

### 1.3 部分重绘 / damage region

- **Uno/Skia**（2025）实现 damage-clipped partial repaint，并额外留了一个
  `UNO_FORCE_FULL_REPAINT=1` 开关 —— "给我一条能测真实全帧代价的基准路径"。
- **OpenHarmony graphic_2d** 的 `RSDirtyRegionManager`：聚合脏区域、最小化重绘。
- Cocos Creator 的 dirty region 实测：UI demo 的 drawcall 从 366 降到 28。

**取用**：CPU 后端持有**跨帧**缓冲区，所以部分重绘在那里是天然正确的；
但判定必须保守、可观测、可关闭（`Pa::set_damage_tracking(false)` 就是
`UNO_FORCE_FULL_REPAINT` 的等价物）。GPU 侧（交换链多帧在飞）**不启用** ——
理由写进代码注释，不留"看起来能省"的陷阱。

### 1.4 SDF 与解析跨度（analytic span）

- SIGGRAPH 2025 的 *Real-Time GPU Vector Graphics SDF Generation* 用
  tessellation + fragment 并行求像素到笔画的距离；本质仍是"每个片元求一次距离"。
- **CTSS**（Cone-Traced Supersampling）用 cone 与半空间的解析交计算覆盖率，
  比 SSAA 便宜 15–30%。**思想是**：能解析算出来的，就不要逐像素采样。

**取用**：圆角矩形的 SDF 有一个**纯代数**的事实：对 iq 的 `sdRoundBox`，
内部满足 `dist <= -0.5` 的区域恰好是
`|px| <= bx - r - 0.5 && |py| <= by - r - 0.5`。
于是每条扫描线的**核心跨度**可以一行算出，那段写像素时既不用求 SDF 也不用混合
（alpha 必然是 1，因为 `smoothstep(-0.5, 0.5, -0.5) == 0`）。
这不是近似：它与逐像素路径**逐位相同**，所以可以拿"参考实现"直接证明。

---

## 2. Pa.hpp：像素加速器（不改变一个像素）

四条机制，**全部保持逐位一致**（这是硬约束，也是唯一能自动验证的约束）：

| 机制 | 省什么 | 为什么逐位一致 |
|------|--------|----------------|
| `DamageTracker` | 未变化 tile 的整段光栅化 | 每 tile 内容哈希不变 ⇒ 该 tile 的像素不会变 |
| `TileBins` | 每 tile 只遍历真正覆盖它的实例 | 遍历集合的**顺序与内容**不变，只是被裁剪 |
| `core_span` + `fill_span` | 圆角矩形内部区域的 SDF 求值与混合 | 该区域 `alpha` 恒为 1，写入结果与参考路径相等 |
| `drop_occluded` | 被不透明实例整块盖住的实例 | 那些像素必然被后继实例覆盖；不透明是**判定条件**，不是假设 |

**保守条件的证明要点**

1. 遮挡剔除要求：后继实例 `j` 是**不透明**（`color.a == 255`）、不是玻璃/阴影，
   且 `j` 的**内缩矩形**（`bx - r_max - 1`, 各边同）覆盖 `i` 的整个 quad。
   内缩 1 px 是因为圆角处 SDF 的 `<= 0` 区域最大也只能到 `r` 的圆心圈上；
   角上留 1 px 余量后，`j` 在该区域内**任何**像素的覆盖率都是 1。
   **玻璃不能当遮挡物** —— 它采样的是背景，删掉它下面的东西会改变它自己采样到的内容。
2. 核心跨度只在**统一半径**（四个角半径差 < 0.001，与参考代码同一个判据）时使用；
   否则退回逐像素。非统一半径的矩形本来就不多（面板/窗口），代价可忽略。
3. damage 哈希必须包含**纹理代际号**（`Context::texture_generation()`）：纹理重传后
   即使显示列表一模一样，像素也会变。

**GPU 侧**：`build_shade_rate_map()` 产出 16×16 的 rate map；是否真正启用由
`FOX_VRS=1` + `VK_KHR_fragment_shading_rate` 双门控（默认关）。

### 2.1 实测结果

跑法：`pa_parity_test`（逐字节对照）+ `pa_pixel_benchmark`（1000×680，每场景 120 帧，
CPU 后端 —— 它是**参考路径**，不是快路径；加速比是"加速前后同一个后端之比"）。
下表是**修复 `dirty_tiles_for` 的 y0 夹紧（§6.1）之后**重测的数字（连跑三次取示数一致的
一轮；修复前场景 [3] 是 1.10x，多着色的那部分面积就是差在哪儿）：

```
场景                        参考(ms) 加速(ms) 加速比  跳过像素  核心区
[1] 静态帧（内容不变）        3.37     0.131   25.7x     99.2%    80.9%
[2] 局部动画（一个控件在动）  3.22     0.169   19.0x     99.0%    79.9%
[3] 全屏动画（每帧都变）      3.16     2.33     1.36x     59.3%    74.2%
   遮挡剔除（400 控件+整屏面板）：2020 实例 → 340，删掉 83.2%
```

三个数字必须一起读。第 3 行仍是**诚实的失败**：整屏都在动时 damage 省不下像素，
哈希/分块本身却有成本，所以只有 1.36x。分段时间（`pa::Stats::ns_track/ns_clear/ns_draw`）
把原因摊开：登记哈希恒为 0.032 ms/帧，清屏 0.051，光栅化 2.16 —— 时间花在剩下的
41% 像素上，而不是"加速器本身很贵"。

**逐位一致是被测试抓出来的，不是推导出来的**。`pa_parity_test` 到目前抓到三个真 bug：

1. **核心区推导多减了一个半径**。参考 SDF 是 `d = min(max(q),0) + length(max(q,0)) - r`
   （`q = |p| - b + r`），于是 `alpha == 1` 的解是 `|p| <= b - 0.5` —— **半径被约掉了**。
   按"内缩 r"写出来的核心区会少画一圈像素。小数坐标的随机几何（场景 5）专门盯这类边界。
2. **"容量不够就重画整个剪辑矩形"的兜底**。脏 tile 会被清成背景色，干净 tile 不会；
   往干净 tile 上重画半透明边缘，等于把上一帧合成好的像素再混一次，**不可逆**。
   实测差 1 个色阶（239 vs 238），只在"脏 tile 数恰好超过容量"的帧出现。
   修法是按 tile 总数分配缓冲区，取消兜底 —— 不允许存在"保守但不对"的路径。
3. **`dirty_tiles_for` 交出的矩形只夹了 x、没夹 y0**（场景 7 抓到，见 §6.1）。实例顶边
   上方的半格被当成"要遍历的矩形"，图片没有覆盖率测试，会向上渗出最多 15 行。
   有覆盖率测试的图元在形状外写不进去，所以一直没暴露 —— **一个是非题被换成了
   一个逐字节比较**，才现形。

另外两处是**性能**上被抓出来的，记在这里免得被重新踩：

- 脏 tile 必须按"行带 × 跨度"合并后再画。一个 tile 一个 tile 地画会把同一行拆成 4 段、
  来回跳 x，cache 局部性直接崩：实测每像素成本是线性扫描的 2.3 倍，于是"跳过 59% 像素"
  的场景一点都不快（1.00x）。合并后退化成线性扫描 → 1.10x（y0 夹紧修复后又到 1.36x，
  见 §6.1）。
- 实例内容哈希按**字**（8 字节）而不是按字节：96 字节实例 × 每帧上千个 × 登记+比较两遍，
  按字节 FNV 的时间足够吃掉 damage 省下的全部收益（静态帧 16.8x → 24.2x 就是这一处的差）。

**开关**：`CpuBackend::set_accel(false)` 走逐像素参考路径（等价于旧的 `FOX_NO_PA`，
但不需要重新编译），`stats()`/`damage()` 暴露命中率与脏区计数。测试就是这么写出来的：
同一帧、同一显示列表、两个后端设置，逐字节比较。

---

## 3. ModelInterop.hpp：`.rvdlm` → safetensors / gguf

`.rvdlm`（`qml::ModelExporter`，魔数 `QKM1`）是**本项目自己的单张量 + 元数据**格式。
导出给外部生态需要两种容器：

- **safetensors**：`u64` 小端头长度 + JSON（`dtype`/`shape`/`data_offsets`）+ 数据段，
  JSON 用空格补齐到 8 字节对齐。**没有**元数据段（只有可选的 `__metadata__`）。
- **GGUF v3**：`GGUF` + 版本 + 张量数 + KV 数 → KV 段 → 张量信息段
  （名字/维度数/维度/类型/偏移）→ 按 `general.alignment`（默认 32）对齐 → 数据段。
  维度是**逆序**的（`ne[0]` 是最内层），最容易写错的一个点。

**取用**：写一个 header-only 的双向转换（`save_*` / `load_*`），并用
**独立的最小读取器**（测试里不调用生产代码）反向解析校验字节布局。
只做 `f32/f64/i32/i64/u8/bool` 的映射，遇到无法映射的类型**报错而不是猜**。

### 3.1 落盘结果：三份实现互相对账

`runtime/include/qml/ModelInterop.hpp`（header-only，无第三方依赖）：

| 接口 | 说明 |
|------|------|
| `save_safetensors` / `save_gguf` | 单张量导出；`Metadata` 是**有序** vector，导出前排序 |
| `load_safetensors` / `load_gguf` | 读回（可多张量）；越界/长度不符/未知 dtype/对齐非 2 的幂一律报错 |
| `export_rvdlm_to_safetensors<T>` / `export_rvdlm_to_gguf<T>` | `.rvdlm` 直通；**类型由模板参数声明** |
| `import_safetensors_to_rvdlm<T>` / `import_gguf_to_rvdlm<T>` | 容器 → `.rvdlm`；T 与容器 dtype 对账，不一致即报错 |
| `import_*_to_rvdlm_auto` | 同上，但类型按**容器 dtype** 自动分发（容器记录 dtype，`.rvdlm` 不记录 —— 两侧信息不对称决定语法不对称） |
| `gguf_meta_to_rvdlm` | GGUF KV 段 → `.rvdlm` 元数据的反向映射（`general.name`→`name`、`general.architecture`→`architecture`、`general.*` 其余丢弃） |
| `CpuBackend`… 无关；命令面：`EXPORT_SAFETENSORS` / `EXPORT_GGUF` / `IMPORT_SAFETENSORS` / `IMPORT_GGUF`（stdin 文本通道）与 `CMD_EXPORT_MODEL` / `CMD_IMPORT_MODEL`（二进制帧）；CLI 面：`rvdl export <safetensors\|gguf> <in.rvdlm> <out> [tensor] [f32\|f64\|i32\|i64\|u8]` / `rvdl import <safetensors\|gguf> <in> <out.rvdlm> [tensor]` | `EXPORT_GGUF <in.rvdlm> <out.gguf> [张量名] [f32\|f64\|i32\|i64\|u8]`；`IMPORT_GGUF <in.gguf> <out.rvdlm> [张量名]` |

**为什么导出必须显式给类型**：`.rvdlm` 的 `QKMHeader` **不记录元素类型**，只有
元素个数与形状。按错的类型读不会报错 —— f64 的 8 字节按 f32 重组出来是一份
"看起来像张量"的垃圾。所以导出的入口是模板参数，命令行的类型后缀默认 `f32`
并在应答里**回显实际用的类型**。

**为什么导入不必给**：容器自带 dtype（safetensors 的 JSON `dtype` / GGUF 张量
信息段的 ggml 类型号）。`import_*_auto` 按容器 dtype 分发；手动指定 T 的入口
仍然保留，但先与容器 dtype 对账，不一致即报错 ——「按错的类型读不会报错」
这条纪律在两侧都成立。两个已知边界：**BOOL 容器 → `.rvdlm` 不支持**
（`std::vector<bool>` 是位压缩特化、没有 `data()`，`numqk::Tensor<bool>` 先天
不可用 —— 先转成 U8/I32 再导入，不假装支持）；GGUF 的量化类型（Q4_0 等）
读取即报错。

**验证是三种实现互相对账，不是自证**：

1. 生产代码 `load_*`；
2. `runtime/tests/model_interop_test.cpp` 内的最小读取器（不调用生产代码一行）；
3. `runtime/tests/model_interop_crosscheck.py`（Python `struct` + `json`，第三份）。

自己写自己读只能证明自洽：`ne[0]` 写反、`data_offsets` 基准写错、JSON 长度差
8 字节，都能在闭环里原样通过。三份实现都同意才叫符合规范。测试还钉住两件行为：
**写出逐字节确定**（两次导出必须相同，否则文件不能进版本库/不能被哈希）、
**不能无损映射就报错**（u8/bool → GGUF 直接失败并说明 ggml 表里没有该类型；
读到未知 dtype/ggml 类型同样失败，不假装支持量化类型）。

**已知限制（写下来而不是绕过去）**：数据段按主机字节序 `memcpy`，因此这份实现
只在小端主机上逐字节正确（目标平台全是小端；大端需要逐元素交换，代码里不做，
也不假装做了）。

---

## 4. A2a.hpp：Android 特别优化

Android 的代价模型与桌面**不同**，所以优化项也不同（研究依据：Khronos Vulkan
Guide 的 TBR 最佳实践、Android NDK Vulkan design guidelines、Android Frame Pacing）：

1. **发热与功耗**是硬约束。Linux 桌面可以一直全速；手机上持续全速会降频，
   表现是"越用越卡"。所以 A2a 提供 **thermal 感知的质量档**（读
   `/sys/class/thermal/*`），热了先降玻璃模糊/阴影，最后才降帧率。
2. **大小核**（big.LITTLE / DynamIQ）。渲染线程应该钉在**性能核簇**上，
   否则它会被调度到小核上和后台任务抢时间片。核簇从
   `/sys/devices/system/cpu/cpuN/cpu_capacity` + `cpuinfo` 推导。
3. **TBR 的 load/store**：`LOAD_OP_CLEAR`（不要 `vkCmdClearAttachments`）、
   `STORE_OP_DONT_CARE`、`LAZILY_ALLOCATED` 的深度/暂存附件 —— 移动 GPU 上
   这些直接等于带宽和电量。A2a 暴露"本设备是不是 TBR"的判断，供后端选择策略。
4. **预旋转**与**帧节奏**：`currentTransform` 由合成器做旋转是要花电的；
   `VK_GOOGLE_display_timing` / Swappy 负责"在正确的时间呈现"。
   A2a 只做**可判定**的部分（是否存在旋转、是否支持 display timing），
   真正的呈现由宿主决定。
5. **非 Android 平台上一律惰性**：`a2a::available()` 返回 false，
   所有探测返回 -1。理由与 fox 的剪贴板一致：**"没有这个东西"必须是一个可判断的值**，
   而不是"一个会静默返回垃圾的桩"。

### 4.1 落盘结果：可判定性与"不猜"

`runtime/include/utils/A2a.hpp`（header-only，**不 include Vulkan/fox 头**：它输出策略值，
含 Vulkan 枚举数值常量，由后端自己翻译 —— 于是它能在没有 Vulkan 的机器上被测）。
测试 `runtime/tests/a2a_test.cpp` **不需要真机**：所有解析都接受一个可注入的"根目录"，
fixture 覆盖真实 sysfs 的形状（多簇、capacity 抖动、读失败、整摄氏度、只有电池 zone）。

| 关注点 | 接口 | 关键判定 |
|--------|------|----------|
| 惰性 | `available()` / `current_quality()` | 非 Android 恒 false / 中性值（**没有证据 ≠ 设备很热**） |
| 热状态 | `read_thermal()` / `tier_of_temp()` | 只算 **SoC zone**：充电时 `battery` 的 88°C 不能把等级顶到 Critical |
| 单位 | `read_thermal()` 内 | 值 < 1000 视为整摄氏度并归一 —— 否则热降级整条链路静默失效 |
| 大小核 | `read_topology()` / `performance_cores()` / `compress_cpu_list()` | `cpu_capacity` 优先，`cpufreq` 兜底；**比值 > 1.15 才分簇**（判等会把一簇切成七八个假簇） |
| 钉核 | `pin_current_thread_to()` | **宿主显式调用**，库不偷偷改调度策略；失败给可读原因 |
| TBR | `classify_gpu()` + `attachment_policy()` | `LOAD_OP_CLEAR` / `STORE_OP_DONT_CARE` / `transient_depth` / `memoryless` |
| 质量档 | `quality_for()` | **降级顺序**：模糊 → 阴影 → 最后才是帧率（由测试钉死：`fps < 60 ⟹ 模糊/阴影已全关`） |
| 宿主上报 | `report_display_rotation/timing/gpu()` | 需要 JNI/扩展枚举才知道的事**由宿主报告**；A2a 不猜（猜错的旋转会画出倒着的画面，且不报错） |

**两条必须说清楚的边界**：
- thermal 等级的温度阈值是**启发式**（`ThermalThresholds`，厂商间差 ±10°C）。Android 官方的
  thermal status 在 PowerManager（Java/JNI 侧），NDK 的纯 C++ 拿不到 —— 不声称这些数字是标准。
- GPU 是否 TBR 从渲染器字符串**猜**（返回值里带 `confident` 标记）：真正的判定需要扩展或
  厂商文档。猜错只是策略次优（多一次无关的 `LOAD_OP_CLEAR`），不会画错 —— 所以允许猜，但把
  "我在猜"说出来。

---

## 5. fox 取代 nuklear：切换、判定、回退

现状：`rvdl_cgui_*` / `rvdl_cgfx_*` 是 Nuklear 实现（`ClassicGui.cpp` /
`ClassicGfx.cpp`，复用 `qgui::RenderContext`）。目标：**fox 优先，fox 失败才用 Nuklear**。

**为什么不是"编译期二选一"**：用户要的是"fox 异常时回退"，那是一个**运行时**判定 ——
驱动崩溃、扩展缺失、窗口创建失败，都只在运行时才知道。所以需要：

1. `FoxHost`：fox 的宿主（自己建 GLFW 窗口 + `fox_init_backend_vulkan`）。
   **只依赖 `fox.h`**（C ABI），因此不引入任何 Vulkan 类型，可被纯逻辑测试驱动。
2. **失败即失败**：`fox_create` 返回 null、`fox_init_backend_vulkan` 返回 0、
   `fox_backend_ready` 为 0，三条都会走到 Nuklear 路径，并**打印一条原因**
   （静默回退是这轮最想避免的东西：用户会以为自己在看 fox）。
3. **运行中异常**（帧提交失败、后端报错）→ `FoxHost` 记录 fault 计数，
   达到阈值后**下一帧切回 Nuklear**，并把已渲染的帧内容交给 Nuklear 重新画一遍
   （显示列表在 rvdl 侧每帧重建，所以切换不需要状态迁移）。
4. 策略层（`BackendPolicy`）是**纯状态机**，用假的 fox API 驱动测试：
   正常路径、init 失败、运行中失败、失败后不再重试 fox。

### 5.1 落盘结果：判定点在哪、证据在哪

`runtime/include/qhal/FoxHost.hpp`（`FoxApi` 函数指针表 + `BackendPolicy` + `FoxHost`，
只依赖 `fox.h`）、`runtime/src/ClassicRoute.hpp`（cgfx 的转发接缝）、重写的
`ClassicGui.cpp`（`rvdl_cgui_*` 的 fox 优先路由 + 窗口接缝 + 质量档对表）与
`ClassicGfx.cpp`（六个 cgfx 原语在 fox 活跃时全部转发）。`rvdl_rt` 链接 `fox_core`。

| 决定 | 做法 | 判据（`runtime/tests/fox_host_test.cpp`，75 项） |
|------|------|------|
| 谁来画 | `BackendPolicy` 纯状态机 | 正常路径逐帧 `active()`；故障累计到阈值即切；阈值可配（默认 3，测试也跑 1） |
| 三条初始化失败门 | create 返回 null / init 返回 0 / ready 为 0 | 三条各自落到 Nuklear，且 `reason()` 是**三条不同的文本**（第三条逐字来自 `fox_error()`），失败时窗口与上下文都被回收 |
| 绝不静默 | 每次决定经过 `report` | 测试按子串读回打印内容（不是"有没有打印"） |
| 帧边界切换 | 切走那一帧仍由 fox 画完，下一帧全部落到 Nuklear | 显示列表每帧重建，所以不需要状态迁移；策略绝不在帧中途翻转 |
| 不回头 | 判死后 `active()` 恒 false | 之后 `fox_*` **一个入口都不再被调用**（假 API 记录每一次调用），二次 init 也拒绝且不再调 `fox_create` |
| 质量档 | A2a `current_quality()` → `fox_set_quality()` | 假 API 上三档转发值逐项断言；真实现每 60 帧对一次表、每次变化打印一行 |

窗口是 `WindowFactory` 函数指针接缝（真实现是 GLFW），所以整套判定在**无窗口、
无 Vulkan、无 GPU** 的机器上可跑 —— `fox_host_test` 就是这么跑的（本机 CMake 配置
因缺 Vulkan 而失败，这个测试照样全绿）。

---

## 6. 补完 fox

| 缺项（README 自陈） | 做法 |
|---|---|
| 没有描边折线图元 | `push_path` 已存在并展开为 `PT_PATH` 段；补 **widget 用法**（勾选框改用它，16 个实例 → 2 个）+ C ABI `fox_path` |
| 勾选框对勾是"一串圆点" | 同上；`PT_PATH` 的圆帽即真实折线 |
| 没有椭圆 | 新增 `PT_ELLIPSE`（精确 SDF：`k1*(k1-1)/k2`）+ 着色器 + CPU 参考 + SPIR-V 重生成 |
| 没有 PNG 解码器与 mipmap | 自写 **inflate**（PNG 的 IDAT 只用得到它）+ 行滤波器 + mipmap 链 + 三线性采样（落盘，见 6.1） |
| `rvdl_cgui_*` → `fox_*` 薄封装未写 | 第 5 节 |
| 玻璃模糊质量 / 小尺寸 AA 偏硬 | **明确不做**：都是"看起来更好"的改动，在没有 GPU 校验的环境里无法证伪，属本轮划界之外 |

**PNG 为什么自写 inflate 而不是引 stb_image**：fox 的立身之本就是"删掉 Nuklear 后
仍然能构建"。stb_image 是另一份第三方实现，加进来就把"自有"变成了"又藏了一份"。
inflate 的 RFC1951 子集（stored / fixed / dynamic Huffman）约 250 行，
**而且可以用已知向量精确验证**（`""` / 单字节 / 长重复串 / LZ77 回溯窗口边界）。

### 6.1 落盘结果：PNG / mipmap / 三线性

`runtime/include/ciUd/fox/image/png.{hpp,cpp}`（自写 inflate + CRC32 + Adler-32 +
行滤波器 + 色彩类型 0/2/3/4/6、位深 1..16 + mip 链 + 双线性/三线性 + `zlib_store`）。
接线：`Context::create_texture_png` / `create_texture_mipped` / `sample_rgba_lod`，
C ABI `fox_texture_create_png`，CPU 光栅器在 `PT_IMAGE` 上按每实例算一次 LOD
（texel/pixel，`log2`；无 mip 的纹理仍走原来的最近邻，既有像素一个不动）。

**证据不是"看着对"，而是三类可判定的命题**（`runtime/tests/fox_png_test.cpp`，
66 项，向量由 `runtime/tests/gen_png_vectors.py` 生成 —— 生成器入仓，向量可复核）：

| 命题 | 判据 |
|------|------|
| 解对 | zlib 自己产出的流解回原字节：空 / 单字节 / 300 B / 1 KB / 70 KB（重复结构，跨窗口回溯）/ 每种颜色类型与位深 / 五种行滤波器 |
| 覆盖到 | `InflateStats` 计数：**stored / fixed / dynamic 三种块类型都 > 0**（"我觉得覆盖了"不算证据）；`Z_FIXED` 策略专门用来逼出 fixed 块 |
| 拒绝对 | 每条非法流断言**错误文本**：越界回溯、保留块类型 3、长度码 286/287、CRC、Adam7、行滤波器 5、palette 越界、zlib 头/Adler。只断言"失败"的解码器会把"什么都解不出来"当成通过 |
| 写对 | `zlib_store()` 与 `zlib.compress(data, 0)` **逐字节相同**（空/20 B/300 B），70 KB 自写流往返且恰好 2 个 stored 块 |
| 金字塔对 | 4x4 色块图：level 1 精确还原（含 a=128 / a=64 的预乘平均，不许漂）；level 2 = 整图平均，数字**手算**写在注释里 |
| 采样对 | texel 中心 = 该 texel 原值；lod 越界 = 最后一层；uv 越界 = 钳边缘；小数 lod = 两级双线性的线性插值 |

**LZ77 窗口边界是手写的**：zlib 不会为了测试恰好用上距离 32768，也永远不会产出
"距离 32769"。前者用固定 Huffman 位写入器构造（2 个字面量 + 126 个「长 258、距 2」
游程把输出撑到 32768，再回溯距离 32768、长度 3 —— 多一字节就越界）；
后者在 DEFLATE 里**根本表示不出来**（码 29 的 13 位额外位覆盖 24577..32768），
所以真正的"回溯越界"是"只输出了 1 字节却引用距离 2"。两个手写流都先喂给
Python 的 zlib 验证（合法的必须解出预期、非法的必须报 `invalid distance too far back`）。

**像素对拍（`pa_parity_test` 场景 7）**：同一张 4x4 不透明四色块纹理，
① 带 mip 缩到 1x1 ⇒ 整图平均 `(128,128,128,255)`（缩小不再走样）；
② 不带 mip 的同图 ⇒ 最近邻 `(255,255,255,255)`；两条都验证加速开/关逐字节相同。

**这一场景又抓到 Pa.hpp 的第三个缺陷**（前两个见 §2.1）：`DamageTracker::dirty_tiles_for`
把脏 tile 交出去时只夹了 `x`、没夹 `y0`，于是实例**顶边上方的半格**也会被着色。
有覆盖率测试的图元（圆角矩形/椭圆/三角形/文字/玻璃）在形状外写不进去，所以一直
没暴露；**图片没有覆盖率测试**（矩形内逐像素采样纹理），会向上渗出最多 15 行。
旧场景的图标顶边是透明的，笔画落在透明像素上等于没画 —— 换成不透明纹理立刻现形。

**明确划在外面的**：
- **Adam7 交错**：明确报错拒绝，不静默解错（`interlace` 位非 0 即失败）；
- ~~**Vk 后端只上传第 0 级**~~ **已补齐**：`make_image` 接受层数、
  `upload_texture` 逐级 `vkCmdCopyBufferToImage`（每级自己的 mipLevel /
  bufferOffset / extent，extent 逐级减半最小 1x1）、屏障与 image view 的
  `levelCount` 覆盖全链、sampler 开 `mipmapMode=LINEAR` + `maxLod=16`。
  层数变化（`create_texture` ↔ `create_texture_mipped`）触发图像重建 ——
  拿 1 级图像拷 5 级数据，`vkCmdCopyBufferToImage` 只会静默忽略越界区域
  （subresourceRange 之外不报错）。本机无 Vulkan ICD，因此该路径**未经
  运行验证**，只做了语法级编译检查 —— 这一点写下来而不是绕过去；
- 玻璃模糊 / 小尺寸 AA 的品质（见上表最后一行）。

### 6.2 落盘结果：新图元（椭圆 / 三角形 / 折线 / 渐变）

| 图元 | 落点 | 关键决定 |
|------|------|----------|
| 椭圆 | `CmdType::Ellipse` → `PTYPE_ELLIPSE` | 边界精确的 SDF `k1*(k1-1)/k2`（`q = p/r`；圆退化为 `\|p\|-r`，这是要盯住的判据）。远离边界时梯度是近似（AE 场），但 AA 斜坡只活在边界 ±1px 内，那里是精确的 —— 矩形近似在小尺寸上会把圆画成方，且失真方向随半径变 |
| 三角形 | `CmdType::Triangle`，顶点进折线共用的顶点缓冲 | 填充 = 三条边的带符号距离取 `max`；**绕向在着色器里算**（叉积符号），调用方两个方向都能列 |
| 折线 | `push_path` 展开成逐段 `PT_PATH`（圆帽 / 圆角连接） | 勾选框从「16 个圆点」变成一条 3 点折线（展开 2 个实例）。**HEAD 里的 `push_path` 一直在空转**：`cmd.rect` 恒为 `{0,0,0,0}`（注释写着「后端会重算」，后端从未重算），`build_instances()` 丢掉所有非正尺寸矩形的命令 —— 描边折线"存在"了很久、勾选框却还在用一串圆点，根因就是这一行。修法：push 时算 AABB（含描边半宽与圆帽外扩），因为裁剪 / damage / 遮挡剔除全部以 `cmd.rect` 为输入；场景 6 因此断言"段中点必须是描边色" |
| 双色渐变 | `push_gradient`（矩形 + 第二停止点） | 第二停止点的 alpha 当启用位、轴由 `Cmd::gradient_horizontal` 给出 —— 「有停止点没轴」的半指定渐变会静默画成纯色矩形，所以它需要是具名入口而不是给 `push()` 加两个参数 |

**证据（`pa_parity_test` 场景 6）**：椭圆中心实色、AABB 角落仍是背景（SDF，而不是矩形
近似）、边缘存在半透明 AA 斜坡；三角形质心填充、外侧保持背景（绕向无关）；折线段
中点被描边；**遮挡安全** —— 大椭圆 / 大三角形即使盖住控件也不能当盖面（只有
`PT_RECT` + 不透明才能剔除），一旦有人把盖面判定扩到这两个类型，这里会变成"内容消失"。
C ABI `fox_path` 另在 `fox_c_test.c` 里有 C 侧覆盖（3 点画得出、1 点被拒绝）。

**SPIR-V 是重生成的，不是手贴的**：`backend/embed_spv.sh` 与 `embed_spv.ps1` 是一对
孪生生成器（bash + PowerShell），产物逐字节一致，`spirv-val` 干净。生成脚本的两个
缺陷（缩进按字重复、CRLF 写两次）在这轮修掉 —— 手贴的 SPIR-V 和 GLSL 源是两份可以
各自"自洽"的东西：`sdPath` 少传一个实参就是这么躺着的，直到 glslc 拒绝编译。
