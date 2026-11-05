#pragma once
/*
 * Fox - C API
 * =============
 * Fox 稳定的、可从 C 调用的接口。C++ API（core/、widget/、window/）是实现；
 * 这个头文件是其他语言绑定的目标。
 *
 * 为什么存在
 * ---------------
 * Fox 的目标是替代 Ravendrel 里的 Nuklear，而 rvdl 通过 C ABI 调用原生函数
 * （见 qhal/ClassicGui.hpp，目前是以 Nuklear 为底层的接口）。没有 C ABI，
 * rvdl 根本调不到 Fox，所以这不是可选的便利——它就是集成点。
 *
 * 分层：这才是库 API（`fox_*`，对应 Nuklear 的 `nk_*`）。项目面向 rvdl 的
 * 垫片（`rvdl_cgui_*`）是上面的一层薄翻译，放在项目里，不在这里。两者分开
 * 才能让任何东西复用 Fox，也和今天 Nuklear 的拆分方式完全一致。
 *
 * 约定
 * -----------
 *  * `bool` 写作 `int`（0 / 1）。C 没有保证存在的 bool。
 *  * 字符串是 UTF-8、NUL 结尾，只在调用期间借用——Fox 从不保存指针。
 *  * 出参是普通指针（`float *value`、`int *value`）。
 *  * 矩形是逻辑单位。Fox 内部只在一个地方转成设备像素，同一组数字在任何
 *    显示密度下都成立。
 *  * 每个调用都是立即模式：记入当前帧的显示列表后返回。调用方不保留任何东西。
 *  * 边界上没有 C++ 类型、引用、默认参数、异常。这个头文件必须能按纯 C99
 *    编译。
 */

#include <stddef.h> /* size_t——下面剪贴板/IME 字节长度调用用到。
                       必须显式引入：先包含本头文件的 C99 编译单元也得能编译。 */
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* ── 不透明句柄 ───────────────────────────────────────────────────── */

typedef struct fox_ctx fox_ctx;

/* fox_texture_create() 返回的纹理句柄。0 表示"无"。
   刻意与字体用不同的 id 空间：字形图集和图标图集都是"可采样的纹理"，
   但它们的上传和释放节奏完全不同。 */
typedef uint32_t fox_tex;

/* 控件标识。任意稳定的非零值；连续帧用同一个 id，控件的悬停/滚动/动画
   状态才能保留。fox_make_id() 用 salt 加字符串帮你推导一个。 */
typedef uint32_t fox_id;

/* ── 值类型 ──────────────────────────────────────────────────────── */

typedef struct fox_rect {
    float x, y, w, h;
} fox_rect;

typedef struct fox_color {
    uint8_t r, g, b, a;
} fox_color;

/* 纹理内的归一化子矩形：{u, v, du, dv}。 */
typedef struct fox_uv {
    float u, v, du, dv;
} fox_uv;

/* ── 枚举（取值与 C++ 枚举完全一致）────────────────────────────────── */

typedef enum {
    FOX_ALIGN_START = 0,
    FOX_ALIGN_CENTER = 1,
    FOX_ALIGN_END = 2,
    FOX_ALIGN_STRETCH = 3,
} fox_align;

typedef enum {
    FOX_BUTTON_PLAIN = 0,  /* 仅文字 */
    FOX_BUTTON_FILLED = 1, /* 强调色填充 */
    FOX_BUTTON_TINTED = 2, /* 浅强调色背景 */
    FOX_BUTTON_BEZEL = 3,  /* 经典 macOS 边框样式 */
} fox_button_style;

typedef enum {
    FOX_SLIDER_CONTINUOUS = 0,
    FOX_SLIDER_STEPPED = 1,
} fox_slider_style;

typedef enum {
    FOX_MOUSE_LEFT = 0,
    FOX_MOUSE_RIGHT = 1,
    FOX_MOUSE_MIDDLE = 2,
} fox_mouse_button;

typedef enum { FOX_APPEARANCE_LIGHT = 0, FOX_APPEARANCE_DARK = 1 } fox_appearance;

/* 具名主题色，宿主可以读 token，而不是硬编码十六进制值、渐渐偏离设计系统。 */
typedef enum {
    FOX_COLOR_WINDOW_BG = 0,
    FOX_COLOR_SIDEBAR_BG,
    FOX_COLOR_SIDEBAR_TEXT,
    FOX_COLOR_SIDEBAR_TEXT_DIM,
    FOX_COLOR_TEXT,
    FOX_COLOR_TEXT_SECONDARY,
    FOX_COLOR_TEXT_TERTIARY,
    FOX_COLOR_TEXT_ON_ACCENT,
    FOX_COLOR_CONTROL_BG,
    FOX_COLOR_CONTROL_BG_HOVER,
    FOX_COLOR_CONTROL_BG_ACTIVE,
    FOX_COLOR_CONTROL_BG_DISABLED,
    FOX_COLOR_CONTROL_BORDER,
    FOX_COLOR_SEPARATOR,
    FOX_COLOR_SELECTION,
    FOX_COLOR_ACCENT,
    FOX_COLOR_ACCENT_HOVER,
    FOX_COLOR_ACCENT_ACTIVE,
    FOX_COLOR_COUNT
} fox_color_token;

/* 具名主题度量，逻辑像素。宿主读这些值，就能和 Fox 用同一套栅格布局，
   而不是硬编码数字——设计系统一变数字就过时。 */
typedef enum {
    FOX_METRIC_CORNER_WINDOW = 0,
    FOX_METRIC_CORNER_PANEL,
    FOX_METRIC_CORNER_CONTROL,
    FOX_METRIC_CORNER_FIELD,
    FOX_METRIC_CONTROL_HEIGHT,
    FOX_METRIC_CONTROL_HEIGHT_SMALL,
    FOX_METRIC_CONTROL_HEIGHT_LARGE,
    FOX_METRIC_SIDEBAR_WIDTH,
    FOX_METRIC_TITLEBAR_HEIGHT,
    FOX_METRIC_ROW_HEIGHT,
    FOX_METRIC_SPACING_XS,
    FOX_METRIC_SPACING_SM,
    FOX_METRIC_SPACING_MD,
    FOX_METRIC_SPACING_LG,
    FOX_METRIC_HAIRLINE,
    FOX_METRIC_FONT_CAPTION,
    FOX_METRIC_FONT_SMALL,
    FOX_METRIC_FONT_BODY,
    FOX_METRIC_FONT_CALLOUT,
    FOX_METRIC_FONT_TITLE3,
    FOX_METRIC_FONT_TITLE2,
    FOX_METRIC_FONT_TITLE1,
    FOX_METRIC_COUNT
} fox_metric_token;

/* ── 生命周期 ────────────────────────────────────────────────────────── */

typedef struct fox_config {
    const char *title;  /* 可以为 NULL */
    int32_t width;      /* 逻辑像素 */
    int32_t height;     /* 逻辑像素 */
} fox_config;

/* 创建一个 CONTEXT。只有分配失败才返回 NULL；原因见 fox_last_error()。

   ⚠ 上下文没有窗口、没有 GPU：fox_has_window() 返回 0。这是合法且受支持
   的模式（文本度量、布局计算、离屏渲染）。用 fox_init_backend_vulkan()
   挂接窗口。

   旧说法（"失败返回 NULL；原因见 fox_error()"）根本做不到：返回 NULL 就
   没有上下文可传给 fox_error()。 */
fox_ctx *fox_create(const fox_config *cfg);
void fox_destroy(fox_ctx *ctx);

/* 给已有的 GLFW 窗口挂接 Vulkan 后端（裸指针，这样本头文件不用接触
   Vulkan 或 GLFW 类型）。

   ⚠ 拆除顺序：窗口必须在 fox_shutdown_backend() / fox_destroy() **之后**
     销毁。后端先销毁 VkSurfaceKHR（来自 glfwCreateWindowSurface——那个
     辅助函数把所有权交给应用）再销毁 VkInstance，Vulkan 要求这两者先
     消失。先销毁窗口会泄漏 surface；没有能同时满足两条的顺序，因为
     surface 就依附于窗口。

   后端刻意不放进 fox_create()：让宿主自己掌握 surface、设备和帧节奏，
   嵌入到更大的渲染器里正需要这样。 */
int fox_init_backend_vulkan(fox_ctx *ctx, void *glfw_window);
void fox_shutdown_backend(fox_ctx *ctx);
int fox_backend_ready(fox_ctx *ctx);

/* 宿主应当关闭窗口时非零：宿主请求了（fox_set_should_close），或窗口
   请求了（关闭按钮）。

   无头上下文（无窗口）恒为 FALSE："未初始化"和"用户想退出"是两回事。
   用 fox_has_window() 区分。 */
int fox_should_close(fox_ctx *ctx);

/* 挂接了窗口时非零。0 = 无头（仅离屏渲染 / 文本度量），这是合法且受支持
   的模式。 */
int fox_has_window(fox_ctx *ctx);

/* 请求 Fox 关闭：之后 fox_should_close() 一直报 1，直到调用
   fox_set_should_close(ctx, 0)。不清除 fox_error()。 */
void fox_set_should_close(fox_ctx *ctx, int value);

/* ── 帧 ────────────────────────────────────────────────────────────── */

/* 开始一帧：推进时间，清空显示列表和输入边沿。 */
void fox_begin_frame(fox_ctx *ctx);
/* 结束一帧：清除输入边沿（pressed/released/chars）和布局栈，然后转换
   显示列表并经后端提交。

   ⚠ 这是唯一清除输入边沿的地方。宿主若从不调用它，每次按键都会永远
   卡住：单次按下后 fox_key_pressed() 一直返回 1，布局栈无上限增长
   （每帧一个 Layout，约 80 字节 → 60fps 下约 17MB/小时）。 */
void fox_end_frame(fox_ctx *ctx);

/* 把宿主 OS 事件泵入上下文。在 fox_begin_frame() 之前调用。 */
void fox_poll_events(fox_ctx *ctx);

void fox_set_size(fox_ctx *ctx, int32_t w, int32_t h); /* 逻辑 */
int32_t fox_width(fox_ctx *ctx);                         /* 逻辑 */
int32_t fox_height(fox_ctx *ctx);                        /* 逻辑 */
float fox_time(fox_ctx *ctx);
float fox_dt(fox_ctx *ctx);

/* 每逻辑像素的物理像素数。Fox 只在一个地方用它乘逻辑几何；宿主必须继续
   传逻辑坐标。 */
float fox_scale(fox_ctx *ctx);

/* ── 主题 ────────────────────────────────────────────────────────────── */

/* 按给定外观应用 Golden Gate 默认主题。

   长度 token 是逻辑单位，这里不缩放：Fox 只有一个缩放点（后端），
   缩放两次会让 2x 显示屏上的 UI 大一倍。 */
void fox_set_theme(fox_ctx *ctx, fox_appearance appearance);

/* 设置强调色（0xRRGGBB）。替换强调色派生色（accent / hover / active /
   text-on-accent），主题其余部分不动。 */
void fox_set_accent(fox_ctx *ctx, uint32_t rgb); /* 0xRRGGBB */
fox_color fox_theme_color(fox_ctx *ctx, fox_color_token token);
float fox_theme_metric(fox_ctx *ctx, int32_t token);

/* ── 输入 ────────────────────────────────────────────────────────────── */

float fox_mouse_x(fox_ctx *ctx);
float fox_mouse_y(fox_ctx *ctx);
int fox_mouse_down(fox_ctx *ctx, fox_mouse_button button);
int fox_mouse_pressed(fox_ctx *ctx, fox_mouse_button button);
int fox_mouse_released(fox_ctx *ctx, fox_mouse_button button);
/* 当前点击序列中的点击数（1 = 单击，2 = 双击，……）。
   按键分开计：左键双击不能让右键看起来也像双击。 */
int fox_mouse_click_count(fox_ctx *ctx, fox_mouse_button button);
float fox_mouse_wheel(fox_ctx *ctx);

int fox_key_down(fox_ctx *ctx, int32_t key);
int fox_key_pressed(fox_ctx *ctx, int32_t key);
int fox_key_released(fox_ctx *ctx, int32_t key);
int fox_shift(fox_ctx *ctx);
int fox_ctrl(fox_ctx *ctx);
int fox_alt(fox_ctx *ctx);
int fox_super(fox_ctx *ctx);

/* 可打印 ASCII 的便捷写法：fox_key_down(ctx, 'a')。 */
int fox_key_ascii_down(fox_ctx *ctx, int32_t ascii);
int fox_key_ascii_pressed(fox_ctx *ctx, int32_t ascii);

/* 具名按键槽位（fox::Key）。ASCII 字符 0..127 用自身值；具名键从 0x100
   开始。 */
typedef enum {
    FOX_KEY_NONE = 0,
    FOX_KEY_BACKSPACE = 0x100,
    FOX_KEY_TAB,
    FOX_KEY_ENTER,
    FOX_KEY_ESCAPE,
    FOX_KEY_SPACE = 0x20,
    FOX_KEY_DELETE,
    FOX_KEY_UP,
    FOX_KEY_DOWN,
    FOX_KEY_LEFT,
    FOX_KEY_RIGHT,
    FOX_KEY_HOME,
    FOX_KEY_END,
    FOX_KEY_PAGE_UP,
    FOX_KEY_PAGE_DOWN,
    FOX_KEY_SHIFT,
    FOX_KEY_CONTROL,
    FOX_KEY_ALT,
    FOX_KEY_SUPER,
    FOX_KEY_F1, FOX_KEY_F2,  FOX_KEY_F3,  FOX_KEY_F4,  FOX_KEY_F5,  FOX_KEY_F6,
    FOX_KEY_F7, FOX_KEY_F8,  FOX_KEY_F9,  FOX_KEY_F10, FOX_KEY_F11, FOX_KEY_F12,
} fox_key;

/* ── 控件 ──────────────────────────────────────────────────────────── */

/* 每个控件立即绘制并返回是否被交互。`id` 把控件绑定到保留状态；每帧传
   同一个 id。 */

float fox_label(fox_ctx *ctx, fox_rect r, const char *text, fox_color color,
                fox_align align);

int fox_button(fox_ctx *ctx, fox_rect r, const char *label,
               fox_button_style style, int enabled);

int fox_checkbox(fox_ctx *ctx, fox_rect r, const char *label, int *value);
int fox_toggle_switch(fox_ctx *ctx, fox_rect r, int *value);

/* 返回时 `value` 被截断到 [min,max]。拖动中返回非零。 */
int fox_slider(fox_ctx *ctx, fox_rect r, float *value, float min, float max,
               fox_slider_style style, float step);

/* 单行可编辑字段。`buffer` 必须有 `capacity` 字节空间，含结尾符。
   内容变化时返回非零。`id` 固定该字段的标识；传 0 则从 `r` 推导。 */
int fox_text_field(fox_ctx *ctx, fox_rect r, char *buffer, int32_t capacity,
                   const char *placeholder, fox_id id);

/* 用与 fox_text_field 的 `id` 相同方式标识的字段的光标位置（字段 buffer
   内的字节偏移）。 */
int32_t fox_text_field_caret(fox_ctx *ctx, fox_id id);

void fox_separator(fox_ctx *ctx, fox_rect r, int horizontal);
void fox_panel(fox_ctx *ctx, fox_rect r, fox_color fill, float radius, int glass);
void fox_glass_bar(fox_ctx *ctx, fox_rect r, float radius);

/* ── 组合控件 ───────────────────────────────────────────────────────────── */

/* 菜单栏标题。点击切换其下拉层；被点击的那一帧返回非零。每个标题每帧调
   一次；开/关状态存在上下文里（同时最多打开一个弹出层）。 */
int fox_menu_title(fox_ctx *ctx, fox_rect r, const char *label);

/* 标题矩形为 `anchor` 的菜单的下拉层。只在该菜单打开时绘制；返回供
   fox_menu_item 使用的内容矩形（关闭时全零）。`items_csv` 逗号分隔
   （"New,Open,Save"）。 */
fox_rect fox_menu_popup(fox_ctx *ctx, fox_rect anchor, const char *items_csv,
                        float item_height);

/* 打开的下拉层里的一个条目。被点击返回非零（菜单随后自动关闭），否则 0。
   每行的下标调用方自己知道——行是它排的——所以不返回下标。 */
int fox_menu_item(fox_ctx *ctx, fox_rect r, const char *label, int enabled);

/* 关闭所有打开的菜单（如窗口失去焦点时）。 */
void fox_menu_close_all(fox_ctx *ctx);

/* 下拉选择框：显示 items[selected] 和一个箭头；点击展开列表。返回新选中
   的下标，未变返回 -1。`selected` 被截断到范围内并就地更新。 */
int fox_combo(fox_ctx *ctx, fox_rect r, const char *items_csv, int *selected);

/* 可选列表行：选中时高亮；点击选中它。被点击返回非零；`selected` 就地
   更新。 */
int fox_selectable(fox_ctx *ctx, fox_rect r, const char *label, int *selected);

/* 可滚动组：边框 + 标题 + 裁剪 + 滚动。返回内容矩形（滚动偏移已计入）；
   往里画条目，然后调 fox_group_end。必须配对。`id` 固定滚动状态，组挪了
   位置也能保持偏移；`content_height` 是完整内容高度。 */
fox_rect fox_group_begin(fox_ctx *ctx, fox_rect r, const char *title,
                         fox_id id, float content_height);
void fox_group_end(fox_ctx *ctx);

/* 数字属性行：标签 + 可拖拽的数值框（水平拖动改值；聚焦时上下方向键
   微调）。值变化返回非零；`value` 就地更新。 */
int fox_property_int(fox_ctx *ctx, fox_rect r, const char *label, int32_t *value,
                     int32_t min, int32_t max, int32_t step);
int fox_property_float(fox_ctx *ctx, fox_rect r, const char *label, float *value,
                       float min, float max, float step);

/* ── 底层图元 ───────────────────────────────────────────────────────────── */

/* 上面的控件覆盖 UI；这些覆盖其余一切——游戏 HUD、图表、粒子系统——用的
   是同一个显示列表，所以排序、裁剪、损坏跟踪、批处理都和控件完全一样。
   坐标是逻辑像素；颜色带直通 alpha，a < 255 的颜色会与已画在下面的内容
   混合。

   与控件不同，这些接口收矩形，因为"画在哪"由调用方决定：用底层图元的
   场景恰恰是布局碍事的地方。 */

/* 填充矩形，直角。 */
void fox_fill_rect(fox_ctx *ctx, fox_rect r, fox_color c);

/* ── 裁剪 ───────────────────────────────────────────────────────────── */

/* 压入裁剪矩形；到配对的 pop 之前画的一切都限制在它里面。必须配对
   （push/pop），像栈一样——多出的 push 会把本帧其余部分都裁掉，多出的
   pop 是空操作而不是崩溃，这会让 bug 一直藏着，直到布局碰巧"看起来
   还行"。

   这是给手工排版的宿主用的：上面的控件自己裁剪（group_begin、
   window_begin），自己画行的宿主也需要同样的保证。 */
void fox_push_clip(fox_ctx *ctx, fox_rect r);
void fox_clip_pop(fox_ctx *ctx);

/* ── 文本度量 ───────────────────────────────────────────────────────── */

/* 按任意像素大小度量（字面按需加载/缓存，宿主不限于主题用到的字号）。
   都接受 NUL 结尾的 UTF-8 字符串；非法序列按 U+FFFD 度量，与渲染一致
   ——度量和绘制不一致会是个坑。 */
float fox_text_width(fox_ctx *ctx, int32_t px, const char *text);
float fox_text_height(fox_ctx *ctx, int32_t px);            /* 行高 */
float fox_text_offset_x(fox_ctx *ctx, int32_t px, const char *text,
                        int32_t byte_index);                /* 文本内光标 x */

/* `text` 按 `width` 折行后占多少个视觉行（优先按词断、CJK 硬断——与
   text_area 用的算法相同，宿主的行数计算和 Fox 一致）。显式 '\n' 总是
   另起一行。 */
int32_t fox_text_lines(fox_ctx *ctx, int32_t width, int32_t px, const char *text);

/* `px` 像素大小的字面当前已光栅化的字形数。随文本绘制惰性增长；0 表示
   从没请求过字形（或该字号没有字面）。宿主的"字体烘焙了吗"自检应该用
   这个，而不是硬编码的 1/0——那只能报"上下文存在"，永远分不清"烘了
   2000 个字形"和"什么都没烘"。 */
int32_t fox_glyph_count(fox_ctx *ctx, int32_t px);

/* ── 键入字符 ───────────────────────────────────────────────────────── */

/* 上一帧以来键入的码点，最旧的在前。队列由 fox_char() 取空；
   fox_char_pending() 是剩余数量。边沿（和这个队列）由 fox_end_frame()
   清空，宿主要在 begin 和 end 之间轮询。 */
int32_t fox_char_pending(fox_ctx *ctx);
int32_t fox_char(fox_ctx *ctx);

/* 内切于 `r` 的填充椭圆（中心 = r 的中心，半径 = r.w/2、r.h/2）。
   任一轴小于 1x1 像素的矩形不画——与 Nuklear 的 nk_fill_circle 规则一致，
   两个后端对"太小"的判定相同。 */
void fox_fill_ellipse(fox_ctx *ctx, fox_rect r, fox_color c);

/* 填充三角形。退化三角形（面积为零）不画。 */
void fox_fill_triangle(fox_ctx *ctx, float x0, float y0, float x1, float y1,
                       float x2, float y2, fox_color c);

/* 描边折线：`xy` 是 2*count 个 float，(x0,y0,x1,y1,...)。`cap` 是
   0=butt, 1=round, 2=square；`join` 是 0=miter, 1=round, 2=bevel。
   count < 2（没东西可描）或分配失败时返回 0。 */
int fox_path(fox_ctx *ctx, const float *xy, int32_t count, float thickness,
             fox_color c, int cap, int join);

/* ── 纹理 ─────────────────────────────────────────────────────────── */

/* 上传 RGBA8 像素（w*h*4 字节，直通 alpha，从左上角起行优先）。Fox 会
   复制像素，调用方保留自己 buffer 的所有权。 */
fox_tex fox_texture_create(fox_ctx *ctx, int32_t w, int32_t h, const uint8_t *rgba);
int fox_texture_update(fox_ctx *ctx, fox_tex tex, int32_t w, int32_t h,
                       const uint8_t *rgba);
void fox_texture_destroy(fox_ctx *ctx, fox_tex tex);

/* 解码 PNG 文件镜像（文件的完整字节范围）并作为纹理上传，构建用于缩小
   绘制的 mip 金字塔。这是 fox 自己的解码器，不是 stb_image：纹理路径不
   引入任何第三方代码。任何解码错误都打印原因并返回 0——不会返回一块
   空白纹理。

   支持：颜色类型 gray / RGB / palette / gray+alpha / RGBA，位深 1..16，
   行滤波 0..4，带 CRC 校验的块。不支持（明确报错）：Adam7 隔行。 */
fox_tex fox_texture_create_png(fox_ctx *ctx, const void *bytes, size_t len);

/* 绘制整个纹理，拉伸到 `r` 并乘以 `tint`。白色则原样显示纹理。 */
int fox_image(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_color tint);
/* 绘制图标图集的一格，N 个图标只需一个纹理、一个描述符集，而不是各 N 个。 */
int fox_image_region(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_uv src,
                     fox_color tint);

/* ── 窗口装饰 ────────────────────────────────────────────────────────── */

/* macOS 风格窗口边框：圆角主体、红绿灯按钮、可选的鲜艳侧栏、玻璃标题栏。

   画了窗口框架返回 1。它不报告关闭按钮——见 fox_window_close_requested()。 */
int fox_window_begin(fox_ctx *ctx, fox_rect bounds, const char *title,
                     int show_sidebar);

/* 当前帧按下关闭按钮则非零。

   在 fox_window_begin() 之后立刻读取：该标志在每个窗口帧开始时清零，
   点击关闭灯时置位。 */
int fox_window_close_requested(fox_ctx *ctx);
fox_rect fox_window_content(fox_ctx *ctx);
fox_rect fox_window_sidebar(fox_ctx *ctx);
fox_rect fox_window_titlebar(fox_ctx *ctx);

typedef struct fox_sidebar_item {
    const char *label;
    fox_tex icon_tex; /* 0 表示没有图片图标 */
    fox_uv icon_src;
    const char *icon; /* 单字形回退，或 NULL */
} fox_sidebar_item;

/* 返回被点击的行下标，没有则 -1。 */
int32_t fox_sidebar(fox_ctx *ctx, fox_rect r, const fox_sidebar_item *items,
                    int32_t count, int32_t selected);

/* ── Id ──────────────────────────────────────────────────────────────── */

/* 由 salt 和字符串生成稳定 id，调用方不用自己写哈希。`salt` 对每个控件
   槽位必须唯一；label 标识它。 */
fox_id fox_make_id(uint32_t salt, const char *label);
fox_id fox_make_id_i(uint32_t salt, int32_t n);

/* 按位置作 key 的控件的 id——与 Fox 内部用的公式相同
   （make_id(salt, id_hash_rect(rect))）。宿主需要它自行推导位置型控件的
   id（比如查询其光标）。

   ⚠ 焦点文档引用过这个符号，但库里根本没有它；照文档写的宿主链接不过。 */
fox_id fox_make_id_rect(uint32_t salt, fox_rect r);

/* ── 诊断 ──────────────────────────────────────────────────────────────── */

/* 最近一次失败的人类可读原因。不会是 NULL。 */
const char *fox_error(fox_ctx *ctx);

/* 最近一次失败的人类可读原因，不需要上下文。不会是 NULL。这是查明
   fox_create() 为何失败的唯一途径（它返回 NULL，没有上下文可问）。 */
const char *fox_last_error(void);

/* 创建上下文时给的标题。不会是 NULL。 */
const char *fox_window_title(fox_ctx *ctx);

/* -- 剪贴板 --------------------------------------------------------
 *
 * Fox 里唯一需要真实平台服务的部分，刻意与上下文分开：剪贴板属于应用，
 * 不属于某个窗口，一个进程里的两个上下文必须看到同一个剪贴板。所以没有
 * 上下文句柄——也因此这些函数在上下文存在前后都能调用。
 *
 * 返回写入 `out` 的字节数，不含 NUL，调用方可以检测截断（结果 == cap - 1）。
 * 截断从不切开多字节码点：末尾不完整的字符会被整个丢掉。
 *
 * 没有剪贴板的目标平台（目前是 QuarkOS）报空字符串，写入也不报错。两者
 * 都不算错误，也不值得让文本编辑失败。
 */
size_t fox_clipboard_text(char *out, size_t cap);
int fox_clipboard_set_text(const char *utf8);

/* -- IME ----------------------------------------------------------------
 *
 * Fox 的输入组合支持是完整的，只是没有驱动：模型、预编辑渲染、撤销语义
 * 都能用，宿主通过这些入口把输入组合喂进来。还缺的是监听真实 IME 的部分
 * （通过窗口 HWND 的 Windows TSF，或旧式 IMM32）。
 *
 * 这部分刻意留白，不盲写：无头环境里没法忠实模拟 IME，从没跑过的代码和
 * 不能用的代码从外部看没有区别。在那个后端出现之前，宿主可以合成输入
 * 组合——测试、远程 UI、回放——整条链路都能端到端跑通。
 *
 *   fox_ime_begin(ctx, utf8, caret)   IME 打开了输入组合
 *   fox_ime_update(ctx, utf8, caret)  合成文本变化
 *   fox_ime_commit(ctx)               用户选定候选 -> 成为一步撤销
 *   fox_ime_cancel(ctx)               放弃；不插入任何内容
 *   fox_ime_is_composing(ctx)         是否有打开的输入组合？
 *
 * `caret` 是 `utf8` 内部的字节偏移（不是文档里的），会被截断，所以过期的
 * 偏移无害，不会越界读。
 *
 * 所有这些都路由到该上下文拥有焦点的字段，没有焦点时什么都不做——这是
 * 常态，不是错误：用户开始输入组合、点了别的窗口，IME 还是会发 commit。
 *
 * 这些函数带上下文；fox_clipboard_* 不带，两个方向的差异都是刻意的。
 * Fox 进程里有两个 Context——全局单例，加上每个 fox_ctx 各一个——IME 入口
 * 若去拿全局的，会把输入组合送进完全另一个、在 C ABI 情况下还未初始化的
 * Context。剪贴板正相反：它真的是全局状态，所以那两个不带上下文是对的。
 */
void fox_ime_begin(fox_ctx *ctx, const char *utf8, size_t caret_bytes);
void fox_ime_update(fox_ctx *ctx, const char *utf8, size_t caret_bytes);
void fox_ime_commit(fox_ctx *ctx);
void fox_ime_cancel(fox_ctx *ctx);
int fox_ime_is_composing(fox_ctx *ctx);

/* -- 焦点 --------------------------------------------------------------
 *
 * id 就是控件自己推导的那个：传给 fox_text_field() 的提示串用
 * fox_make_id_i()，按位置作 key 的控件用 fox_make_id_rect()。见上面 id 的
 * 说明——它们必须跨帧稳定。
 *
 * fox_focus(ctx, 0) 清除焦点，这是宿主侧强制收起输入组合的办法
 * （fox_ime_cancel 是礼貌版本；这个是用户点走时干的）。它会自动取消任何
 * 输入组合，因为输入组合锚定在用户刚离开的那个字段的光标上。
 *
 * 这些函数存在是因为宿主确实需要——打开时自动聚焦搜索框是最明显的场景
 * ——也因为 IME 入口是按"拥有焦点的字段"定义的，没有它们就够不着。 */
void fox_focus(fox_ctx *ctx, fox_id id);
fox_id fox_focused_id(fox_ctx *ctx);
int fox_has_focus(fox_ctx *ctx, fox_id id);

/* 刚提交的那一帧的计数。 */
uint32_t fox_last_instance_count(fox_ctx *ctx);
int fox_used_blur_path(fox_ctx *ctx);

/* ── 帧健康度 ─────────────────────────────────────────────────────── */

/* 累计没能上屏的帧数：acquire、submit 或 present 返回了真实错误，或交换链
   重建失败。没提交的帧就是用户没看到的帧，宿主若看不出来就会一直往虚空里
   画——这正是 rvdl 宿主必须在运行时检测、而不能假设不会发生的故障。 */
uint32_t fox_frame_faults(fox_ctx *ctx);

/* 最近一次故障帧的失败原因。首次故障前为 ""，宿主可以无条件记录。
   不会是 NULL。 */
const char *fox_last_fault(fox_ctx *ctx);

/* 上一帧因实例缓冲空间不足丢掉的实例数。不算故障——帧上屏了，只是少了
   些图形——但和 0 区分开是同样的理由：宿主应该能直接看到缺口，而不是
   靠推断。 */
uint32_t fox_last_frame_dropped(fox_ctx *ctx);

/* ── 宿主策略输入 ───────────────────────────────────────────────────────── */

/* 驱动报告的 GPU 标识（VkPhysicalDeviceProperties）。
   fox_init_backend_vulkan() 成功前为 "" / 0。宿主用它保存逐 GPU 策略
   （见 utils/A2a.hpp）；绝不从操作系统猜。 */
const char *fox_gpu_name(fox_ctx *ctx);
uint32_t fox_gpu_vendor(fox_ctx *ctx);

/* 热压力下的质量旋钮。分级而不是布尔：降级是分级的（全 / 半 / 关），
   "半"是真实存在的中间状态。

     blur       0 = 全分辨率下采样，1 = 半分辨率，2 = 关
                （关 = 玻璃不再采样背景；面板本身照画，丢了面板就丢了信息）
     shadows    0 = 全，1 = 半径减半，2 = 关（不生成阴影实例）
     animations 1 = 正常，0 = 插值直接跳到目标值

   fox 不读温度、不做决策——宿主做，然后把结果传下来。fox_get_quality
   读回当前生效值；出参指针为 NULL 则忽略。 */
void fox_set_quality(fox_ctx *ctx, int blur, int shadows, int animations);
void fox_get_quality(fox_ctx *ctx, int *blur, int *shadows, int *animations);

/* 把上屏图像的 GPU 侧回读以二进制 PPM 写到 `path`，不暗示退出（宿主继续
   运行）。成功返回非零。这是"GPU 真的画了这个吗"的真值依据，窗口截图
   答不了——它可能被遮挡、在缩放中途截取，或显示的是过期的合成器表面。 */
int fox_dump_ppm(fox_ctx *ctx, const char *path);

#if defined(__cplusplus)
} /* extern "C" */
#endif
