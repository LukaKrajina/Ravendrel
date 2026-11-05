// Fox fox.cpp - C API 实现
//
// 这里的每个函数都是 fox.h 的 C 类型与 core/ 的 C++ 类型之间的薄层、无异常
// 转换。刻意不含逻辑：C++ API 是行为的唯一事实来源，bug 不可能只存在于
// 一个接口而不存在于另一个。
//
// 本文件无条件遵守三条规则：
//
//  1. C++ 异常不得跨入 C。fox_create / fox_destroy 有包装；控件调用只可能
//     因分配抛异常，而整个进程宁可在明显的地方挂掉，也不要穿越 C 栈帧展开。
//
//  2. 结构体逐字段转换，绝不 reinterpret_cast。对 sizeof 的 static_assert
//     能抓到大小变化，但抓不到字段重排或符号性不匹配——实际发生的恰恰是
//     这类失败。这种转换零成本且不会错。
//
//  3. 除非 C++ API 明说，否则不在比调用活得久的 C++ 类型里缓存任何东西。
//     唯一的常驻新增是后端和窗口句柄，因为它们的生命周期确实长于一帧。

#include "fox.h"

#include "core/context.hpp"
#include "text/soft_wrap.hpp"
#include "backend/vk_backend.hpp"
#include "widget/primitives.hpp"
#include "widget/composite.hpp"
#include "window/window.hpp"

#include <exception>
#include <new>
#include <string>
#include <vector>
#include <cstdio>
// strlen，剪贴板读回要用。以前能编译只是因为别的头文件碰巧把它带了进来；
// 显式包含才让本文件不依赖包含顺序（clang -fsyntax-only 验证过）。
#include <cstring>

using namespace fox;

namespace
{
    /// 进程级「最近一次失败原因」。
    ///
    /// fox_error(ctx) 只能报告**这个上下文**上的失败 —— 而 fox_create 失败时
    /// **没有上下文可传**，于是 fox.h 里「失败时 fox_error() 解释原因」这条
    /// 承诺根本没法兑现（fox.h:163）。加一个进程级的出口，这条承诺才成立。
    ///
    /// ⚠ 刻意用**固定缓冲**而不是 `thread_local std::string`：
    ///   这个文件由 nvcc_wrapper 编译（CUDA 版 rvdl_rt），而 nvcc 对
    ///   「共享库里的 thread_local + 非平凡析构」处理得不可靠 ——
    ///   实测：加了 thread_local std::string 之后，fox_create 直接卡死在
    ///   99% CPU（连 fox_create 的返回都到不了）。
    ///   诊断字符串不需要线程局部性：固定缓冲 + 原子写入足够，
    ///   极端竞争下最多消息错乱，不会崩。
    char g_last_error[256] = {0};

    void set_last_error(const char *msg)
    {
        if (!msg)
        {
            g_last_error[0] = '\0';
            return;
        }
        std::snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
    }
} // namespace

// ── 类型转换 ─────────────────────────────────────────────────────────

namespace
{
    Rect to_rect(fox_rect r) { return Rect{r.x, r.y, r.w, r.h}; }
    fox_rect from_rect(const Rect &r) { return fox_rect{r.x, r.y, r.w, r.h}; }

    Color to_color(fox_color c) { return Color(c.r, c.g, c.b, c.a); }
    fox_color from_color(const Color &c) { return fox_color{c.r, c.g, c.b, c.a}; }

    Rect to_uv(fox_uv s) { return Rect{s.u, s.v, s.du, s.dv}; }

    ButtonStyle to_button_style(int s)
    {
        switch (s)
        {
        case FOX_BUTTON_PLAIN: return ButtonStyle::Plain;
        case FOX_BUTTON_FILLED: return ButtonStyle::Filled;
        case FOX_BUTTON_BEZEL: return ButtonStyle::Bezel;
        case FOX_BUTTON_TINTED:
        default: return ButtonStyle::Tinted;
        }
    }

    SliderStyle to_slider_style(int s)
    {
        return s == FOX_SLIDER_STEPPED ? SliderStyle::Stepped : SliderStyle::Continuous;
    }

    Align to_align(int a)
    {
        switch (a)
        {
        case FOX_ALIGN_CENTER: return Align::Center;
        case FOX_ALIGN_END: return Align::End;
        case FOX_ALIGN_STRETCH: return Align::Stretch;
        case FOX_ALIGN_START:
        default: return Align::Start;
        }
    }

    int from_bool(bool b) { return b ? 1 : 0; }
} // namespace

// ── 上下文 ─────────────────────────────────────────────────────────

// 放在 `struct fox_ctx` 里而不是 typedef 的 void*：C 头文件前向声明了
// `struct fox_ctx`，这个定义就是补全它的。C API 拥有的一切都在这里，
// fox_destroy() 只有一个东西要销毁。
struct fox_ctx
{
    Context ctx;
    VkBackend backend;
    Window window;
    bool backend_ready = false;
    std::string error;

    // 侧栏滚动在 C++ API 里是出参，但在 C API 里必须跨帧保留，
    // 所以由 C 层持有。
    int sidebar_scroll = 0;

    void fail(const char *msg)
    {
        error = msg ? msg : "unspecified error";
        std::fprintf(stderr, "[fox] %s\n", error.c_str());
        set_last_error(error.c_str());
    }
};

// C 头文件用 `typedef struct fox_ctx fox_ctx;`。只要标签一致，在 C++ 里
// 补全同一个结构体是合法的，这里正是如此。
static_assert(sizeof(fox_rect) == 4 * sizeof(float),
              "fox_rect must stay four floats");
static_assert(sizeof(fox_color) == 4, "fox_color must stay four bytes");

// ── 生命周期 ───────────────────────────────────────────────────────────

extern "C" fox_ctx *fox_create(const fox_config *cfg)
{
    fox_ctx *c = nullptr;
    try
    {
        c = new fox_ctx();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[fox] create failed: %s\n", e.what());
        set_last_error(e.what());
        return nullptr;
    }
    catch (...)
    {
        set_last_error("out of memory");
        return nullptr;
    }

    int w = cfg && cfg->width > 0 ? cfg->width : 1000;
    int h = cfg && cfg->height > 0 ? cfg->height : 680;
    const char *title = (cfg && cfg->title) ? cfg->title : "Fox";
    c->ctx.init(title, w, h);
    return c;
}

extern "C" void fox_destroy(fox_ctx *ctx)
{
    if (!ctx) return;
    // 顺序有讲究：先关后端——shutdown() 会销毁 VkSurfaceKHR 和宿主窗口所
    // 依附的 VkInstance，而 Vulkan 要求这两者先消失。所以宿主必须在调用
    // 本函数**之后**再销毁自己的窗口。
    ctx->backend.shutdown();
    ctx->ctx.shutdown();
    delete ctx;
}

extern "C" int fox_init_backend_vulkan(fox_ctx *ctx, void *glfw_window)
{
    if (!ctx) return 0;
    try
    {
        if (!ctx->backend.init((GLFWwindow *)glfw_window, ctx->ctx))
        {
            ctx->fail("fox_init_backend_vulkan failed");
            return 0;
        }
        ctx->backend_ready = true;
        return 1;
    }
    catch (...)
    {
        ctx->fail("fox_init_backend_vulkan threw");
        return 0;
    }
}

extern "C" void fox_shutdown_backend(fox_ctx *ctx)
{
    if (!ctx) return;
    ctx->backend.shutdown();
    ctx->backend_ready = false;
}

extern "C" int fox_backend_ready(fox_ctx *ctx)
{
    // 问后端自己，而不是问 fox_ctx 里那个独立的 bool —— 那个 bool 可以和
    // 后端的真实状态漂移（后端死了它还是 true），于是宿主以为有后端、
    // 实际什么都画不出来。
    if (!ctx) return 0;
    return ctx->backend_ready && ctx->backend.ready() ? 1 : 0;
}

extern "C" int fox_should_close(fox_ctx *ctx)
{
    if (!ctx) return 0;
    // 「宿主该关闭了」—— 只反映两个**真实**事实：
    //   · 宿主自己请求过关闭（fox_set_should_close）
    //   · 窗口请求过关闭（由 poll_events 同步进 Context）
    //
    // ⚠ 之前是 `ctx->ctx.should_close() || ctx->backend.should_close()`，而
    // VkBackend::should_close() 写的是 `window_ == nullptr || glfwWindowShouldClose()`
    // —— **把「没有窗口」当成「用户想关闭」**。后果：
    //   · headless 上下文（只做文本度量 / 离屏渲染，完全合法的模式）永远
    //     报告「应该关闭」；
    //   · 宿主那个「无窗 fox 上下文」于是永远返回真 → 主循环第一帧就退出
    //     （表现为「界面一闪就没」，frames=0）。
    return ctx->ctx.should_close() ? 1 : 0;
}

extern "C" int fox_has_window(fox_ctx *ctx)
{
    // 有没有窗口。0 = headless（离屏 / 文本度量），这是一个**受支持的模式**，
    // 不是错误状态。用它把「未初始化」与「用户要关闭」区分开。
    return (ctx && ctx->backend_ready && ctx->backend.has_window()) ? 1 : 0;
}

extern "C" void fox_set_should_close(fox_ctx *ctx, int value)
{
    if (!ctx) return;
    // 之前这个函数**什么都不做**：value 被丢弃，只顺手清了 ctx->error ——
    // 一个 setter 既不设状态、又擦诊断信息（把真正的失败原因抹掉）。
    // 而它的注释宣称「请求会被记录在 context 上并由宿主循环遵守」——
    // 那条记录从来不存在。
    ctx->ctx.set_close_requested(value != 0);
}

// ── frame ───────────────────────────────────────────────────────────────

extern "C" void fox_poll_events(fox_ctx *ctx)
{
    if (ctx && ctx->backend_ready) ctx->backend.poll_events(ctx->ctx);
}

extern "C" void fox_begin_frame(fox_ctx *ctx)
{
    if (ctx) ctx->ctx.frame_begin();
}

extern "C" void fox_end_frame(fox_ctx *ctx)
{
    if (!ctx) return;
    // ⚠ 必须在这里结束帧 —— 这是 C ABI 通路**唯一**的 frame_end 入口。
    //
    // C++ 侧（fox_demo / fox_headless）都显式调 c.frame_end()，所以这个洞
    // 只在走 C ABI 时暴露 —— 而 C ABI 正是 rvdl 的唯一通路。
    //
    // Context::frame_end() 是**唯一**调用 input_.end_frame() 与 layout_.end()
    // 的地方（core/context.cpp:229）。漏掉它的后果：
    //  1. **按键边沿永久卡死**：keys_pressed/keys_released 只在 poll_events
    //     里置位、只在 input_.end_frame() 里清除。用户按一次 A，此后
    //     fox_key_pressed() **每一帧**都返回 1（双击标记同理）。
    //  2. **布局栈无界泄漏**：layout_.begin() 每帧 push 一个 Layout，
    //     layout_.end() 从不 pop → ~80B/帧，60fps 约 17MB/小时，且无上界。
    //
    // 顺序：frame_end() 只清输入边沿与弹布局栈，不动显示列表，放在 render
    // 之前即可（display list 已在本帧的绘制调用里建好）。但**不能**因为
    // 「没有后端」就提前返回 —— headless 那一帧的输入边沿同样要清，
    // 否则 headless 宿主会一直看到「上一次按下的键还按着」。
    ctx->ctx.frame_end();
    if (ctx->backend_ready) ctx->backend.render(ctx->ctx.dl(), ctx->ctx);
}

extern "C" void fox_set_size(fox_ctx *ctx, int32_t w, int32_t h)
{
    // **逻辑**尺寸 —— 与整个 API 的单位一致（fox.h 的全局约定是「矩形一律
    // 逻辑单位」，fox_width/fox_height 返回的也是逻辑值）。
    //
    // 之前这里直接调 set_framebuffer_size()，也就是**物理**像素：在 2x 屏上
    // 传 1000×680 会变成逻辑 500×340，整个 UI 缩到一半。它能活下来只是
    // 因为在 1x 屏上恰好正确 —— 典型的「在开发机上测不出来」。
    if (!ctx || w <= 0 || h <= 0) return;
    ctx->ctx.set_size_logical((int)w, (int)h);
}

extern "C" int32_t fox_width(fox_ctx *ctx) { return ctx ? ctx->ctx.width() : 0; }
extern "C" int32_t fox_height(fox_ctx *ctx) { return ctx ? ctx->ctx.height() : 0; }
extern "C" float fox_time(fox_ctx *ctx) { return ctx ? ctx->ctx.time() : 0.0f; }
extern "C" float fox_dt(fox_ctx *ctx) { return ctx ? ctx->ctx.dt() : 0.0f; }
extern "C" float fox_scale(fox_ctx *ctx) { return ctx ? ctx->ctx.scale() : 1.0f; }

// ── 主题 ───────────────────────────────────────────────────────────────

extern "C" void fox_set_theme(fox_ctx *ctx, fox_appearance appearance)
{
    if (!ctx) return;
    // 走 Context，这样 DPI 缩放会随后重新应用；直接调 theme().init() 会把
    // 每个长度 token 重置回 1x。
    ctx->ctx.set_theme(appearance == FOX_APPEARANCE_DARK ? Appearance::Dark
                                                        : Appearance::Light,
                       ctx->ctx.theme().accent);
}

extern "C" void fox_set_accent(fox_ctx *ctx, uint32_t rgb)
{
    if (!ctx) return;
    // 之前是 `set_theme(appearance, (Accent)0)` —— 把 rgb **丢掉**，而且
    // (Accent)0 是 Multicolor（主题表里根本没有这个分支）→ 落回默认蓝；
    // 更糟的是 set_theme() 会把整个主题（所有颜色、所有 metric）重置。
    // 现在只换强调色派生的那几个颜色，主题其余部分原样保留。
    ctx->ctx.theme().set_accent_rgb(rgb);
}

extern "C" fox_color fox_theme_color(fox_ctx *ctx, fox_color_token token)
{
    if (!ctx) return fox_color{0, 0, 0, 255};
    const Theme &t = ctx->ctx.theme();
    switch (token)
    {
    case FOX_COLOR_WINDOW_BG: return from_color(t.window_bg);
    case FOX_COLOR_SIDEBAR_BG: return from_color(t.sidebar_bg);
    case FOX_COLOR_SIDEBAR_TEXT: return from_color(t.sidebar_text);
    case FOX_COLOR_SIDEBAR_TEXT_DIM: return from_color(t.sidebar_text_dim);
    case FOX_COLOR_TEXT: return from_color(t.text);
    case FOX_COLOR_TEXT_SECONDARY: return from_color(t.text_secondary);
    case FOX_COLOR_TEXT_TERTIARY: return from_color(t.text_tertiary);
    case FOX_COLOR_TEXT_ON_ACCENT: return from_color(t.text_on_accent);
    case FOX_COLOR_CONTROL_BG: return from_color(t.control_bg);
    case FOX_COLOR_CONTROL_BG_HOVER: return from_color(t.control_bg_hover);
    case FOX_COLOR_CONTROL_BG_ACTIVE: return from_color(t.control_bg_active);
    case FOX_COLOR_CONTROL_BG_DISABLED: return from_color(t.control_bg_disabled);
    case FOX_COLOR_CONTROL_BORDER: return from_color(t.control_border);
    case FOX_COLOR_SEPARATOR: return from_color(t.separator);
    case FOX_COLOR_SELECTION: return from_color(t.selection);
    case FOX_COLOR_ACCENT: return from_color(t.accent_color);
    case FOX_COLOR_ACCENT_HOVER: return from_color(t.accent_hover);
    case FOX_COLOR_ACCENT_ACTIVE: return from_color(t.accent_active);
    default: return from_color(t.text);
    }
}

extern "C" float fox_theme_metric(fox_ctx *ctx, int32_t token)
{
    if (!ctx) return 0.0f;
    const Theme &t = ctx->ctx.theme();
    switch (token)
    {
    case FOX_METRIC_CORNER_WINDOW: return t.corner_window;
    case FOX_METRIC_CORNER_PANEL: return t.corner_panel;
    case FOX_METRIC_CORNER_CONTROL: return t.corner_control;
    case FOX_METRIC_CORNER_FIELD: return t.corner_field;
    case FOX_METRIC_CONTROL_HEIGHT: return t.control_height;
    case FOX_METRIC_CONTROL_HEIGHT_SMALL: return t.control_height_small;
    case FOX_METRIC_CONTROL_HEIGHT_LARGE: return t.control_height_large;
    case FOX_METRIC_SIDEBAR_WIDTH: return t.sidebar_width;
    case FOX_METRIC_TITLEBAR_HEIGHT: return t.titlebar_height;
    case FOX_METRIC_ROW_HEIGHT: return t.row_height;
    case FOX_METRIC_SPACING_XS: return t.spacing_xs;
    case FOX_METRIC_SPACING_SM: return t.spacing_sm;
    case FOX_METRIC_SPACING_MD: return t.spacing_md;
    case FOX_METRIC_SPACING_LG: return t.spacing_lg;
    case FOX_METRIC_HAIRLINE: return t.hairline;
    case FOX_METRIC_FONT_CAPTION: return t.font_caption;
    case FOX_METRIC_FONT_SMALL: return t.font_small;
    case FOX_METRIC_FONT_BODY: return t.font_body;
    case FOX_METRIC_FONT_CALLOUT: return t.font_callout;
    case FOX_METRIC_FONT_TITLE3: return t.font_title3;
    case FOX_METRIC_FONT_TITLE2: return t.font_title2;
    case FOX_METRIC_FONT_TITLE1: return t.font_title1;
    default: return 0.0f;
    }
}

// ── 输入 ───────────────────────────────────────────────────────────────

extern "C" float fox_mouse_x(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.input().mouse_pos.x : 0.0f;
}
extern "C" float fox_mouse_y(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.input().mouse_pos.y : 0.0f;
}
extern "C" int fox_mouse_down(fox_ctx *ctx, fox_mouse_button b)
{
    if (!ctx || (int)b >= (int)MouseButton::Count) return 0;
    return from_bool(ctx->ctx.input().buttons[(int)b].down);
}
extern "C" int fox_mouse_pressed(fox_ctx *ctx, fox_mouse_button b)
{
    if (!ctx || (int)b >= (int)MouseButton::Count) return 0;
    return from_bool(ctx->ctx.input().buttons[(int)b].pressed);
}
extern "C" int fox_mouse_released(fox_ctx *ctx, fox_mouse_button b)
{
    if (!ctx || (int)b >= (int)MouseButton::Count) return 0;
    return from_bool(ctx->ctx.input().buttons[(int)b].released);
}
extern "C" int fox_mouse_click_count(fox_ctx *ctx, fox_mouse_button button)
{
    // 按按键分开计，不是全局的：Fox 在每个 ButtonState 上跟踪点击数，
    // 左键双击不能让右键看起来也像双击。
    if (!ctx || (int)button >= (int)MouseButton::Count) return 0;
    return ctx->ctx.input().buttons[(int)button].click_count;
}
extern "C" float fox_mouse_wheel(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.input().scroll_y : 0.0f;
}

extern "C" int fox_key_down(fox_ctx *ctx, int32_t key)
{
    return ctx ? from_bool(ctx->ctx.input().key_down((Key)key)) : 0;
}
extern "C" int fox_key_pressed(fox_ctx *ctx, int32_t key)
{
    return ctx ? from_bool(ctx->ctx.input().key_press((Key)key)) : 0;
}
extern "C" int fox_key_released(fox_ctx *ctx, int32_t key)
{
    return ctx ? from_bool(ctx->ctx.input().key_released_now((Key)key)) : 0;
}
extern "C" int fox_key_ascii_down(fox_ctx *ctx, int32_t a)
{
    return ctx && a >= 0 && a < KEY_SLOTS ? from_bool(ctx->ctx.input().keys_down[a]) : 0;
}
extern "C" int fox_key_ascii_pressed(fox_ctx *ctx, int32_t a)
{
    return ctx && a >= 0 && a < KEY_SLOTS ? from_bool(ctx->ctx.input().keys_pressed[a]) : 0;
}
extern "C" int fox_shift(fox_ctx *ctx) { return ctx && ctx->ctx.input().shift; }
extern "C" int fox_ctrl(fox_ctx *ctx) { return ctx && ctx->ctx.input().ctrl; }
extern "C" int fox_alt(fox_ctx *ctx) { return ctx && ctx->ctx.input().alt; }
extern "C" int fox_super(fox_ctx *ctx) { return ctx && ctx->ctx.input().super_key; }

// ── 控件 ─────────────────────────────────────────────────────────────

extern "C" float fox_label(fox_ctx *ctx, fox_rect r, const char *text, fox_color color,
                           fox_align align)
{
    if (!ctx || !text) return 0.0f;
    return label(ctx->ctx, to_rect(r), text, to_color(color), to_align(align));
}

// ── 裁剪 ────────────────────────────────────────────────────────────

extern "C" void fox_push_clip(fox_ctx *ctx, fox_rect r)
{
    if (!ctx) return;
    ctx->ctx.dl().push_clip(to_rect(r));
}

extern "C" void fox_clip_pop(fox_ctx *ctx)
{
    if (!ctx) return;
    ctx->ctx.dl().push_clip_pop();
}

// ── 文本度量 ────────────────────────────────────────────────────────

extern "C" float fox_text_width(fox_ctx *ctx, int32_t px, const char *text)
{
    if (!ctx || !text) return 0.0f;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    return text_width(f, text);
}

extern "C" float fox_text_height(fox_ctx *ctx, int32_t px)
{
    if (!ctx) return 0.0f;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    return fonts().line_height(f);
}

extern "C" float fox_text_offset_x(fox_ctx *ctx, int32_t px, const char *text,
                                   int32_t byte_index)
{
    if (!ctx || !text) return 0.0f;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    return text_offset_x(f, text, (size_t)(byte_index > 0 ? byte_index : 0));
}

extern "C" int32_t fox_text_lines(fox_ctx *ctx, int32_t width, int32_t px, const char *text)
{
    if (!ctx || !text) return 0;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    const std::string s(text);
    const f32 avail = (f32)(width > 0 ? width : 1);
    // 逐逻辑行换行（text/soft_wrap.hpp 的同一算法 —— 宿主行算术与 Fox 一致），
    // 空串按 1 行算：一个空文档仍占一行，光标要能落在上面。
    size_t rows = 0;
    size_t ls = 0;
    const size_t n = s.size();
    for (size_t i = 0; i <= n; ++i)
    {
        if (i == n || s[i] == '\n')
        {
            std::vector<WrapSpan> spans((i - ls) + 1);
            const int nr = wrap_line(f, s, ls, i, avail, spans.data(), (int)spans.size());
            rows += (size_t)(nr > 0 ? nr : 1);
            ls = i + 1;
        }
    }
    return (int32_t)(rows > 0 ? rows : 1);
}

extern "C" int32_t fox_glyph_count(fox_ctx *ctx, int32_t px)
{
    if (!ctx) return 0;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    GlyphAtlas *a = fonts().atlas(f);
    return a ? (int32_t)a->glyph_count() : 0;
}

// ── 键入字符 ────────────────────────────────────────────────────────

extern "C" int32_t fox_char_pending(fox_ctx *ctx)
{
    if (!ctx) return 0;
    return (int32_t)ctx->ctx.input().chars.size();
}

extern "C" int32_t fox_char(fox_ctx *ctx)
{
    if (!ctx || ctx->ctx.input().chars.empty()) return 0;
    const u32 cp = ctx->ctx.input().chars.front();
    ctx->ctx.input().chars.erase(ctx->ctx.input().chars.begin());
    return (int32_t)cp;
}

extern "C" int fox_button(fox_ctx *ctx, fox_rect r, const char *label_text,
                          fox_button_style style, int enabled)
{
    if (!ctx || !label_text) return 0;
    return from_bool(button(ctx->ctx, to_rect(r), label_text, to_button_style(style),
                            enabled != 0));
}

extern "C" int fox_checkbox(fox_ctx *ctx, fox_rect r, const char *label_text, int *value)
{
    if (!ctx || !label_text || !value) return 0;
    bool v = *value != 0;
    const bool changed = checkbox(ctx->ctx, to_rect(r), label_text, v);
    *value = v ? 1 : 0;
    return from_bool(changed);
}

extern "C" int fox_toggle_switch(fox_ctx *ctx, fox_rect r, int *value)
{
    if (!ctx || !value) return 0;
    bool v = *value != 0;
    const bool changed = toggle_switch(ctx->ctx, to_rect(r), v);
    *value = v ? 1 : 0;
    return from_bool(changed);
}

extern "C" int fox_slider(fox_ctx *ctx, fox_rect r, float *value, float min, float max,
                          fox_slider_style style, float step)
{
    if (!ctx || !value) return 0;
    float v = *value;
    const bool dragging = slider(ctx->ctx, to_rect(r), v, min, max, to_slider_style(style), step);
    *value = v;
    return from_bool(dragging);
}

// ── 组合控件 ──────────────────────────────────────────────────────────

namespace
{
    /// 逗号分隔的列表，拆成 C 字符串。
    ///
    /// 自己持有存储：C++ 的 combo/menu API 收 `const char *const *`，
    /// 而调用方的 buffer 不能改（原地替换逗号等于对别人的字符串产生副作用）。
    /// 早先的草图返回指向局部 std::string 的裸指针——函数一返回就悬空，
    /// 这种 bug 只在负载下表现为乱掉的菜单文字。
    struct CsvList
    {
        std::string storage;             ///< 持有字节
        std::vector<const char *> items; ///< 指向 storage 内部
        int count() const { return (int)items.size(); }
    };

    CsvList split_csv(const char *csv)
    {
        CsvList list;
        if (!csv) return list;
        list.storage = csv;
        char *p = list.storage.data();
        list.items.push_back(p);
        for (char &ch : list.storage)
        {
            if (ch == ',')
            {
                ch = '\0';
                list.items.push_back(&ch + 1);
            }
        }
        return list;
    }
} // namespace

extern "C" int fox_menu_title(fox_ctx *ctx, fox_rect r, const char *label)
{
    if (!ctx || !label) return 0;
    return from_bool(menu_title(ctx->ctx, to_rect(r), label));
}

extern "C" fox_rect fox_menu_popup(fox_ctx *ctx, fox_rect anchor,
                                   const char *items_csv, float item_height)
{
    if (!ctx) return fox_rect{0, 0, 0, 0};
    const CsvList items = split_csv(items_csv);
    const Rect content = menu_popup(ctx->ctx, to_rect(anchor), items.count(),
                                    item_height > 0.0f ? item_height : 22.0f);
    return from_rect(content);
}

extern "C" int fox_menu_item(fox_ctx *ctx, fox_rect r, const char *label, int enabled)
{
    // ⚠ 返回值的**极性**：fox.h 的契约是「点击时返回非零，否则返回 0」。
    //
    // 之前是 `from_bool(clicked) ? 0 : -1` —— **极性反了**：
    //   点击   → from_bool(true)  = 1 → 真 → 返回 0    （宿主永远看不到点击）
    //   未点击 → from_bool(false) = 0 → 假 → 返回 -1   （宿主每帧都以为点了）
    // 于是宿主写 `if (fox_menu_item(...)) { act(); }` 会**每一帧**都触发，
    // 而真正的点击**永不触发**。而「未点击返回 -1」本身也违反「or 0」。
    if (!ctx || !label) return 0;
    return menu_item(ctx->ctx, to_rect(r), label, enabled != 0) ? 1 : 0;
}

extern "C" void fox_menu_close_all(fox_ctx *ctx)
{
    if (ctx) menu_close_all(ctx->ctx);
}

extern "C" int fox_combo(fox_ctx *ctx, fox_rect r, const char *items_csv, int *selected)
{
    if (!ctx || !selected) return -1;
    const CsvList items = split_csv(items_csv);
    int sel = *selected;
    const bool changed = combo(ctx->ctx, to_rect(r), items.items.data(), items.count(), sel);
    *selected = sel;
    return changed ? sel : -1;
}

extern "C" int fox_selectable(fox_ctx *ctx, fox_rect r, const char *label, int *selected)
{
    if (!ctx || !label || !selected) return 0;
    bool v = *selected != 0;
    const bool clicked = selectable(ctx->ctx, to_rect(r), label, v);
    *selected = v ? 1 : 0;
    return from_bool(clicked);
}

extern "C" fox_rect fox_group_begin(fox_ctx *ctx, fox_rect r, const char *title,
                                    fox_id id, float content_height)
{
    if (!ctx) return fox_rect{0, 0, 0, 0};
    return from_rect(group_begin(ctx->ctx, to_rect(r), title, (Id)id, content_height));
}

extern "C" void fox_group_end(fox_ctx *ctx)
{
    if (ctx) group_end(ctx->ctx);
}

extern "C" int fox_property_int(fox_ctx *ctx, fox_rect r, const char *label,
                                int32_t *value, int32_t min, int32_t max, int32_t step)
{
    if (!ctx || !value) return 0;
    int v = *value;
    const bool changed = property_int(ctx->ctx, to_rect(r), label, v, (int)min, (int)max,
                                      (int)(step > 0 ? step : 1));
    *value = v;
    return from_bool(changed);
}

extern "C" int fox_property_float(fox_ctx *ctx, fox_rect r, const char *label,
                                  float *value, float min, float max, float step)
{
    if (!ctx || !value) return 0;
    float v = *value;
    const bool changed = property_float(ctx->ctx, to_rect(r), label, v, min, max, step);
    *value = v;
    return from_bool(changed);
}

extern "C" int fox_text_field(fox_ctx *ctx, fox_rect r, char *buffer, int32_t capacity,
                              const char *placeholder, fox_id id)
{
    if (!ctx || !buffer || capacity <= 1) return 0;
    return from_bool(text_field(ctx->ctx, to_rect(r), buffer, (int)capacity, placeholder,
                                 (Id)id));
}

extern "C" int32_t fox_text_field_caret(fox_ctx *ctx, fox_id id)
{
    if (!ctx || id == 0) return 0;
    // 光标的**字节偏移**（fox.h 的契约）。
    //
    // ⚠ 之前返回的是 `st.caret` —— 那是**闪烁相位**（距离上次编辑的秒数），
    // 不是位置；而且它全库只被赋过一次 0，所以这个函数**对任何字段都返回 0**
    // （宿主于是以为光标永远在最前面）。
    // 真正的偏移在 TextBuffer::caret() 里。
    //
    // 用 peek 而不是 text_edit()：后者会**创建**缓冲区与撤销栈 —— 一个只读
    // 查询不该分配。
    const TextBuffer *tb = ctx->ctx.text_edit_peek((Id)id);
    return tb ? (int32_t)tb->caret() : 0;
}

extern "C" void fox_separator(fox_ctx *ctx, fox_rect r, int horizontal)
{
    if (ctx) separator(ctx->ctx, to_rect(r), horizontal != 0);
}

extern "C" void fox_panel(fox_ctx *ctx, fox_rect r, fox_color fill, float radius, int glass)
{
    if (ctx) panel(ctx->ctx, to_rect(r), to_color(fill), radius, glass != 0);
}

extern "C" void fox_glass_bar(fox_ctx *ctx, fox_rect r, float radius)
{
    if (ctx) glass_bar(ctx->ctx, to_rect(r), radius);
}

// ── 底层图元 ──────────────────────────────────────────────────────────

extern "C" void fox_fill_rect(fox_ctx *ctx, fox_rect r, fox_color c)
{
    if (ctx) ctx->ctx.dl().push(to_rect(r), to_color(c));
}

extern "C" void fox_fill_ellipse(fox_ctx *ctx, fox_rect r, fox_color c)
{
    if (ctx) ctx->ctx.dl().push_ellipse(to_rect(r), to_color(c));
}

extern "C" void fox_fill_triangle(fox_ctx *ctx, float x0, float y0, float x1, float y1,
                                  float x2, float y2, fox_color c)
{
    if (!ctx) return;
    ctx->ctx.dl().push_triangle(Vec2{x0, y0}, Vec2{x1, y1}, Vec2{x2, y2}, to_color(c));
}

extern "C" int fox_path(fox_ctx *ctx, const float *xy, int32_t count, float thickness,
                        fox_color c, int cap, int join)
{
    if (!ctx || !xy || count < 2) return 0;

    // C++ API 收 Vec2；C API 收交错的 float。这一次分配以上调用方自己的
    // count 为界——和 fox_sidebar 的转换同一个套路。
    Vec2 *pts = nullptr;
    try
    {
        pts = new Vec2[(size_t)count];
    }
    catch (...)
    {
        ctx->fail("fox_path allocation failed");
        return 0;
    }
    for (int32_t i = 0; i < count; ++i)
    {
        pts[i].x = xy[i * 2];
        pts[i].y = xy[i * 2 + 1];
    }
    ctx->ctx.dl().push_path(pts, (u32)count, thickness, to_color(c), (u8)cap, (u8)join);
    delete[] pts;
    return 1;
}

// ── 纹理 ────────────────────────────────────────────────────────────

extern "C" fox_tex fox_texture_create(fox_ctx *ctx, int32_t w, int32_t h,
                                      const uint8_t *rgba)
{
    if (!ctx) return TEX_NONE;
    return ctx->ctx.create_texture((u32)w, (u32)h, rgba);
}

extern "C" int fox_texture_update(fox_ctx *ctx, fox_tex tex, int32_t w, int32_t h,
                                  const uint8_t *rgba)
{
    if (!ctx) return 0;
    return from_bool(ctx->ctx.update_texture(tex, (u32)w, (u32)h, rgba));
}

extern "C" void fox_texture_destroy(fox_ctx *ctx, fox_tex tex)
{
    if (ctx) ctx->ctx.destroy_texture(tex);
}

extern "C" fox_tex fox_texture_create_png(fox_ctx *ctx, const void *bytes, size_t len)
{
    if (!ctx || !bytes) return TEX_NONE;
    return ctx->ctx.create_texture_png((const u8 *)bytes, len, "fox_texture_create_png");
}

extern "C" int fox_image(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_color tint)
{
    if (!ctx) return 0;
    return from_bool(image(ctx->ctx, to_rect(r), tex, to_color(tint)));
}

extern "C" int fox_image_region(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_uv src,
                                fox_color tint)
{
    if (!ctx) return 0;
    return from_bool(image_region(ctx->ctx, to_rect(r), tex, to_uv(src), to_color(tint)));
}

// ── 窗口装饰 ─────────────────────────────────────────────────────────

extern "C" int fox_window_begin(fox_ctx *ctx, fox_rect bounds, const char *title,
                                int show_sidebar)
{
    if (!ctx) return 0;
    // 返回「这一帧画了窗口框」。
    //
    // ⚠ **不是**「关闭按钮被按下」。Window::begin() 恒返回 true，把关闭状态
    // 混进返回值只会让宿主分不清「没画出来」和「用户要关闭」两件事。
    // 关闭请求走 fox_window_close_requested()。
    ctx->window.begin(ctx->ctx, to_rect(bounds), title ? title : "Fox",
                      show_sidebar != 0);
    return 1;
}

extern "C" int fox_window_close_requested(fox_ctx *ctx)
{
    // 本帧窗口的关闭按钮（红绿灯那颗）是否被按下。
    //
    // Window::begin() 在每次开头把标志清零、按下时置位，所以宿主在
    // fox_window_begin() 之后读它，拿到的就是「这一帧」。
    //
    // 之前这个状态**只有 C++ 侧读得到**（Window::close_requested()），
    // C ABI 没有任何出口 —— 宿主既收不到真正的关闭请求，文档里那句
    // 「Returns 1 if the close button was pressed」也永远是假的。
    return (ctx && ctx->window.close_requested()) ? 1 : 0;
}

extern "C" fox_rect fox_window_content(fox_ctx *ctx)
{
    return ctx ? from_rect(ctx->window.content()) : fox_rect{0, 0, 0, 0};
}
extern "C" fox_rect fox_window_sidebar(fox_ctx *ctx)
{
    return ctx ? from_rect(ctx->window.sidebar()) : fox_rect{0, 0, 0, 0};
}
extern "C" fox_rect fox_window_titlebar(fox_ctx *ctx)
{
    return ctx ? from_rect(ctx->window.titlebar()) : fox_rect{0, 0, 0, 0};
}

extern "C" int32_t fox_sidebar(fox_ctx *ctx, fox_rect r, const fox_sidebar_item *items,
                               int32_t count, int32_t selected)
{
    if (!ctx || !items || count <= 0) return -1;

    // C++ API 收带类型的条目数组；为这次调用构造一个。这是 C 层唯一的
    // 分配点，大小以上调用方自己的 count 为界。
    SidebarItem *tmp = nullptr;
    try
    {
        tmp = new SidebarItem[(size_t)count];
    }
    catch (...)
    {
        ctx->fail("fox_sidebar allocation failed");
        return -1;
    }
    for (int32_t i = 0; i < count; ++i)
    {
        tmp[i].label = items[i].label;
        tmp[i].icon_tex = (TexId)items[i].icon_tex;
        tmp[i].icon_src = to_uv(items[i].icon_src);
        // icon_src 的默认语义：{0,0,1,1} = 整张纹理。
        //
        // C 结构体没有默认成员初始化，宿主只填 label + icon_tex 时 icon_src
        // 是全零 —— 而 image_region 见到 du=dv=0 直接返回 false，于是
        // **图标永远不画、且没有任何报错**（最坏的一类：静默失效）。
        // 零宽高按「整张纹理」处理（C++ 侧 SidebarItem 的默认值也是这个）。
        if (tmp[i].icon_src.w <= 0.0f && tmp[i].icon_src.h <= 0.0f)
            tmp[i].icon_src = Rect{0.0f, 0.0f, 1.0f, 1.0f};
        tmp[i].icon = items[i].icon;
    }
    const int32_t clicked =
        sidebar(ctx->ctx, to_rect(r), tmp, (int)count, (int)selected, ctx->sidebar_scroll);
    delete[] tmp;
    return clicked;
}

// ── id ─────────────────────────────────────────────────────────────────

extern "C" fox_id fox_make_id(uint32_t salt, const char *label_text)
{
    return ctx().make_id((Id)salt, label_text);
}
extern "C" fox_id fox_make_id_i(uint32_t salt, int32_t n)
{
    return ctx().make_id((Id)salt, (Id)n);
}
extern "C" fox_id fox_make_id_rect(uint32_t salt, fox_rect r)
{
    // 与 widget_id()（widget/primitives.cpp:40）**同一条公式**：
    //   make_id(salt, id_hash_rect(rect))
    //
    // ⚠ fox.h 的文档引用了这个函数，但全库**没有实现** —— 宿主于是无法为
    // 「按位置派生的字段」算出 id，fox_text_field_caret 对这类字段根本不可用，
    // 而文档还把它当既有能力写着。
    return ctx().make_id((Id)salt, id_hash_rect(to_rect(r)));
}

// ── 诊断 ─────────────────────────────────────────────────────────────

extern "C" const char *fox_error(fox_ctx *ctx)
{
    return ctx ? ctx->error.c_str() : "null context";
}

extern "C" const char *fox_last_error(void)
{
    return g_last_error;
}

extern "C" const char *fox_window_title(fox_ctx *ctx)
{
    // 标题在 init() 时存下来了（之前被直接丢弃），所以宿主与 Fox 内部
    // 读的是同一个值。
    return ctx ? ctx->ctx.title() : "";
}

extern "C" uint32_t fox_last_instance_count(fox_ctx *ctx)
{
    return ctx ? ctx->backend.last_instance_count() : 0;
}

extern "C" uint32_t fox_frame_faults(fox_ctx *ctx)
{
    if (!ctx) return 0;
    // 无后端 = 每一帧都没提交 = 每一帧都没到屏幕。
    //
    // 之前这里恒返回 0，于是宿主**无法区分**「全部成功」与「根本没画」——
    // 而 fox.h 恰恰警告过这种「宿主分辨不出来」的情况（"a host that cannot
    // tell will happily keep drawing into the void"）。
    if (!ctx->backend_ready) return (uint32_t)ctx->ctx.frame_index();
    return ctx->backend.frame_faults();
}

extern "C" const char *fox_last_fault(fox_ctx *ctx)
{
    return ctx ? ctx->backend.last_fault() : "null context";
}

extern "C" uint32_t fox_last_frame_dropped(fox_ctx *ctx)
{
    return ctx ? ctx->backend.last_dropped() : 0;
}

extern "C" const char *fox_gpu_name(fox_ctx *ctx)
{
    return ctx ? ctx->backend.gpu_name() : "";
}

extern "C" uint32_t fox_gpu_vendor(fox_ctx *ctx)
{
    return ctx ? ctx->backend.gpu_vendor() : 0;
}

extern "C" void fox_set_quality(fox_ctx *ctx, int blur, int shadows, int animations)
{
    if (ctx) ctx->ctx.set_quality(blur, shadows, animations != 0);
}

extern "C" void fox_get_quality(fox_ctx *ctx, int *blur, int *shadows, int *animations)
{
    // 没有上下文 = 从没设置过质量档；报默认值，而不是不动调用方的变量
    // （往复用 buffer 里读回的调用方绝不该看到没要过的过期值）。
    const Context::Quality q = ctx ? ctx->ctx.quality() : Context::Quality{};
    if (blur) *blur = q.blur;
    if (shadows) *shadows = q.shadows;
    if (animations) *animations = q.animations ? 1 : 0;
}

// 只做翻译——逻辑不在这里。截断和码点边界处理在
// Platform::clipboard_text_into 里，因为那是文本的规则而不是 C ABI 的规则，
// 在这里复制一份就多一个可能写错的地方。
extern "C" size_t fox_clipboard_text(char *out, size_t cap)
{
    Platform *p = platform();
    if (!p) { if (out && cap) out[0] = 0; return 0; }
    p->clipboard_text_into(out, cap);
    if (!out || cap == 0) return 0;
    return strlen(out);
}

extern "C" int fox_clipboard_set_text(const char *utf8)
{
    Platform *p = platform();
    if (!p || !utf8) return 0;
    p->set_clipboard_text(std::string_view(utf8));
    return 1;
}

// IME：纯透传。所有语义——commit 对撤销历史做什么、什么算取消输入组合、
// 光标去哪——都在 TextBuffer 和 Context 里，因为那是文本的规则。在这里再放
// 一份，C ABI 包装层就会开始和它包装的 C++ 实现不一致。
extern "C" void fox_ime_begin(fox_ctx *ctx, const char *utf8, size_t caret_bytes)
{
    if (ctx) ctx->ctx.ime_begin(utf8, caret_bytes);
}
extern "C" void fox_ime_update(fox_ctx *ctx, const char *utf8, size_t caret_bytes)
{
    if (ctx) ctx->ctx.ime_update(utf8, caret_bytes);
}
extern "C" void fox_ime_commit(fox_ctx *ctx)
{
    if (ctx) ctx->ctx.ime_commit();
}
extern "C" void fox_ime_cancel(fox_ctx *ctx)
{
    if (ctx) ctx->ctx.ime_cancel();
}
extern "C" int fox_ime_is_composing(fox_ctx *ctx)
{
    return ctx && ctx->ctx.ime_composing() ? 1 : 0;
}

extern "C" void fox_focus(fox_ctx *ctx, fox_id id)
{
    if (!ctx) return;
    // 清焦点时放弃正在合成的内容：输入组合锚定在用户刚离开的那个字段的
    // 光标上。
    if (id == 0) ctx->ctx.ime_cancel();
    ctx->ctx.focus().set_focus((Id)id);
}

extern "C" fox_id fox_focused_id(fox_ctx *ctx)
{
    return ctx ? (fox_id)ctx->ctx.focus().focused() : 0;
}

extern "C" int fox_has_focus(fox_ctx *ctx, fox_id id)
{
    return ctx && ctx->ctx.focus().has_focus((Id)id) ? 1 : 0;
}

extern "C" int fox_used_blur_path(fox_ctx *ctx)
{
    return ctx ? from_bool(ctx->backend.used_blur_path()) : 0;
}

extern "C" int fox_dump_ppm(fox_ctx *ctx, const char *path)
{
    if (!ctx || !path) return 0;
    return from_bool(ctx->backend.dump_ppm(path));
}
