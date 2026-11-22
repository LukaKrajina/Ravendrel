# 图形加速与模型互转参考

本手册记录 Ravendrel 运行时里五个已落盘图形/互操作子系统的**用途与调用方式**。每项标注状态：已实现可用 / 有边界 / 默认关闭。理论依据与实验数据已收敛到文末一句话概念。

## 目录

- [Pa 像素加速器](#pa-像素加速器)
- [ModelInterop 模型互转](#modelinterop-模型互转)
- [A2a Android 适配](#a2a-android-适配)
- [Fox GUI 后端与回退](#fox-gui-后端与回退)
- [PNG / mipmap / 新图元](#png--mipmap--新图元)
- [命令面与测试](#命令面与测试)

---

## Pa 像素加速器

`runtime/include/utils/Pa.hpp` — **已实现可用**。四条逐位一致的机制：分箱（tile binning）、早期终止、部分重绘（damage region）、SDF 解析跨度。

| 接口 | 用途 |
| --- | --- |
| `DamageTracker` / `TileBins` | 脏区跟踪 / 每 tile 实例分箱 |
| `core_span` + `fill_span` / `drop_occluded` | 解析跨度填充 / 遮挡实例剔除 |
| `build_shade_rate_map()` | 按已知信息生成着色率图 |
| `CpuBackend::set_accel(false)` | 切回参考路径（免重编译，用于对照） |
| `stats()` / `damage()` | 命中率与统计（`pa::Stats::{ns_track, ns_clear, ns_draw}`） |

> VRS 加速默认关闭：需 `FOX_VRS=1` + `VK_KHR_fragment_shading_rate` 双门控，无 GPU 校验不静默生效。

## ModelInterop 模型互转

`runtime/include/qml/ModelInterop.hpp` — **已实现可用**，header-only 无三方依赖。在 `.rvdlm` 与 safetensors / GGUF 之间互转。

| 接口 | 用途 |
| --- | --- |
| `save_safetensors` / `save_gguf` | 导出（须显式给类型 `T`，因为 `.rvdlm` 不记录元素类型） |
| `load_safetensors` / `load_gguf` | 载入 |
| `export_rvdlm_to_*<T>` / `import_*_to_rvdlm<T>` / `import_*_to_rvdlm_auto` | 互转；`_auto` 按容器 dtype 分发 |
| `Metadata` | 有序 vector（GGUF KV 保序） |

```bash
rvdl export safetensors model.rvdlm model.safetensors [tensor] [f32|f64|i32|i64|u8]
rvdl import gguf model.gguf model.rvdlm [tensor]
# stdin：EXPORT_GGUF / IMPORT_GGUF <in> <out> [张量名]
```

边界：仅小端主机逐字节正确；GGUF 量化类型（Q4_0 等）读取即报错；BOOL 转 U8/I32。

## A2a Android 适配

`runtime/include/utils/A2a.hpp` — **已实现可用**，header-only，不含 Vulkan / fox 头。非 Android 上惰性（`available()=false`，质量 = −1）。

| 接口 | 用途 |
| --- | --- |
| `available()` / `current_quality()` | 是否 Android / 当前画质档 |
| `read_thermal()` / `tier_of_temp()` | 温度与档位 |
| `read_topology()` / `performance_cores()` / `compress_cpu_list()` / `pin_current_thread_to()` | CPU 拓扑与大核钉绑（宿主显式调用） |
| `classify_gpu()` / `attachment_policy()` / `quality_for()` | GPU 分类（是否 TBR，带 confident 标记）与画质决策 |
| `report_display_rotation/timing/gpu()` | 宿主上报显示/帧节奏信息 |

> 温度阈值是启发式（厂商差 ±10°C），GPU 是否 TBR 从渲染器字符串推断，均有标记，不作精确承诺。

## Fox GUI 后端与回退

`runtime/include/qhal/FoxHost.hpp` — **已实现可用**（75 项测试）。fox 优先，失败自动回退 Nuklear。

- `FoxApi`（函数指针表）/ `BackendPolicy`（纯状态机）/ `FoxHost`；`WindowFactory` 接缝。
- 三条初始化失败门：`create=null` / `init=0` / `ready=0` → 回退并打印不同原因。
- 故障累计（阈值默认 3）在帧边界切回，判死后不再重试（显示列表每帧重建，无需状态迁移）。

转发层：`runtime/src/ClassicRoute.hpp` / `ClassicGui.cpp` / `ClassicGfx.cpp`，`rvdl_cgui_*` / `rvdl_cgfx_*` 六个原语在 fox 活跃时转发（`FoxRoute.hpp` 的 `ClassicRoute`）。

## PNG / mipmap / 新图元

`runtime/include/ciUd/fox/image/png.{hpp,cpp}` — **已实现可用**（66 项测试）。自写 inflate + CRC32 + Adler32 + 行滤波（约 250 行 RFC1951 子集），避免引入 stb_image 破坏「删掉 Nuklear 仍能构建」。

| 接口 | 用途 |
| --- | --- |
| `Context::create_texture_png` / `create_texture_mipped` / `sample_rgba_lod` | PNG 纹理 / mip 链 / 三线性采样 |
| `fox_texture_create_png` | C ABI 入口 |

支持颜色类型 0/2/3/4/6、位深 1..16。新图元：`PT_ELLIPSE`（SDF `k1*(k1-1)/k2`）、三角形（绕向着色）、`push_path`（折线圆帽/圆角）、`push_gradient`（双色渐变）、C ABI `fox_path`（3 点可画，1 点拒绝）。

> 边界：Vulkan 逐级上传路径未经真机运行验证（本机无 ICD，仅语法级编译检查）；玻璃模糊与小尺寸 AA 明确不做（无 GPU 校验无法证伪）。

## 命令面与测试

```
环境：FOX_VRS=1 + VK_KHR_fragment_shading_rate 双门控（默认关）
脚本：runtime/include/ciUd/fox/backend/embed_spv.sh / embed_spv.ps1（bash+PowerShell 孪生生成器，产物逐字节一致）
测试：pa_parity_test、pa_pixel_benchmark、fox_png_test、fox_host_test、a2a_test、
      model_interop_test(.cpp/.py)、runtime/tests/gen_png_vectors.py
```

## 一句话概念

- **分块与早期终止**：当代 GPU 都是 tile-based 变体；每 tile 分箱 + 遮挡剔除 + 未变化 tile 不重画。
- **VRS**：按 tile 指定着色率约提速 15%，但 rate map 生成代价必须小于省下的代价，故 GPU 侧默认关。
- **部分重绘**：CPU 后端跨帧缓冲天然适合脏区重绘；GPU 交换链多帧在飞故不启用。
- **SDF 解析跨度**：圆角矩形内部 `dist<=-0.5` 有纯代数判据，核心跨度 alpha 恒为 1，与逐像素路径逐位一致。
- **遮挡剔除条件**：后继实例不透明（`a==255`）、非玻璃/阴影、内缩矩形完整覆盖，才能删被盖实例。
- **Fox 运行时回退**：驱动崩溃/扩展缺失只在运行时可知，故在帧边界切换、打印原因、判死不回头。

子系统源码与更多示例见仓库对应 `runtime/include/` 与 `runtime/tests/` 目录。