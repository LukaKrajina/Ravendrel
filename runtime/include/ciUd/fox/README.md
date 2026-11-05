# Fox — 自研图形用户界面库 / 框架

对标 **macOS 27 (Golden Gate)** 桌面 UI 风格的 GUI 框架，设计目标是最终**取代 Nuklear**。

```
runtime/include/ciUd/fox/
├── fox.h       C ABI（纯 C99）—— 宿主与 rvdl 语言唯一的接入面
├── fox.cpp     C ABI 实现，零逻辑，全部转调下面的 C++ API
├── fox_c_test.c 用 C 编译的冒烟测试：证明头文件真的是 C 干净的
├── core/      types, context, anim       —— 最底层原子，不依赖其它 Fox 模块
├── theme/     Golden Gate 设计令牌        —— 颜色 / 度量 / 动效，纯数据
├── draw/      display list                —— 原子绘制命令缓冲
├── text/      font + glyph atlas          —— 自有 TrueType 光栅化
├── input/     输入状态机
├── layout/    立即模式布局游标
├── widget/    原子控件（按钮/开关/滑块/…）
├── window/    窗口装饰（交通灯/侧边栏/玻璃标题栏）
├── backend/   Vulkan SDF 渲染器 + GLSL 着色器
└── detail/    stb_truetype 私有副本
```

依赖方向严格单向向下，每层可单独测试与裁剪。
**C++ API 是行为的唯一真相**，C ABI 只是转调 —— 所以 bug 不可能只存在于其中一个表面。

---

## 为什么自己带一份 stb_truetype

`detail/stbtt_impl.hpp` 是从 `vendor/Nuklear/nuklear.h`（行 12209..16755）**原样提取**的
stb_truetype v2 实现。

理由不是"省一个依赖"：**一个要取代 Nuklear 的库，如果还 include Nuklear 的头文件，那就没有
真的去掉依赖，只是把它藏起来了。** Nuklear 内部那份没有 `nk__` 前缀（裸 `stbtt_`/`stbrp_`
符号），所以可以整段提取。授权不变（MIT / public domain）。

### 提取过程中踩到的 API 陷阱（都已实测确认）

这些都是 stb_truetype **静默返回错误但貌似合理的值**，不报错，只是画面不对：

| # | 陷阱 | 后果 |
|---|------|------|
| 1 | `stbtt_InitFont(info, data, OFFSET)` 收**字节偏移**；`stbtt_PackFontRanges(.., INDEX, ..)` 收**face 索引** | 对 `.ttc` 两者不同（offset=32 / index=0）。混用会让 CFF 解析器越界崩溃 |
| 2 | `stbtt_GetFontVMetrics` / `stbtt_GetCodepointHMetrics` 返回**字体单位**；`stbtt_GetCodepointBitmap` 自己收 scale | 没有任何函数会隐式套用 `info->scale`。当成像素会把每个基线推到屏幕外约 100 倍 |
| 3 | `stbtt_GetCodepointHMetrics(info, cp, &advance, &lsb)` 顺序固定 | 反了会静默交换 advance 与 bearing，两者都是貌似合理的 int |
| 4 | 烘焙过 scale 的 fontinfo 对 `GetCodepointBitmap` **无效** | 传 `1,1` 会按字体单位光栅化（20px 的 'A' 变成 ~1394px），直接撑爆图集 |

`text/font.hpp` 把这四条写在了文件头，`text/font.cpp` 在每个用到的地方都标了 `trap #N`。

---

## 渲染器

`backend/` 是 Fox 自己的 Vulkan 管线，**不经过 Nuklear**。

- 单条管线，实例化四边形，逐片元求 **SDF 圆角矩形** SDF —— 圆角/边框/渐变/软阴影/磨砂玻璃
  全部由同一个 shader 表达。这是 Nuklear 后端做不到的（`nk_draw` 只有矩形和字形）。
- 玻璃走两趟：整帧先画进离屏 `scene` → `vkCmdBlitImage` 4 倍降采样（**降采样本身就是模糊**）
  → 玻璃片元采样该图。
- 着色器编译成 SPIR-V 后由 `embed_spv.ps1` 内嵌进 `shaders_spv.h`，因此构建期**不依赖 glslc**
  （Linux/macOS builder 与 Android NDK 交叉编译不一定装了着色器编译器）。

### 调试开关

```bash
FOX_VALIDATE=1   # 打开 Khronos 校验层
FOX_TRACE=1      # 打印 display list → instance 的转换统计
FOX_NOBLUR=1     # 强制走无玻璃路径（单趟直接画到 swapchain）
FOX_VRS=1        # 允许启用 VRS（配合 damage 的 rate map；默认关）
```

`FOX_VALIDATE=1` 是排查 Vulkan 问题的主要手段 —— 驱动在
`vkCreateGraphicsPipelines` 内部崩溃时，不开校验层完全无从定位（本项目就靠它定位到
"viewport/scissor 为空且未声明动态状态"这一条）。

### 与项目加速器的关系：`utils/Pa.hpp`

`backend/cpu_backend.cpp`（参考实现，Linux 下也能跑）消费 `runtime/include/utils/Pa.hpp`。
它是**项目**给 fox 的加速器，不是 fox 的一部分：fox 不 include 加速器的任何内部类型，
只通过 `CpuBackend` 的几个方法用到它。三条要记住的性质：

1. **开关即参考实现**：`CpuBackend::set_accel(false)` 关掉加速，走逐像素路径。
   这取代了早期的 `FOX_NO_PA` 编译开关（那要求重新编译，而"关掉加速"恰恰是**测试**
   最需要的动作）。同样的输入、同样的输出，只是慢 —— 逐字节一致性因此可验证，
   而不是靠推导：`runtime/tests/pa_parity_test.cpp` 就是同一帧渲两遍再对字节。
2. **每帧三段可观测**：`stats()` 里有跳过像素数、核心区命中数、被剔除实例数，以及
   加速器自己的分段时间（登记哈希 / 清屏 / 光栅化）。`damage()` 暴露脏区。
   没有这些数，"为什么这个场景没有变快"只能靠猜。
3. **两个不变量是别处代码的责任，不是加速器的**：
   - `Context::texture_generation()` 必须在纹理重传后 +1；
   - `FontManager::atlas_generation()` 必须在新字形**首次栅格化**后 +1。
     damage 的帧键由「背景色 ⊕ 纹理代际 ⊕ 图集代际」组成；字形是**惰性**栅格化的
     （第一次画到才解码），所以「显示列表一模一样、像素却变了」是真实存在的情况，
     不允许出现帧键不变的漏画。

遮挡剔除（`drop_occluded`）不只在 CPU 后端生效：它跑在 `build_instances()` 里，
所以 Vulkan 后端同样受益（实例数减少 = 顶点/实例带宽减少）。它是**判定**而非假设：
只有后继实例不透明（`a == 255`）、非玻璃/非阴影、且其内缩矩形整块盖住前者时才删。
`Context::hidden_culled()` 报告本帧删了多少（每帧重置，关掉剔除时必为 0）。

字形基线的历史缺陷（`bitmap_top = ascent + lgy`）由 `runtime/tests/fox_metrics_test.cpp`
钉住：它只检查**可量**的东西（底边是否落在基线上、降部是否低于基线、代际号单调），
因为"人眼看着歪"不是一条可回归的判据。

---

## 构建

已加入 `runtime/CMakeLists.txt`：

```cmake
add_library(fox_core STATIC ...)   # theme/text/input/layout/core/widget/window
                                    # + draw/instance.cpp
                                    # + backend/vk_backend.cpp + backend/cpu_backend.cpp
                                    # + fox.cpp（C ABI）
add_executable(fox_demo ...)       # C++ showcase（Vulkan，需要窗口）
add_executable(fox_c_test ...)     # 以 C99 编译的 C ABI 冒烟测试
add_executable(fox_headless ...)   # CPU 光栅化 + headless 回读：不要窗口、不要 GPU
add_executable(fox_text_test ...)  # TextBuffer 单元测试：只链接一个 TU
```

`fox_text_test` 只链接 `text_buffer.cpp`，**不链接 `fox_core`**。这是刻意的：
编辑模型的难点（光标不落在多字节中间、输入合并成一步撤销）与渲染无关，
而通过渲染器去测它，等于让一个坏掉的 GPU 能让纯逻辑测试失败。

**这个 target 是有回报的**：它抓到了 40 号（反向 UTF-8 步进返回原位，导致退格在
ASCII 上无效、Ctrl+← 死循环）—— 这两个都靠"跑一下看看"发现不了。

`backend/vk_backend.cpp` 与 `fox.cpp` 都在 **fox_core** 里而不是 demo 里：
C ABI 需要渲染器，而一个不能渲染的 "core" 对宿主来说没有意义。

`fox_c_test` 这个 target 的意义在于**让构建在 `fox.h` 不再 C 干净时失败**。
一个 C++ 测试会痛快地接受引用、默认参数和命名空间，所以"C 能编译"必须用 C 来证明。

`fox_headless` 不链接 GLFW 的窗口路径，也不需要 Vulkan 设备，所以它在**任何**环境
里都能跑（QuarkOS、CI 容器、没有驱动的机器）。

与项目其余部分共用同一工具链（clang-cl + lld-link + vendor GLFW + Vulkan SDK）。

### ASan 构建

```bat
build_fox_asan.bat          REM 输出到 %TEMP%\foxasan
```

本机 LLVM 只提供**动态** ASan 运行时，隐式链接会拉到与 CRT 冲突的静态版本
（`duplicate symbol: malloc/free/...`），所以要显式指定
`clang_rt.asan_dynamic-x86_64.lib` + `clang_rt.asan_dynamic_runtime_thunk-x86_64.lib`，
并把 `clang_rt.asan_dynamic-x86_64.dll` 放到 exe 旁边。

关键取舍：**ASan 只插桩 Fox 自己的翻译单元**。Vulkan loader 与 NVIDIA 驱动不被插桩，
它们的分配只是经过 sanitizer 分配器。这是刻意的范围 —— 第五轮那次堆损坏的嫌疑人
本来就在 host 侧代码里，而它一分钟就被抓了出来。

修改着色器后重新生成内嵌头：

```powershell
powershell -File runtime/include/ciUd/fox/backend/embed_spv.ps1 `
    -Glslc "$env:VULKAN_SDK\Bin\glslc.exe"
```

---

## 当前状态（2026-10-12 打磨后）

### 本轮修掉的真实缺陷

按严重程度排列，全部有可复现的验证手段：

1. **`FontManager` 悬垂指针（文字全部变豆腐块的根因）**
   `GlyphAtlas::bind(&e.face)` 在 `std::vector<Entry>` 里存了裸指针。加载第二个字号触发
   realloc 之后，所有已绑定的 `face_` 全部悬垂 → `glyph()` 解引用已释放内存 → 每个字形
   都走 fallback 分支画成豆腐框。
   **验证**：`detail/realloc_probe.cpp` 加载 4 个字号后重新取第 1 个字体的 `'B'`，
   修复前 `exit code -1073741819`（访问违例），修复后正常。
   修复：`faces_` 改 `std::deque`（push_back 不使已有引用失效）+ `bind()` 移到
   `push_back` **之后**（否则捕获的是局部变量的地址）。

2. **`GlyphAtlas::glyph` 返回可写共享静态**
   `static AtlasGlyph s_fallback` 被按引用返回，两个缺失字形会互相别名。改为成员
   `fallback_`。

3. **每个字号重读一次字体文件**
   `load_file` 每次 `fread` 整个文件（msyh.ttc 约 19 MB），一个五个字号的界面就是
   约 95 MB 磁盘读 + 五份相同副本。加了按 `(path, face_index)` 的字节缓存。

4. **`load_file` 异常不安全**
   原来在 `faces_` 里 `emplace_back` 后逐步 `pop_back` 回滚，且 malloc 的
   `stbtt_fontinfo` 在中途失败时泄漏。改为在栈上构造完整 Entry 再一次性 move 进去。

5. **单一字形图集 → 多字号互相采样**
   不同字号的光栅化位图尺寸/前进宽度都不同，共用一张纹理会让 13px 的字采样 28px 的
   格子（度量全对、画面全错）。改为**每个 FontId 一张图集**，命令携带 `font`，
   后端按**连续段**（run）分批，只在字体变化时换 descriptor。
   保留 run 而非"先画所有形状再画所有文字"：实例是 alpha 混合、几乎无深度测试，
   **绘制顺序就是绘制次序**，把文字集中到最后会让标签盖住其后绘制的面板。

6. **`vkEndCommandBuffer` 漏调**（上一轮）
   绘制命令录进了命令缓冲但缓冲停在 RECORDING，`vkQueueSubmit` 拒绝它。
   表现是「窗口正常、清屏正常、就是不画东西」，与"着色器没生效"无法区分。

7. **fence 在 SIGNALIZED 状态提交**
   fence 建时给 `SIGNALED` 让首次 `vkWaitForFences` 不阻塞，但提交前必须
   `vkResetFences`，否则部分驱动丢弃整次提交。

8. **viewport/scissor 为空且未声明动态状态**（上一轮）
   逐帧 `vkCmdSetViewport` 就必须同时声明 `VK_DYNAMIC_STATE_VIEWPORT`/`SCISSOR`，
   否则驱动在 `vkCreateGraphicsPipelines` 内部崩溃（`0xC0000005`）且不报错。

9. **每帧 free 仍在飞行中的 command buffer**
   `vkFreeCommandBuffers` 作用在 pending 缓冲上是
   VUID-vkFreeCommandBuffers-00047。改为复用单个 `frame_cmd_`，仅在 shutdown 释放。

10. **实例缓冲数据竞争**
    原来 `memcpy` 写持久映射的实例缓冲**在** `vkWaitForFences` **之前**，GPU 可能仍在读
    上一帧的数据 → 偶发、不可复现的几何错乱。已调整为先等待再写。

11. **副屏与离屏共用一个 render pass**
    两者 `initialLayout`/`finalLayout` 本就不同（swapchain 是 PRESENT 往返，离屏是
    UNDEFINED→COLOR_ATTACHMENT），共用一个 pass 必然有一个是假的。已拆成
    `render_pass_` / `scene_pass_` + 两个 pipeline。

12. **shader module 在第二个 pipeline 创建前就被销毁**
    `scene_pipe_` 用的是已释放的 module
    （VUID-VkPipelineShaderStageCreateInfo-module-parameter）。

13. **descriptor 绑定越界写 null imageView**
    Vulkan 既不允许 COMBINED_IMAGE_SAMPLER 绑 null view，也不允许绘制时 binding 未写。
    加了 1×1 占位图（`create_dummy_image`）。

### 第二轮：画面空白与"看不见"的四个根因

上一轮结束时 UI 只剩标题栏和三个交通灯。四个独立缺陷叠在一起，逐个剥掉：

14. **`vkCmdDraw` 参数顺序错误（只画第一批实例的主因）**
    ```c
    vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
    ```
    批次偏移要选的是**实例**，所以它必须放在**第五个**参数。原来写成了第四个，
    驱动被要求从 6 顶点的四边形缓冲的**第 18 个顶点**开始取数据 —— 越界，于是第 2
    批及以后的 24 个批次里 364 个实例全部静默消失。
    **为什么它能躲过校验层**：命令缓冲合法、批次表正确、`firstVertex` 只是越界读取
    而 Vulkan 对它的上界检查要到 draw 时才由驱动做，校验层不报。
    **验证**：把 24 个批次合并成一个 `first=0` 的整批绘制，画面立刻完整（37,908 →
    361,846 个非背景像素），说明实例数据一直是对的，错的只是取数起点。

15. **NDC 又翻了一次 Y**
    `gl_Position = vec4(ndc.x, -ndc.y, 0, 1)`。**Vulkan 的裁剪空间本来就是 +Y 朝下**
    （framebuffer 坐标也是），所以这里不需要翻转，是从 OpenGL 习惯带过来的。
    **为什么它能躲过眼睛**：整窗背景矩形上下对称，翻转后看着完全正常；只有不对称的
    细节（标题栏、交通灯、文字基线）才会悄悄跑到另一头。OpenGL 是相反约定，别把
    那个习惯搬过来。

16. **字形 uv 又翻了一次 Y**
    `v_uv = vec2(in_unit.x, 1.0 - in_unit.y)`。`GlyphQuad::uv0/uv1` 里 stb_truetype
    给的 V 已经是"第 0 行 = 字形顶部"，图集也是按行上传的，所以 uv0.y 本来就指着
    顶部。翻转后每个字形原地上下颠倒 —— 而单词仍然从左往右读，看起来像字体问题
    而不像 uv 问题。

17. **空格被画成豆腐框**
    `stbtt_GetCodepointBitmap` 对**空白字形**和**字体里根本没有这个码位**返回同样的
    `nullptr`，而下面那条路径无法区分，于是给两者都画了一个空心框。界面上每一个空格
    都顶着一个悬在基线上方的方框。已按码位先把空白拦掉：只推进笔、不绘制。
    **验证**：把字形图集导出成图片放大看 —— 图集里躺着一个孤零零的豆腐框，正是空格。

18. **swapchain render pass 的 `initialLayout` 写成了 `PRESENT_SRC_KHR`**
    `loadOp` 本来就是 `CLEAR`，上一帧的内容必然被丢弃，所以 `initialLayout`
    应当是 `UNDEFINED`（也是所有 Vulkan 教程的写法）：等于告诉驱动"进入时的状态
    我不关心"，驱动可以省掉一次真实 barrier。
    写成 `PRESENT_SRC_KHR` 时，校验层把交换链图像的当前布局记为 `UNDEFINED`，
    而 render pass 断言它是 PRESENT —— 这个矛盾被**记在了绘制时绑定的那个
    descriptor 上**，报成 `VUID-vkCmdDraw-None-09600`："某张交换链图像在绘制时
    被当作 `u_blurred` 采样"。`u_blurred` 和交换链图像其实都跟这件事无关。

    上一轮为此把 `blur_layout_`/`scene_layout_`、`srcAccessMask` 配对、descriptor
    写入时机全查了一遍 —— 方向完全错了。**报错的字段名指向谁，就去怀疑谁的布局；
    但报错的位置（draw 时）不等于出错的对象。**

19. **`create_descriptors()` 跑在 `create_sampler()` 前面**（顺手修掉的真缺陷）
    第一次 descriptor 写入时 sampler 还是 `VK_NULL_HANDLE`。已调换顺序，并在
    `write_descriptors()` 里加了 `if (!sampler_) return;` 兜底。
    注意：这个**不是**上面那条 `VUID` 的原因，别把它当同一个 bug 的修复。

### 地面真值：`dump_ppm()`

排查过程中最大的浪费是**用窗口截图判断"画面是不是空的"**：截图会被别的窗口挡住、
在 resize 中间拍到、或者拍到合成器的陈旧表面，于是"UI 是空白的"变成一句无法证伪的
断言，只能靠猜。

`VkBackend::dump_ppm(path)` 把交换链图像真的从显存拷回主机并写成 PPM，绕开合成器：

```bash
FOX_DUMP=out.ppm fox_demo.exe      # 渲染 4 帧后回读并退出
```

它顺带统计"与左上角背景色不同的像素数"，把判断数字化：空画面是 0，完整界面是
三十多万。**同一个工具定位了上面 14 号缺陷** —— 因为它能一眼区分"数据没生成"和
"生成了但没画出来"。

> ⚠ 校验层的输出**不会**进 `Start-Process -RedirectStandardError` 重定向的文件，
>   它直接写进程控制台。早期我一直靠读那个文件判断"有没有报错"，于是把
>   `count = 0` 当成干净，**反复得出相反的错误结论**。判断校验是否通过必须看控制台
>   输出本身（或者自己装一个写文件的 debug messenger），不要读重定向文件。

配套的调试开关：

```bash
FOX_VALIDATE=1        # Khronos 校验层 —— 驱动崩溃/静默失败一律先开这个
                      # ⚠ 输出走进程控制台，不会进重定向文件。别读重定向文件判断
FOX_TRACE=1           # display list → instance 的转换统计
FOX_NOBLUR=1          # 强制无玻璃单趟路径（玻璃出问题时用来隔离）
FOX_SCALE=2           # 模拟 HiDPI：窗口与帧缓冲一起放大到 2 倍密度
FOX_DUMP=out.ppm      # GPU 回读真实交换链并退出（地面真值）
FOX_DUMP_FRAMES=90    # 回读前先渲染多少帧（用来抓定时器驱动的变化）
FOX_EXIT_AFTER=25     # 跑 N 帧后干净退出（用来抓 teardown 缺陷）
FOX_BEAT=1            # 每帧一行心跳；崩溃前最后一行就是崩溃点
FOX_ATLAS=out.ppm     # 导出正文图集的 CPU 像素，放大后退出（区分"图集里有豆腐框"
                      #   和"uv 指错了格子"—— 屏幕上两者长得一模一样）
```

### 当前验证状态

- `BUILD OK`
- **1x**：交互运行稳定（12/12），玻璃与无玻璃两条路径校验层均为 0 错误，
  `FOX_DUMP` 回读 363,734 / 680,000 像素非背景，整套控件全部可见
- **1x 干净退出：约 80–90% 概率堆损坏**（见上"仍然存在的两个缺陷"A）
- **2x：第 0 帧崩溃**（本轮引入的回归，见上 B）


### 第三轮：图像原语（第 1 条地基的第 1 块）

`CmdType` 此前只有 Rect / Border / Text / Glass / ClipPush / ClipPop —— **没有 image**。
侧边栏的"图标"其实是 `◆ ◇` 这些文本字形。字形图标不能独立染色、不能独立缩放，
还继承标签字体的光栅化结果，所以那不是图标。

新增：

- `CmdType::Image` + `Cmd::tex` / `Cmd::src`
- `Context::create_texture / update_texture / destroy_texture / texture`
  —— **后端无关的 RGBA8 注册表**，只管像素，不管怎么进显存
- `image()` / `image_region()` 两个控件函数
- shader `PTYPE_IMAGE` + **binding 3**
- demo：一张 128×32 的四格图标表，侧边栏改用真实图标；另加一张**按定时器重写像素**
  的纹理，用来跑通重上传路径

三个值得记下来的决定：

20. **不能用"采样器数组 + 实例携带纹理下标"**
    Vulkan 明确要求聚合成数组的采样器只能用常量整型表达式下标索引
    （`VUID-VkDescriptorSetLayoutBinding-descriptorType-06482`）。per-instance 的
    纹理下标在规范层面就不合法。
    正确做法是**按纹理再分一批 descriptor set**，和现有的按字体分批是同一个形状。
    代价是纹理切换处要重新绑定；收益是完全合法且实现与现有架构同构。

21. **必须有 sub-rect，否则图标表不成立**
    没有 sub-rect 的话每个图标一张纹理 = 每个图标一个 descriptor set，20 个图标的
    工具栏就是 20 个 set 只画 20 个四边形。`Cmd::src` 存归一化子矩形，
    实例里走 `color2`（与文字用 color2 传 uv1 是同一个约定）。
    这条是一开始设计时没想到、写 demo 时才暴露的。

22. **批次键从 font 改成 descriptor set 下标**
    原来 run 是按字体 id 划分的。加了 image 之后，形状和图片会被并进同一个 run，
    于是图片批绑到形状那个 set 上，采样到 1×1 占位图，**什么都不画**。
    run 的真实身份是"这批实例需要哪些绑定"，所以键就该是那个下标。

同时修掉的两个真缺陷（都是加图像后被校验层点名的）：

23. **render() 里 fence 等得太晚**
    `upload_atlas()` / `sync_textures()` 都会调 `vkUpdateDescriptorSets`，而它们原本跑在
    `vkWaitForFences` **之前** —— 上一帧的命令缓冲可能还在执行，改写它引用的
    descriptor 是竞态。已把等待提到 `render()` 的第一行。实例缓冲的覆盖写竞态也一并
    由这一步解决。

24. **单条 `sem_render_` 与呈现引擎抢用**
    一个共享的 render-finished 信号量会在交换链图像 N 仍被呈现引擎读取时，被第 N+1 帧
    重新 signal（`VUID-vkQueueSubmit-pSignalSemaphores-00067`）。表现为偶发撕裂而不是
    报错。改为**每张交换链图像一条信号量**（规范推荐做法，也顺带硬化了主路径）。
    `dump_ppm()` 另外自己 signal 一条专用信号量 —— 复用主路径那条就是同一个竞态，
    而且呈现等待一条没人 signal 的信号量会被
    `VUID-vkQueuePresentKHR-pWaitSemaphores-03268` 直接拒掉。

### ✅ 第五轮：ASan 定位，A 与 B 都已修复

上一轮记的两个"未解决"缺陷，**根因其实是同一个** —— 而且都不是 HiDPI 的问题。

#### 怎么定位的

给 Fox 加了一个独立的 ASan 构建（`build_fox_asan.bat`，输出到 `%TEMP%\fxasan`）。
本机 LLVM 只提供**动态** ASan 运行时，所以链接时显式指定
`clang_rt.asan_dynamic-x86_64.lib` + `clang_rt.asan_dynamic_runtime_thunk-x86_64.lib`，
并把 DLL 放到 exe 旁边 —— 隐式选择会拉到与 CRT 冲突的静态版本，报一堆
`duplicate symbol: malloc/free/calloc/realloc`。

关键点：**ASan 只插桩 Fox 自己的翻译单元**。Vulkan loader 与 NVIDIA 驱动没有被插桩，
它们的分配只是经过 sanitizer 的分配器。这是刻意的范围 —— 嫌疑人本来就在 host 侧代码里。

它一次跑就抓到了：

```
ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 16384 ... 0 bytes after a 4135-byte region
```

#### 缺陷 A（以及 B）的真正根因

`fox_demo.cpp` 的 `build_icon_sheet()` 分配的是 **w\*h 字节**，
而 `Context::create_texture(w, h, rgba)` 按约定读 **w\*h\*4** 字节：

```cpp
std::vector<u8> px((size_t)w * h, 0);        // 4096 字节
// ...
c.create_texture(128, 32, px.data());        // 读 16384 字节 → 4 倍越界
```

`16384` 正是 `128×32×4`，`4135` 正是那个 4096 字节分配（加上分配器取整）。
`pulse_px` 同样的错（`kCell*kCell` 应为 `kCell*kCell*4`）。

**缺陷 B 根本不是 HiDPI 的缺陷** —— 2× 下第 0 帧崩溃只是同一个越界在更大分配上的表现。
把越界修掉之后，2× 一次通过。

这也解释了为什么它"只在有纹理时出现"、"交互运行不出现、干净退出才出现"：
读越界不会立刻崩，它只是踩坏了相邻堆；踩到什么、什么时候被 `free()` 检测到，
取决于分配布局 —— 所以表现是随机且延迟的。

#### ASan 顺带挖出的第三个缺陷（比 A 严重，而且与本轮无关）

```
ERROR: AddressSanitizer: global-buffer-overflow
READ of size 1 ... 0 bytes after global variable
  'struct fox::EdgeTracker fox::g_edges' of size 259
```

`InputState::keys_down / keys_pressed / keys_released` 声明为 **`[128]`**，
但 `Key::Backspace = 0x100`（256），命名键整体落在 `0x100..0x11C`。

于是每帧写 `in.keys_down[256]` 都是**越过 128 字节数组 128 字节**，
直接踩掉后面的 `chars`、修饰键与指针状态；`g_edges.key_down[256]` 又越过
256 字节数组 1 字节。

还有一个被同一个错误界掩盖的行为缺陷：`InputState::end_frame()` 只清理前 128 项，
所以 `keys_pressed[0x100..0x11C]` 一旦按下就**永久停在 true** —— 第二次按 Tab
会表现得像同时按住了好几个键。

修法：引入 `KEY_SLOTS = 0x120`（由枚举推出），三个数组与 `EdgeTracker` 都按它定长，
并在两个写入循环里逐项再做一次边界检查（那张表是数据，写错了本该是断言失败而不是
静默越界写）。

**这个 bug 会让方向键、Tab、Enter、Esc、F 键污染鼠标状态 —— 如果没有 ASan，它几乎
不可能被定位到，因为它看起来像"键盘有时不好使"。**

### 当前验证状态

- `BUILD OK`（发布构建，C++ demo 与 C ABI 测试两个可执行文件）/ `ASAN BUILD OK`
- **C ABI 测试：以 C99 编译、链接、运行，校验层 0 错误，`[ctest] OK`**
- **ASan：含 C ABI 的构建，`FOX_DUMP` 正常退出，0 错误**
- **发布构建 1x 干净退出：0/10 崩溃**（修复前 8–9/10）
- **发布构建 2x 干净退出：0/6 崩溃**（修复前 0/6）
- 校验层：C++ demo 与 C ABI 测试，玻璃与无玻璃两条路径均为 0 错误
- **HiDPI 已验证**：1× 与 2× 同一逻辑区域的回读图**逐像素一致** —— 布局、位置、
  尺寸完全相同，只有光栅化分辨率不同。`FOX_SCALE=2` 下帧缓冲 2000×1360，
  侧边栏 400/2000 = 20%，与 200/1000 逻辑一致
- 图像原语逐项验证：子矩形选择、染色、40% alpha、拉伸、**重上传现在真的生效了**
  （`Texture::generation` 已修，此前从未重上传过）

### ✅ 第六轮：C ABI 层

`fox.h`（纯 C99）+ `fox.cpp`（C++ 实现），外加一个**用 C 编译的**冒烟测试。

#### 分层

`fox_*` 是**库自己的** C API（对应 Nuklear 的 `nk_*`）。
项目的 rvdl 侧薄封装（`rvdl_cgui_*`，见 `qhal/ClassicGui.hpp`）属于项目，不在这里。
这样切分正是项目今天对 Nuklear 的做法，也让 Fox 可以被任何宿主复用。

约定：`bool` 写作 `int`；字符串是 UTF-8 且**只借用不持有**；矩形是**逻辑单位**；
调用即立即模式，不保留调用方状态；**没有 C++ 引用、默认参数、异常跨边界**。

#### 为什么要一个"用 C 编译的测试"

**"头文件能编译"不等于"ABI 能用"。** 一个 C++ 测试会痛快地接受引用、默认参数和命名空间，
而这些在 C 里全是错误。所以 `fox_c_test.c` 用 `/TC` 编译：

- 它立刻抓到了 `getenv` 缺 `<stdlib.h>`（C99 不允许隐式声明）；
- 如果 `fox.h` 里混进任何 C++ 东西，构建直接失败。

它还真的调用了整条链路：生命周期、frame、窗口装饰、标签/按钮/开关/勾选/滑块/
输入框/分隔线/面板/玻璃条、纹理创建+更新+绘制、主题色与度量读取，最后 `fox_dump_ppm`
回读一张图作为证据。

#### 顺带修掉的两个缺陷

31. **surface 所有权判断反了**
    我上一轮认定"surface 来自 `glfwCreateWindowSurface`，归 GLFW 所有"，于是把
    `vkDestroySurfaceKHR` 删了。C ABI 测试一开校验层就报：

    ```
    vkDestroyInstance(): VkInstance has 1 leaked objects ... VkSurfaceKHR
    VUID-vkDestroyInstance-instance-00629
    ```

    **那个工厂函数是把所有权交给调用方的**，GLFW 只是代建。已恢复销毁，并写清顺序：
    **必须先 `fox_destroy()`（含 surface + instance），再销毁 GLFW 窗口**。

    这是本轮最讽刺的一处：上一轮我为了修一个不存在的 bug，引入了一个**每次退出都报**
    的真 bug，而我当时没有验证层可看，所以没发现。

32. **`text_field()` 全应用共用一个 id**
    `make_id(0xB0000005, "field")` 是常量，于是屏幕上**每一个输入框共享同一份
    `WidgetState`**：聚焦第二个会把焦点环画到第一个上，两个光标位置是同一个数。
    输入框没有 label 可用，所以改用**位置**派生 id（吸附到整像素，避免布局抖动导致
    "控件变成另一个控件"而丢状态），并新增 `id_hint` 参数供调用方固定身份。

33. **阴影是一条不透明的黑色硬边色块**（本轮由 CPU 后端暴露，Vulkan 侧同样存在）
    `build_instances()` 把阴影的四边形向外撑开 `blur` 像素，于是 SDF 的 `d == 0`
    落在**撑开后的边界**上，而不是卡片边界上；而 shader 写的是对称的
    `1 - smoothstep(-blur, blur, d)`。由于 quad 内部 `d` 永远到不了 `+blur`，
    这条曲线只能走到 0.5 就被 quad 边界硬切掉 —— 结果是一条全黑、有清晰轮廓的
    色带，根本不是阴影。正确方向是**向内**衰减：
    `1 - smoothstep(-blur, 0, d)`（`d == 0` 处为 0，卡片边缘处为 1，藏在卡片下面）。
    两个后端必须同时改，否则 CPU 参考实现立刻失去意义。

34. **阴影的撑开放大泄漏到了图形自身的填充**（同上，本轮）
    撑开写的是对 `d` 的**原地修改**，而 `d` 随后既被复制成阴影实例、又被当作
    图形自身的填充用。结果是**凡是带阴影的矩形，四边都被画大了 `blur` 像素**。
    之前没被发现，是因为阴影先画、基本盖住了被撑大的填充；真正露出来的是超出
    预期边缘的那一圈。（顺带把三个"用阴影矩形顺便当深色垫底"的 hack 改成
    `shadow_rect()`，顺便修掉了它们的同样的放大。）

35. **Widget id 依赖控件的值 / 是常量**（本轮）
    `toggle_switch` 的 id 是 `make_id(salt, value ? "sw1" : "sw0")` —— **值一变，
    id 就变**，于是每次点击都换一份全新的 `WidgetState`，而新槽位的 `anim == 0`。
    表现是"滑块动画卡住不动"，真因是 id 从动画底下滑走了。
    `slider` 的 id 是 `make_id(salt, "slider")` —— **全应用所有滑块共用一份状态**，
    屏幕上两个滑块时拖动任意一个会同时移动两个。
    现在统一走 `widget_id(salt, rect)`：按 (控件种类, 屏幕位置) 派生，唯一、逐帧稳定、
    且不依赖值或 label。

36. **`state_cache_` 从不回收**（本轮）
    `WidgetState::last_frame` 早就标了注释写着 "GC marker"，但**回收器从来没写**。
    在把 id 改成位置派生之前这不显眼；改完之后，滚动列表每滚过一个像素位置就会
    多留一份状态，永不回收。新增 `Context::prune_state_cache()`，按**秒**（不是帧）
    判定，10 秒未触碰即回收 —— 用秒是为了让阈值在 15 fps 和 240 fps 下含义一致。

37. **勾选框的对勾是一个点**（本轮）
    原来是两个轴对齐小矩形（4×2 和 2×4），在 16 px 的框里糊成一坨，像脏点而不像对勾。
    Fox 目前没有旋转图元，改成沿两条臂铺一串直径等于线宽的圆点（cap 和拐角都是圆的，
    相邻点重叠所以不会出现虚线），代价约 16 个 instance。
    **真正的解法是补一个描边折线图元**（树的连线、迷你折线图同样需要）。

38. **`Cmd::color` 默认是不透明黑**（API 陷阱，本轮踩了）
    给 Rect 挂 `Shadow` 而忘了设 `color`，就会画出一整块黑板 —— 加上 34 号的泄漏，
    表现为控件外围一圈硬黑边。这是本 API 最容易写错的地方，因此新增
    `shadow_rect()`：只投影、不填色，并注明调用顺序。

39. **headless 渲染出的永远是 `anim == 0` 的一帧**（本轮）
    `frame_begin()` 读真实时钟，而 headless 是紧凑循环调用它，测出的 dt 是微秒级，
    于是所有动画都停在起始值 —— 参考图里开关的旋钮永远在左边，与真实状态无关。
    新增 `Context::set_fixed_dt()`：钉住 dt 并绕开时钟。它同时也让 golden-image
    测试成为可能（真实时钟会让每次跑出来的图都不一样，diff 毫无意义）。

### 本轮新验证能力

**`fox_headless` —— 不需要窗口、不需要 GPU、不需要驱动的渲染器。**
这是"第二后端"真正的价值所在，也是本轮抓出 33 号的**唯一**原因：33 号在 Vulkan
侧同样存在，但驱动没有报错、校验层也不会说话，而回读图里它就摆在那儿。

它同时给了项目三样东西：

1. **QuarkOS 路径**：`runtime/qcos` 是 freestanding 内核（171 行 `kernel_main.cpp`，
   只有 IDT/PIC/PIT/任务/串口，**没有显示驱动、没有 framebuffer、没有 libc**）。
   OpenGL 后端在那里永远跑不起来，只有 CPU 光栅化器可以 —— 而它恰好就是
   `CpuBackend` + `fox_headless`。
2. **确定性的视觉回归**：窗口截图不是证据（会被遮挡、可能在 resize 中途被抓、可能
   抓到过期的合成表面）。读自己缓冲区的像素没有这类失效模式。
3. **GLSL 的参考实现**：两个后端消费同一份 `InstanceData`，所以如果它们给出不同的
   画面，就一定有一个是错的，而且有可复现的方法找出是哪一个。

`draw/instance.cpp` 就是为此抽出来的：`InstanceData` / `Batch` / 转换逻辑全部从
`VkBackend::build_instances` 里搬出来共享。**一份实现，不是两份** —— 否则两边会
慢慢漂移，而漂移看起来会像"CPU 渲染器有点不对劲"，而不是"我们在一个地方修了、
在另一个地方弄坏了"。

## 本轮结论

**"参考实现"最值钱的不是省一个后端，是它会在主后端身上找出主后端自己的 bug。**
33 和 34 号都活在 Vulkan 路径里，已经存在很久，没有任何一层会报告它们。

**"缩小后的视图"不能用来判断方向和字形。** 我在 1000×680 的整图上认定文字被水平
镜像了，几乎要改 uv；6× 放大之后 "Fox" 清清楚楚是正的。像素级的小尺寸渲染图，
在判断方向这类问题上是不合格的观测手段 —— 和窗口截图一样不可信。

**先问"这次能修掉哪一类错"，再动手。** 33 号的修法有三种写法（改 padding、改
ramp、改两者），而真正的原因是 padding 泄漏（34 号）—— 如果只盯着"阴影那条带子
不对"就会只改 ramp，留着一个更隐蔽、更普遍的 bug。

40. **UTF-8 反向步进返回了光标自己的位置**（本轮，由单元测试抓到）
    `prev_boundary(s, i)` 从 `i` 开始向前扫连续字节，**返回 `i` 本身**。于是
    对每一个 ASCII 字符（`s[i-1]` 不是连续字节，也就是绝大多数情况）它返回原位：
    - **Backspace 在 ASCII 上什么都不删**；
    - `move_word` 反向**死循环** —— 它的内层步进是 `i = s`，而 `s == i`。
    两处都不会崩：一个吃掉按键，另一个直接挂死进程。

    这个 bug 靠"跑一下看看"是发现不了的：Backspace 表现为"没反应"，需要用户
    恰好注意到；死循环需要恰好按下 Ctrl+←。**单元测试一次就抓到了**。

41. **测试工具自己的悬垂指针**（本轮，ASan 抓到）
    `check()` 的 `detail` 原本是 `const char*`，调用点用
    `(std::string(...) + ...).c_str()` 构造 —— 临时对象在**整个调用期间**就已
    销毁，`detail` 指向已释放内存。而**普通构建下 68 条断言全部通过**，因为那块
    内存碰巧还留着正确的字节。ASan 把它报成读取越界。
    一个会静默读已释放内存的测试工具不是测试工具，是"感觉安全但并不安全"。

42. **MSVC 把零长度字符串字面量标记为"零个可寻址字节"**（本轮，踩坑记录）
    ASan 构建下，**读 `""` 的任何一个字节都会报 global-buffer-overflow** ——
    它的 shadow 是全局红区（`f9`），不是可寻址内存。
    起因是我为了绕过 41 号而写了 `want[0] == '\0'` 这种"显然合理"的判空，
    结果正是这一读触发。结论不是"在比较里绕过去"，而是**不要用零长度字面量**：
    测试改用独立的 `eq_empty()` 断言。

## 本轮：输入闭环（第 1 步 —— 编辑模型）

### `TextBuffer` —— 选区、撤销、剪贴板不是三个功能

它们是**同一个东西的三个视图**：一个带光标、带锚点、带历史的缓冲区。
往裸 `char*` 上挂任意一个，最终都会把这个结构重新发明一遍，而且散落在控件里、
历史寄存在控件状态恰好多出来的字段里。所以直接把它做出来，`text_field` 只是它
的一个视图。

**UTF-8 是全部难点。** 所有下标都是**字节偏移**（因为调用方持有的是字节），
但光标**永远不能落在多字节序列中间**。错了不会报错 —— 它会把一个码位劈开，
下一次编辑把后半截变成 U+FFFD。对一个 CJK 字符按退格是最容易踩到的入口。

**撤销用快照而不是增量。** 增量省内存，但一个输入框只有几十字节、历史有上限；
快照是几行代码，而且没有"增量应用错了"这一类 bug。这里每行的正确性比省内存重要。

新增：
- 方向键 / Home / End / Ctrl+方向键（按词）
- Shift 扩展选区；**约定统一为**：`extend == false` 折叠选区，`extend == true`
  锚点不动
- 点击定位（**之前是写死的"移到末尾"**）、拖拽扩选、双击选词
- Ctrl+A / Ctrl+Z / Ctrl+Y（及 Ctrl+Shift+Z）
- 选中高亮画在字形**下面**，且**包含空格**（按字形跳过高亮中间会有缝）

撤销合并规则：只有**连续单字符输入**和**连续退格**会合并，且只在时间窗内。
`Delete` 永不合并（每次按键是独立一步）；**粘贴永不与前一个字符合并** ——
否则 Ctrl+Z 会把整段粘贴连同前面那个字一起撤掉。

宿主仍持有 `char*`：每帧 `sync_text_from_host()` 比对，宿主在框外改值时
Fox 让步，并且**清空历史**（那是新值，不是用户敲的）。写回时**截断而不是溢出** ——
调用方是个定长 C 数组，它不知道 Fox 有多长。

### `fox_text_test` —— 纯逻辑，不需要窗口/GPU/驱动

只链接 `text_buffer.cpp`。刻意做成最小的 target：**编辑模型的难点与渲染无关**，
而通过渲染器测它等于让一个坏掉的 GPU 能让纯逻辑测试失败。

68 条断言，覆盖：码位边界、移动、退格/删除的码位感知、畸形字节的终止性、
选区前后方向、撤销合并/不合并、撤销恢复选区、`set_text` 清历史、历史上限。

## 本轮结论

**"跑一下看看"对编辑模型无效。** 40 号的两个症状——退格没反应、Ctrl+← 挂死——
都需要用户恰好注意到才可能发现。纯逻辑就该有纯逻辑的单元测试，而且这个 target
只花几秒钟构建。

**为一个"绕过去"的修补写注释之前，先确认那个读是合法的。** 42 号里
`want[0] == '\0'` 看起来是最天经地义的判空；它触发的是工具链对零长度字面量的
表示问题。修法不是"在比较里特判"，而是别再用那个字面量。

**ASan 抓到的是我自己的测试工具，而不是被测代码。** 41 号的悬垂临时在普通构建下
让 68 条断言全绿。这类缺陷的隐蔽程度和被测代码里的同类缺陷**完全一样** ——
所以工具本身也要被工具审。

### 剪贴板（第 2 步）

**为什么不用 GLFW 的剪贴板。** `glfwGetClipboardString` / `glfwSetClipboardString`
是最顺手的答案，也是错的答案：它们要求 `glfwInit()` 成功，在若干平台上还要求一个
活的窗口上下文 —— 而 CPU 路径**刻意**不要这两样。剪贴板必须在"没有窗口"的地方也能用：
headless、CI 容器、以及 QuarkOS。所以这是一个真的接缝，平台代码按系统各一个小文件。

`core/platform.hpp` 这个接口**故意只有一个能力**。每加一个"以防万一"的虚函数，
QuarkOS 构建就多一个必须先实现才能链接的东西 —— 这正是接口不膨胀的原因。

Win32 实现里两个真正重要的点：

- **`OpenClipboard` 会因为别的进程正开着剪贴板而失败**，这是常态且是瞬时的
  （文件管理器正在复制、同一个应用的另一个实例）。把第一次失败当致命错误，
  会让粘贴**间歇性**失败 —— 这是最糟的失效模式，因为它看起来像"Fox 坏了"，
  而不是"操作系统正忙"。所以重试 8 次。
- **UTF-16 → UTF-8 的测量不能用 `-1`。** 传 `-1` 时返回的字节数**包含结尾 NUL**，
  于是"分配 n-1 字节、再要求写入 n 字节"就是一次**堆上字符串的 1 字节溢出**。
  这个是我写出来、被自己读回去发现的。

`text_field` 现在支持 Ctrl+C / Ctrl+X / Ctrl+V：

- **剪切先写剪贴板、成功后才删除**。反过来做的话，剪贴板不可用时文本就丢了 ——
  而那恰恰是用户最不希望丢的时候。
- 粘贴**先按调用方的定长容量截断再插入**，这样写回时不会溢出宿主的数组。
- 平台为 `null`（QuarkOS 现状）时：复制剪切无处可去、粘贴什么也不做 ——
  都是正确的降级，**不允许**剪贴板不可用导致输入框行为异常。

C ABI：`fox_clipboard_text(out, cap)` 返回**不含 NUL** 的字节数，
所以调用方能用 `result == cap - 1` 判断截断；`fox_clipboard_set_text(utf8)`。
**故意不带 context 句柄** —— 剪贴板属于应用而不是窗口，同一进程的多个 context
必须看到同一个剪贴板；顺带也意味着它可以在 context 存在之前调用。

### 43. **剪贴板截断会劈开多字节码位**（本轮）
C ABI 的调用方给的是定长缓冲区。朴素的按字节截断会落在码位中间，
留下一个悬空的 lead/continuation 字节 —— 调用方**无法区分**它和真正的数据损坏。
现在截断回退到 lead 字节，把整个码位丢掉。
由 `fox_c_test` 断言：`"abc" + 3 字节 CJK + "def"` 读进 `cap = 6` 必须得到 `"abc"`。

### 44. **我的测试会截断用户的剪贴板**（本轮，自己发现的）
剪贴板往返测试把原内容存进 4096 字节再恢复。如果用户剪贴板**比这更大**，
恢复就会用一份截断副本**替换掉**它的内容 —— 一个为了检查 6 字节缓冲区边界
而销毁用户数据的测试。
现在：先读，`saved_len + 1 >= sizeof saved` 就**一个字节都不写**直接跳过。
跳过严格优于测试：截断规则已经由下面那个小缓冲区用例覆盖了，而它根本不需要保存。

（顺带修掉了一个坏掉的 `build_fox_asan.bat`：反复对它做正则手术，**静默删掉了**
`fox.cpp` 和 `text_buffer.cpp` 的编译行，症状是链接期报未定义符号、
而代码本身完全正常。用正则编辑生成出来的构建文件，是一个会对你撒谎的构建文件。
已整份重写。）

## 本轮结论

**"平台接口"存在的意义是让"没有这个东西"成为一个可判断的值。**
非 Windows 上 `make_default_platform()` 返回 `null` 而不是"一个会失败的桩"，
于是"不支持"在任何调用点都能测出来，剪贴板在 QuarkOS 上不需要任何特判。

**截断规则要测的是"不劈开码位"，不是"长度对得上"。** 长度对得上和一串合法 UTF-8
是两件事，而只有后者能在别人机器上暴露问题。

**测试自己也会销毁用户数据。** 44 号和 41 号是同一类错误的两个变体：
**先确认你的验证手段本身是安全的**，再确认它有没有测到东西。

45. **`FontManager::measure()` 只有声明没有定义**（本轮，链接期才暴露）
    头文件里声明了 `f32 measure(FontId, const char*)`，`.cpp` 里从来没有定义。
    它一直没被发现，是因为**没有任何东西调用它** —— 未定义的成员函数在有人引用之前
    不是错误，于是那行声明看起来完全正常，而实际上什么都不测。
    字体回退让 `shape_text` 第一次需要它，链接器立刻报了出来。
    这提醒的是：**声明不是实现**，而"看起来没问题"和"确实工作"是两件事。

### 字体回退链（本轮）

先纠正一条**过期结论**：README 之前写着"CJK 渲染错误字形"。**那是错的。**
CJK 一直渲染正确 —— 因为 `load_system_ui()` 刻意优先挑选**有 CJK 覆盖**的主字体
（YaHei / Noto CJK / PingFang），Latin 和 CJK 由同一个 face 覆盖，根本用不到回退。

真正缺的是另一件事：**应用必须能选一个 Latin 优先的主字体**（macOS 27 那个观感
本来就需要），而且 CJK 仍然要能显示。

### ⚠ 一个从未被执行过的机制

加完回退链之后，**它一次都没有触发过** —— 主字体自己什么都覆盖，`resolve()` 根本
走不到 fallback。"回退能用"是一句**没有任何测试验证过的话**。
一个从未运行过的机制和一个已知坏掉的机制，在你的信心层面是同一种东西。

所以新增 `Context::set_font_file()`（同时也是一个真实功能：应用可以自带字体），
`fox_headless` 加 `latin` 模式把主字体强制成 Segoe UI（无 CJK）。
这时回退链**必须**干活，并且肉眼可见：一句 "Fox 图形框架" 里，
"Fox" 用 Segoe 渲染、"图形框架" 用 YaHei 渲染 —— **串中间的 face 切换**。

**要证明一个机制能工作，先制造一个它非工作不可的场景。**

实现要点：

- `has_glyph()` 用 `stbtt_FindGlyphIndex(...) != 0`。**索引 0 按定义就是
  `.notdef`** —— 字体被要求为"没有的码位"画的那个字形，所以 `!= 0` 是精确的
  存在性判断，不是近似。
  （我第一版注释恰好说反了，还去引用本仓库里**根本不存在**的
  `stbtt_FindGlyphInfo`。一个自信但错误的注释比没有注释更贵。）
- `shape_text()` 现在**按解析出的 face 分段发出多个 Text 命令**。
  下游**不需要任何新机制**：`Cmd` 本来就带 `(font, glyph_start, glyph_count)`，
  而 `build_instances` 本来就按 `(font, tex)` 的**连续段**分批 ——
  于是现有分批器恰好在正确的位置切开了，并且每个批次绑定了正确的图集。
  **这就是为什么分批要按"段"而不是"先所有形状后所有文字"**：混合文字没有别的做法。
- 回退链**排除主字体自身**，否则在主字体就是 msyh.ttc 的机器上会白建一份
  Entry 和一份图集（文件字节是共享的，但图集不是）。
- `measure()` 走**和 `shape_text` 完全相同的解析链**。用主字体测量、用回退体绘制，
  会让 label 报出错误的宽度，而症状（文字放不进自己的 rect）会指向布局，
  而不是指向测量。

### 验证

`fox_headless out.ppm none latin` 强制 Latin-only 主字体；
默认路径下回退链保持**完全惰性**（默认渲染与之前逐字节一致）。
normal + ASan + CMake 全绿。

### IME 组合串（本轮）

**preedit 不在文档里。** 这是全部设计的要点。IME 还在**合成中**的文本
（拼音字母、假名读音、候选预览）用户**尚未确认**，所以它不属于文档：
文档不能含有它，而且在确认之前**不允许存在任何撤销条目**。

所以它是 `TextBuffer` 里**独立的状态**，有自己的光标，由**控件**把它贴到显示上，
而不是被拼进 `text_` 里。正因为不拼进去，"Ctrl+Z 撤销一整个已确认候选、
不多也不少"才是**真的**而不是"差不多"。

- `composition_begin / update(utf8, caret)`：组合串开始 / 变化。
  `caret` 是**在组合串内部**的字节偏移，**在 buffer 里被 clamp** ——
  宿主是按它自己对组合串的理解算这个偏移的，盲信它就是越界读。
- `composition_commit()`：**一整步撤销**，并且恢复被替换的选区。
- `composition_cancel()`：什么都不插入，**不留撤销条目** ——
  没发生过的事，用户没什么可撤销的。
- **任何其他编辑（输入、移动、撤销、重做、退格）都会取消组合串。**
  在 9 个调用点统一处理，而不是每个方法自己决定 ——
  否则总会有某个方法悄悄吃掉半个已确认的候选。
- 组合期间 `text_field` **忽略 `chars`**：那些按键属于 IME。
  两边都处理，就是"打了 nihao，提交了你好，后面还多出一串 nihao"。

**渲染**：preedit 作为**独立的一段**画在光标前面的文本之后，而不是拼进重建的字符串。
拼进去看起来更简单，其实是个坑：光标之后**每一个字节偏移都会平移**，
于是选区高亮、光标位置、命中测试全都要按 preedit 长度重新基准化 ——
三个可能出错的地方，而且症状都是"选区好像偏了一点点"。
三次独立绘制没有任何偏移需要重算，光标位置一路累加即可。

### 46. **IME 路由用错了 Context**（本轮，C ABI 测试抓到的）

我最初把 IME 入口写成**自由函数**，内部调用全局 `ctx()`。但 Fox 一个进程里有
**两个 Context**：C++ demo / headless 用的**全局单例**，以及每个 `fox_ctx` 持有的
**自己的那个**。于是 IME 入口把组合串路由进了一个**完全不同的、从未初始化的**
Context。

症状是 `fox_ime_begin` **什么都不做，且没有任何报错**。headless 用的是全局单例，
所以那条路径是好的 —— 只有 C ABI 测试能发现。

修法不是"小心一点"，而是**承认这个操作属于 Context**：
`ime_begin/update/commit/cancel/ime_composing` 现在是 **Context 的成员函数**，
C ABI 传 `ctx`。顺带得到一个对称的结论：
**剪贴板入口不传 context 是对的**（它真的是全局状态），**IME 入口必须传**。
两者差异都是刻意的，方向相反。

### 47. **C ABI 完全没有 focus 接口**（本轮，顺手补上）

`fox.h` 里一个 focus 函数都没有。宿主想自动聚焦搜索框做不到 ——
而 IME 入口按定义就是"路由到聚焦的控件"，没有 focus 就**无法被触发**。
新增 `fox_focus / fox_focused_id / fox_has_focus`；
`fox_focus(ctx, 0)` 顺带取消组合串（组合串锚定在一个用户刚离开的控件的光标上）。

### 刻意**没有**写的部分

**Windows TSF / IMM32 后端没有写。**

理由不是工作量，是**无法验证**：这个环境里没有激活的输入法，
也无法忠实模拟一个（组合串、候选窗口、按键拦截都是 OS 行为）。
写了但从未运行过的代码，从外部看和坏掉的代码**无法区分** ——
这正是上一轮字体回退那一课的延续。

Fox 这一侧的组合串支持是**完整的**：模型、preedit 渲染、撤销语义、
"组合期间忽略 chars"、C ABI 入口，全部工作，并且可被宿主合成驱动
（测试、远程 UI、回放都能），整条路径端到端跑通。
缺的是**聆听真实输入法的那一块**。

### 验证

`fox_text_test` 新增 7 组 / 94 条断言（组合串不进文档、
提交是一步撤销、提交替换选区是一步、取消不留历史、
其他编辑取消组合、重新开始替换组合、preedit 光标 clamp 到**字节**）。
`fox_c_test` 通过 **C ABI** 跑 begin/update/cancel，
外加"没有焦点时必须是 no-op"（用户开始组合、切到别的窗口、IME 仍然提交 ——
这是真实序列，不是假设）。

`fox_headless out.ppm ime` 输出一张肉眼可验证的图：
**"nihao" 带蓝色下划线画在已提交文本之后，光标位于组合串内部**。
下划线是"组合中"与"已提交"之间**唯一的**区别，所以它不是装饰。

normal + ASan + CMake 全绿。

### 仍然没做

- **Windows TSF / IMM32**：见 IME 一节的说明，刻意未写。
- **QuarkOS 剪贴板**：接缝已就位，缺一个 `qcos` syscall 与实现。
- **候选窗口**：Fox 只画 preedit，不画候选列表 —— 候选窗口由 OS 拥有。
- **回退链只有 Windows / Linux / macOS 的候选路径**，没有通用枚举；
  一个自定义字体应用需要自己 `set_font_file()` 并保证回退面覆盖它缺的那些码位。
- 玻璃模糊的视觉质量；SDF 抗锯齿小尺寸偏硬（品质分级已接上，但"好看的模糊"
  需要 GPU 上的离屏链，见 `docs/gfx-acceleration.md` §6）；
- **PNG 与 mipmap**：解码、金字塔、三线性采样已落地（`image/png.cpp`，
  自写 inflate，不引 stb_image），`Context::create_texture_png` /
  `create_texture_mipped` / `sample_rgba_lod` 已接通 CPU 光栅器；
  **Vk 后端现已逐级上传全链**（`make_image` 层数、每级 `VkBufferImageCopy`、
  屏障/view 覆盖全链、sampler `mipmapMode=LINEAR` + `maxLod=16`）——
  此前的 DOCUMENTED GAP（只传第 0 级、强缩小下 GPU 比 CPU 更走样）已关闭。
  仍然缺的：**Adam7 交错**（明确报错拒绝，不静默解错）；
- `rvdl_cgui_*` → `fox_*` 已接线（`qhal/FoxHost.hpp` + 运行时的
  `src/ClassicRoute.hpp`）：fox 优先、失败回退 Nuklear，**每条回退原因都打印**，
  见 `docs/gfx-acceleration.md` §5。
- **复合控件已补齐**（`widget/composite.{hpp,cpp}`）：菜单栏/下拉、组合框、
  可选列表项、可滚动分组、数字属性行 —— 五个此前「demo 里手搓、
  qvm_visualizer 迁移时必然重写」的部件，行为由 `fox_widgets_test`
  （33 项，无窗口合成输入驱动）钉住。
- **text_area 软换行已落地**（`text/soft_wrap.hpp`）：词优先 + CJK 硬断，
  编辑模型仍只认 `\n`（换行是布局属性），命中/光标/导航/选区/滚动条全部
  按视觉行；算法不变量由 `fox_wrap_test`（24 项）钉住。

### 经验

**驱动层崩溃或静默失败，先上校验层，不要猜。** 但校验层只管**参数合法性**，
管不了两件事：参数合法但用错了位置（14、15 号），以及 **host 侧的内存越界** ——
后者校验层一个字都不会说，因为它根本不在 GPU 这一侧。

**上一轮我判"没有调试器、也没装 ASan，所以查不下去"，其实那是不该接受的结论。**
ASan 只需要一个构建脚本，而它一分钟就把两个小时的猜测全部作废了：

- 我曾把责任归给"交换链 semaphore 复用"、"descriptor 角色错位"、"窗口与实例
  销毁顺序" —— 那些**可能都是对的**，但都不是这次崩溃的原因；
- 我甚至花了很多轮去二分"是不是图像绘制路径"，而真因是一个 4 倍的 `w*h` /
  `w*h*4` 笔误，位于 demo 里。

**教训：内存安全类的怀疑，应该先上工具再推理。** 靠读代码猜内存错误，猜错的代价比
装个 ASan 高两个数量级 —— 因为错误信息会直接告诉你**哪个对象、多大、从哪来**。

另外三条仍然成立：

1. **先确认你的观测手段本身有效。** 校验层输出没进重定向文件。
2. **小样本的实验结论不可信。** 4 次采样把"0/4"和"基线 25% 干净"混为一谈。
3. **需要干净退出才复现的缺陷，不能用交互测试判断有没有。** 顺序反了，浪费一整轮。

