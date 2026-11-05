// ═══════════════════════════════════════════════════════════════════════
// Bro 桌面端 UI 原语实现（rvdl_ui_*）
// ═══════════════════════════════════════════════════════════════════════
//
// 必须与 ClassicGfx.cpp / ClassicGui.cpp 编译进同一个模块（rvdl_rt.dll）。
// 原因见 BroUi.hpp 顶部：cgui_state() 里的 inline static 跨 DLL 不共享。
//
// 前置阅读：Bro/ravendrel-abi/bro-ui.patch.md

#include "qhal/BroUi.hpp"
#include "qhal/ClassicGui.hpp"
#include "gui/src/render_context.h"
#include "gui/src/nuklear_config.h"
#include "gui/src/font_loader.h"
#include "qhal/qicon.hpp"

#include <chrono>
#include <cstring>
#include <vector>
#include <cstdio>

namespace
{
    // ── 取句柄 ────────────────────────────────────────────────────────
    //
    // cgui_state().impl 是 qgui::RenderContext*，由 rvdl_cgui_init 写入。
    // 根窗口（nk_begin 出来的那个）用 s.title 查找 —— 和 ClassicGfx.cpp 同理。
    nk_context *ui_ctx()
    {
        auto &s = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        return rc ? rc->ctx() : nullptr;
    }

    nk_command_buffer *ui_canvas()
    {
        auto &s = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc) return nullptr;
        nk_context *c = rc->ctx();
        if (!c) return nullptr;
        auto *win = nk_window_find(c, s.title);
        return win ? &win->buffer : nullptr;
    }

    /// 取指定字号的字体。
    ///
    /// 不走 ctx->style.font —— 那只是「当前默认字号」，多字号场景不够用。
    /// 字体表在 font_loader.cpp（与 atlas 同一个 TU）里。
    /// 越界或该档没烘焙成功时回退到 BODY，再不行才用 style.font。
    const nk_user_font *ui_font(int size)
    {
        const nk_user_font *f = qgui::font_at(size);
        if (f) return f;
        f = qgui::font_at(qgui::FONT_BODY);
        if (f) return f;
        nk_context *c = ui_ctx();
        return c ? c->style.font : nullptr;
    }

    nk_color ui_rgba(int32_t color, nk_byte a)
    {
        return nk_rgba((nk_byte)((color >> 16) & 0xFF), (nk_byte)((color >> 8) & 0xFF),
                       (nk_byte)(color & 0xFF), a);
    }

    // ── UTF-8 ────────────────────────────────────────────────────────
    //
    // 桌面端所有文本都是中文（3 字节/字），折行与量宽必须按码点走，
    // 按字节切会把一个汉字劈成两半，渲染成乱码方块。
    int utf8_len(unsigned char b)
    {
        if (b < 0x80) return 1;
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        return 1;
    }

    float measure(const nk_user_font *f, const char *s, int len)
    {
        if (!f || !s || len <= 0) return 0.f;
        float w = 0.f;
        for (int i = 0; i < len;)
        {
            int cl = utf8_len((unsigned char)s[i]);
            if (i + cl > len) cl = 1;
            w += f->width(f->userdata, f->height, s + i, cl);
            i += cl;
        }
        return w;
    }

    struct Line {
        int off;
        int len;
    };

    /// 折行。\n 与字面 "\n"（槽位文件里换行被转义成两个字符）都当换行。
    int wrap(const nk_user_font *f, const char *s, int len, float maxw, std::vector<Line> &out)
    {
        out.clear();
        if (!f || !s || len <= 0) return 0;
        int start = 0;
        int i = 0;
        float cur = 0.f;
        while (i < len)
        {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '\n' ||
                (ch == '\\' && i + 1 < len && s[i + 1] == 'n'))
            {
                out.push_back({start, i - start});
                i += (ch == '\n') ? 1 : 2;
                start = i;
                cur = 0.f;
                continue;
            }
            int cl = utf8_len(ch);
            if (i + cl > len) cl = 1;
            float cw = f->width(f->userdata, f->height, s + i, cl);
            if (cur > 0.f && cur + cw > maxw)
            {
                out.push_back({start, i - start});
                start = i;
                cur = 0.f;
                continue;
            }
            cur += cw;
            i += cl;
        }
        out.push_back({start, len - start});
        return (int)out.size();
    }

    // ── 帧时序 ────────────────────────────────────────────────────────
    struct Timing {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point prev{};
        double dt_ms = 0.0;
        double time_ms = 0.0;
        int64_t frame = 0;
        bool first = true;

        void tick()
        {
            auto now = std::chrono::steady_clock::now();
            if (first) { prev = now; first = false; }
            dt_ms = std::chrono::duration<double, std::milli>(now - prev).count();
            prev = now;
            time_ms = std::chrono::duration<double, std::milli>(now - t0).count();
            frame += 1;
        }
    };

    Timing &timing()
    {
        static Timing t;
        return t;
    }

    // ── 边沿快照 ──────────────────────────────────────────────────────
    //
    // 大小必须严格等于 NK_KEY_MAX（nk_keyboard.keys 的长度），
    // 用 512 之类魔数会越界读 nk_context 的堆内存。
    struct Edges {
        unsigned char keys[NK_KEY_MAX] = {0};
        unsigned char mouse = 0;
    };

    Edges &edges()
    {
        static Edges e;
        return e;
    }

    int mouse_down()
    {
        nk_context *c = ui_ctx();
        if (!c) return 0;
        return c->input.mouse.buttons[NK_BUTTON_LEFT].down ? 1 : 0;
    }

    // ── 裁剪栈 ────────────────────────────────────────────────────────
    //
    // nk_push_scissor 是**覆盖**语义，不是栈。踩过的坑：
    //   最初的 clip_end 推的是 nk_rect(0,0,0,0)，以为「零尺寸 = 不裁剪」，
    //   实际上 nk_draw_text 里有 `if (c->w == 0 || c->h == 0) return;`
    //   —— 之后**所有**绘制被静默丢掉。
    //   症状：滚动列表里只有最下面一条消息有标题、计划条和状态栏整个消失，
    //   而且不报任何错。
    //
    // 正确做法：自己维护一层栈。clip 时保存当前裁剪矩形，clip_end 恢复它；
    // 栈底是整个窗口。
    constexpr int kClipStackMax = 8;

    struct ClipEntry {
        float x, y, w, h;
    };

    ClipEntry &clip_stack(int i)
    {
        static ClipEntry s[kClipStackMax];
        return s[i];
    }

    int &clip_depth()
    {
        static int d = 0;
        return d;
    }

    void clip_reset()
    {
        clip_depth() = 0;
    }

    ClipEntry clip_current()
    {
        auto &g = qhal::cgui_state();
        if (clip_depth() > 0) return clip_stack(clip_depth() - 1);
        return ClipEntry{0.f, 0.f, (float)g.width, (float)g.height};
    }

    /// 当前图标的场。所有图标共用一块 —— 32×32 float = 4 KB，
    /// 而且图标是静态的，不需要同时存在多份。
    qicon_field &icon_field()
    {
        static qicon_field f;
        return f;
    }
} // namespace

extern "C"
{

// ═══════════════════════════════════════════════════════════════════════
// 帧
// ═══════════════════════════════════════════════════════════════════════

int32_t rvdl_ui_begin(void)
{
    timing().tick();
    clip_reset();   // 每帧清空裁剪栈：rvdl 若漏调 clip_end，下一帧也不会被污染
    return (int32_t)timing().frame;
}

int32_t rvdl_ui_frame(void) { return (int32_t)timing().frame; }
double rvdl_ui_dt_ms(void) { return timing().dt_ms; }
double rvdl_ui_time_ms(void) { return timing().time_ms; }

// ═══════════════════════════════════════════════════════════════════════
// 窗口
// ═══════════════════════════════════════════════════════════════════════

int32_t rvdl_ui_open(void)
{
    auto &s = qhal::cgui_state();
    auto *rc = static_cast<qgui::RenderContext *>(s.impl);
    return (rc && rc->window()) ? 1 : 0;
}

int32_t rvdl_ui_win_w(void) { return rvdl_cgui_width(); }
int32_t rvdl_ui_win_h(void) { return rvdl_cgui_height(); }

// ═══════════════════════════════════════════════════════════════════════
// 文本
// ═══════════════════════════════════════════════════════════════════════
//
// 每个函数都带 size 档位参数（qgui::FontSize）。
// nk_draw_text 用 cmd->font->height，所以字号必须来自烘焙好的字体 ——
// 这也是「写死 16px」这个限制的解法：不是让 nk_draw_text 支持缩放
// （它不支持），而是把多个字号都烘焙进同一张图集，按需选字体。

int32_t rvdl_ui_text(int32_t x, int32_t y, int32_t size, int32_t color, const char *text)
{
    auto *cb = ui_canvas();
    auto *f = ui_font(size);
    if (!cb || !f || !text || !*text) return 0;
    const int len = (int)std::strlen(text);
    const float w = measure(f, text, len);
    // 宽度给 +2：nk_draw_text 会把超出 r.w 的部分裁掉，
    // 量出来的宽度是浮点累积，取整误差会吃掉最后一个字符。
    nk_draw_text(cb, nk_rect((float)x, (float)y, w + 2.f, f->height), text, len, f,
                 nk_rgba(0, 0, 0, 0), ui_rgba(color, 255));
    return 1;
}

int32_t rvdl_ui_textw(int32_t x, int32_t y, int32_t w, int32_t line_height, int32_t size,
                    int32_t color, const char *text)
{
    // 失败时返回**负数**并编码原因，让调用方能区分「没内容可画」
    // 和「环境没就绪」—— 这两种故障的处理方式完全不同。
    if (ui_canvas() == nullptr) return -1;
    if (ui_font(size) == nullptr) return -2;
    if (text == nullptr) return -3;
    if (*text == '\0') return -4;
    if (w <= 0) return -5;

    auto *cb = ui_canvas();
    auto *f = ui_font(size);
    const int len = (int)std::strlen(text);
    std::vector<Line> lines;
    const int n = wrap(f, text, len, (float)w, lines);
    const float lh = line_height > 0 ? (float)line_height : f->height;
    for (int i = 0; i < n; ++i)
    {
        if (lines[i].len <= 0) continue;
        nk_draw_text(cb, nk_rect((float)x, (float)(y + i * (int)lh), (float)w, f->height),
                     text + lines[i].off, lines[i].len, f,
                     nk_rgba(0, 0, 0, 0), ui_rgba(color, 255));
    }
    return n;
}

int32_t rvdl_ui_text_lines(int32_t w, int32_t size, const char *text)
{
    auto *f = ui_font(size);
    if (!f || !text || !*text || w <= 0) return 0;
    std::vector<Line> lines;
    return wrap(f, text, (int)std::strlen(text), (float)w, lines);
}

int32_t rvdl_ui_text_width(int32_t size, const char *text)
{
    auto *f = ui_font(size);
    if (!f || !text) return 0;
    return (int32_t)measure(f, text, (int)std::strlen(text));
}

int32_t rvdl_ui_text_height(int32_t size)
{
    auto *f = ui_font(size);
    return f ? (int32_t)f->height : 16;
}

int32_t rvdl_ui_font_count(void)
{
    return qgui::font_count();
}

int32_t rvdl_ui_font_px(int32_t size)
{
    return (int32_t)qgui::font_px(size);
}

/// 图集里烘进去的**字形数**（0 = 还没烘焙）。
///
/// 用途：核对多字号图集的开销。多字号把显存从 ~6 MB 抬到 ~20 MB，
/// 撑爆 maxImageDimension2D 时只会看到「启动失败」这种毫无线索的错。
///
/// ⚠ 这里报的是字形数，**不是图集宽高**。早先的版本报宽高，
///   但那个回填函数从来没被调用过，于是它恒返回 0 ——
///   而 0 看起来像「图集是空的」，会把人引到完全错误的方向。
///   （nuklear 的 nk_font_atlas_bake 确实会写 atlas->tex_width，
///   但 font_stash_end 之后拿不到它。）
///   字形数至少能确认「真的烘进去了」，而不是「字体对象在但图集是空的」。
int32_t rvdl_ui_atlas_info(void)
{
    return qgui::atlas_info();
}

// ═══════════════════════════════════════════════════════════════════════
// 形状
// ═══════════════════════════════════════════════════════════════════════

int32_t rvdl_ui_rect_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, int32_t alpha)
{
    auto *cb = ui_canvas();
    if (!cb || w <= 0 || h <= 0) return 0;
    nk_byte a = (nk_byte)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
    nk_fill_rect(cb, nk_rect((float)x, (float)y, (float)w, (float)h), 0.f, ui_rgba(color, a));
    return 1;
}

int32_t rvdl_ui_clip(int32_t x, int32_t y, int32_t w, int32_t h)
{
    auto *cb = ui_canvas();
    if (!cb || w <= 0 || h <= 0) return 0;
    // 保存当前裁剪矩形，供 clip_end 恢复
    if (clip_depth() < kClipStackMax)
    {
        clip_stack(clip_depth()) = clip_current();
        clip_depth() = clip_depth() + 1;
    }
    nk_push_scissor(cb, nk_rect((float)x, (float)y, (float)w, (float)h));
    return 1;
}

int32_t rvdl_ui_clip_end(void)
{
    auto *cb = ui_canvas();
    if (!cb) return 0;
    if (clip_depth() > 0) clip_depth() = clip_depth() - 1;
    const ClipEntry r = clip_current();   // 栈底 = 整个窗口
    // 绝不能推 nk_rect(0,0,0,0)：零尺寸会让后续绘制全部被裁掉。
    nk_push_scissor(cb, nk_rect(r.x, r.y, r.w, r.h));
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════
// 输入
// ═══════════════════════════════════════════════════════════════════════

int32_t rvdl_ui_key_down(int32_t key)
{
    nk_context *c = ui_ctx();
    if (!c || key < 0 || key >= NK_KEY_MAX) return 0;
    return nk_input_is_key_down(&c->input, (enum nk_keys)key) ? 1 : 0;
}

int32_t rvdl_ui_key_press(int32_t key)
{
    if (key < 0 || key >= NK_KEY_MAX) return 0;
    auto &e = edges();
    const int down = rvdl_ui_key_down(key);
    const int was = e.keys[key] ? 1 : 0;
    e.keys[key] = (unsigned char)(down ? 1 : 0);
    return (down && !was) ? 1 : 0;
}

int32_t rvdl_ui_key_sync(void)
{
    nk_context *c = ui_ctx();
    if (!c) return 0;
    auto &e = edges();
    for (int k = 0; k < NK_KEY_MAX; ++k)
        e.keys[k] = (unsigned char)(nk_input_is_key_down(&c->input, (enum nk_keys)k) ? 1 : 0);
    return 1;
}

int32_t rvdl_ui_char(void)
{
    // 游标按帧号重置：Nuklear 每帧把本帧的 Unicode 输入重写进
    // keyboard.text，rvdl 每帧最多取 NK_INPUT_MAX 个。
    static int cursor = 0;
    static int64_t seen = -1;
    const int64_t f = timing().frame;
    if (seen != f) { seen = f; cursor = 0; }

    nk_context *c = ui_ctx();
    if (!c) return -1;
    auto &kb = c->input.keyboard;
    if (cursor >= kb.text_len) return -1;

    const unsigned char b0 = (unsigned char)kb.text[cursor];
    int cl = utf8_len(b0);
    if (cursor + cl > kb.text_len) cl = 1;
    nk_rune rune = 0;
    switch (cl)
    {
        case 2:
            rune = (nk_rune)(((b0 & 0x1Fu) << 6) |
                             ((unsigned char)kb.text[cursor + 1] & 0x3Fu));
            break;
        case 3:
            rune = (nk_rune)(((b0 & 0x0Fu) << 12) |
                             (((unsigned char)kb.text[cursor + 1] & 0x3Fu) << 6) |
                             ((unsigned char)kb.text[cursor + 2] & 0x3Fu));
            break;
        case 4:
            rune = (nk_rune)(((b0 & 0x07u) << 18) |
                             (((unsigned char)kb.text[cursor + 1] & 0x3Fu) << 12) |
                             (((unsigned char)kb.text[cursor + 2] & 0x3Fu) << 6) |
                             ((unsigned char)kb.text[cursor + 3] & 0x3Fu));
            break;
        default:
            rune = b0;
            break;
    }
    cursor += cl;
    return (int32_t)rune;
}

int32_t rvdl_ui_char_pending(void)
{
    nk_context *c = ui_ctx();
    return c ? (int32_t)c->input.keyboard.text_len : 0;
}

int32_t rvdl_ui_scroll(void)
{
    nk_context *c = ui_ctx();
    if (!c) return 0;
    auto &sd = c->input.mouse.scroll_delta;
    const float d = (sd.x != 0.f) ? sd.x : sd.y;
    sd.x = 0.f;
    sd.y = 0.f;
    return (int32_t)(-d);
}

int32_t rvdl_ui_mouse_x(void)
{
    nk_context *c = ui_ctx();
    return c ? (int32_t)c->input.mouse.pos.x : 0;
}

int32_t rvdl_ui_mouse_y(void)
{
    nk_context *c = ui_ctx();
    return c ? (int32_t)c->input.mouse.pos.y : 0;
}

int32_t rvdl_ui_mouse_down(void) { return mouse_down(); }

int32_t rvdl_ui_mouse_press(void)
{
    auto &e = edges();
    const int down = mouse_down();
    const int was = e.mouse ? 1 : 0;
    e.mouse = (unsigned char)(down ? 1 : 0);
    return (down && !was) ? 1 : 0;
}

// ═══════════════════════════════════════════════════════════════════════
// 诊断
// ═══════════════════════════════════════════════════════════════════════

int32_t rvdl_ui_probe(void)
{
    int32_t r = 0;
    if (ui_canvas()) r |= 1;
    auto *f = ui_font(qgui::FONT_BODY);
    if (f)
    {
        r |= 2;
        r |= ((int32_t)f->height & 0xFF) << 8;
    }
    // 已有字号档位数放到 16..19 位，一眼能看出多字号有没有真的烘焙成功
    r |= (qgui::font_count() & 0xF) << 16;
    return r;
}

// ═══════════════════════════════════════════════════════════════════════
// 图标
// ═══════════════════════════════════════════════════════════════════════
//
// 场是 32×32 的栈上缓冲，所有图标共用 —— 不需要 malloc。
// 每帧最多重画一次（图标是静态的，但 Nuklear 的命令缓冲每帧重建，
// 所以「缓存」只能省掉 SDF 求值，省不掉命令生成）。

int32_t rvdl_icon_build(int32_t id)
{
    return qicon_build(id, &icon_field());
}

int32_t rvdl_icon_draw(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, float alpha_scale)
{
    auto *cb = ui_canvas();
    if (!cb) return 0;
    return qicon_draw(cb, (float)x, (float)y, (float)w, (float)h, &icon_field(), color, alpha_scale);
}

int32_t rvdl_icon_draw_count(void) { return qicon_draw_count(&icon_field()); }

int32_t rvdl_icon_area(void)
{
    int32_t n = 0;
    const qicon_field &f = icon_field();
    for (int i = 0; i < QICON_CELLS; ++i)
        if (f.d[i] < 0.f) n++;
    return n;
}

int32_t rvdl_icon_size(int32_t id) { return qicon_default_size(id); }

// ═══════════════════════════════════════════════════════════════════════
// 组合代数
// ═══════════════════════════════════════════════════════════════════════

/// 草稿场。布尔运算都是二元的，必须有两个操作数，
/// 所以「当前画的那个」放这里，rvdl_icon_mix 再把它合进主场。
static qicon_field &icon_scratch()
{
    static qicon_field f;
    return f;
}

void rvdl_icon_reset(void)
{
    qicon_clear(&icon_field());
    qicon_clear(&icon_scratch());
}

void rvdl_icon_fill(float v) { qicon_fill(&icon_field(), v); }

int32_t rvdl_icon_shape_rect(float x, float y, float w, float h, float r)
{
    return qicon_rect(&icon_scratch(), x, y, w, h, r);
}

int32_t rvdl_icon_shape_circle(float cx, float cy, float r)
{
    return qicon_circle(&icon_scratch(), cx, cy, r);
}

int32_t rvdl_icon_shape_ring(float cx, float cy, float r, float thickness)
{
    return qicon_ring(&icon_scratch(), cx, cy, r, thickness);
}

int32_t rvdl_icon_shape_segment(float x0, float y0, float x1, float y1, float thickness)
{
    return qicon_segment(&icon_scratch(), x0, y0, x1, y1, thickness);
}

int32_t rvdl_icon_shape_triangle(float x0, float y0, float x1, float y1, float x2, float y2)
{
    return qicon_triangle(&icon_scratch(), x0, y0, x1, y1, x2, y2);
}

int32_t rvdl_icon_shape_star(float cx, float cy, float r_outer, float r_inner, int32_t points,
                           float rot)
{
    return qicon_star(&icon_scratch(), cx, cy, r_outer, r_inner, points, rot);
}

int32_t rvdl_icon_mix(int32_t op)
{
    qicon_field *a = &icon_field();
    qicon_field *b = &icon_scratch();
    switch (op)
    {
        case QICON_OP_SUBTRACT: qicon_subtract(a, b); break;
        case QICON_OP_INTERSECT: qicon_intersect(a, b); break;
        case QICON_OP_SMOOTH: qicon_smooth_union(a, b, 2.f); break;
        default: qicon_union(a, b); break;
    }
    // 草稿用完必须清空，否则下一次 shape_* 是在上一块草稿上接着画，
    // 两个形状会糊成一个 —— 这种 bug 在画面上只是「图标变胖」，
    // 不会报错，很难查。
    qicon_clear(&icon_scratch());
    // 返回**面积**而不是矩形数：矩形数只反映水平段个数，
    // 跟面积没有单调关系，拿它判断「并集有没有变大」会得出错结论。
    return rvdl_icon_area();
}

int32_t rvdl_icon_rotate(float cx, float cy, float angle)
{
    qicon_field dst;
    if (!qicon_rotate(&dst, &icon_field(), cx, cy, angle)) return 0;
    std::memcpy(icon_field().d, dst.d, sizeof(dst.d));
    return 1;
}

int32_t rvdl_icon_scale(float cx, float cy, float sx, float sy)
{
    qicon_field dst;
    if (!qicon_scale(&dst, &icon_field(), cx, cy, sx, sy)) return 0;
    std::memcpy(icon_field().d, dst.d, sizeof(dst.d));
    return 1;
}

int32_t rvdl_icon_tint(int32_t rgb, float hue_deg, float lightness, float contrast)
{
    return qicon_tint(rgb, hue_deg, lightness, contrast);
}

} // extern "C"
