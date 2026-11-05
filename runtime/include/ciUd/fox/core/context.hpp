#pragma once
// Fox core/context.hpp - 立即模式上下文
//
// 一切都挂在这个对象上。它持有：
//   * 显示列表（这一帧画什么）
//   * 布局游标（控件摆在哪）
//   * 输入状态（用户做了什么）
//   * 控件状态缓存（上一帧的状态——跨帧保留）
//
// 交互模型：立即模式 + 保留状态。控件每帧重新声明（类似 Nuklear /
// Dear ImGui），但每个控件都有稳定的哈希 Id，悬停/焦点/动画状态跨帧
// 保留——macOS 风格的动效和玻璃过渡需要这一点。

#include "core/types.hpp"
#include "draw/display_list.hpp"
#include "image/png.hpp"
#include "input/input.hpp"
#include "layout/layout.hpp"
#include "theme/theme.hpp"
#include "text/font.hpp"
#include "text/text_buffer.hpp"
#include "core/platform.hpp"
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fox
{

    // ── 每控件保留状态 ──────────────────────────────────────────────

    /// 跨帧记住的控件状态。
    struct WidgetState
    {
        f32 hover = 0.0f;      ///< 动画插值的 0..1 悬停量
        f32 active = 0.0f;     ///< 动画插值的 0..1 按压量
        f32 anim = 0.0f;       ///< 自由运行的动画 0..1
        bool hovered = false;  ///< 这一帧指针在其上方
        bool pressed = false;  ///< 这一帧指针按在它上面
        bool focused = false;  ///< 拥有键盘焦点
        f32 scroll = 0.0f;     ///< 滚动偏移（列表、文本区）
        /// 水平滚动，单位像素。
        ///
        /// 与 `scroll` 分开：两个轴的滚动互不干扰。共用一个 float 会让
        /// 每次垂直滚动都顺带重置水平位置，反之亦然。
        f32 scroll_x = 0.0f;
        bool visible = true;   ///< 折叠/移出屏幕的控件跳过命中测试
        int32_t last_frame = -1;///< 最后触碰的帧号（GC 标记）
        f32 last_time = -1e30f; ///< 最后触碰时的 Context::time()（GC 标记）。
        ///                         单位是秒不是帧，水位的含义不会随帧率变化。
        i32 caret = 0;         ///< 文本光标位置（可编辑字段）
        /// 拖拽起点（property 数字控件用）：按下时的鼠标 x 与按下时的值。
        ///
        /// 绝对映射 `value = start_value + (x − start_x) · per_px` 而不是
        /// 逐帧增量累加 —— 增量累加随帧率漂移（掉帧即丢位移），且
        /// InputState::mouse_delta 后端从未填充（死字段），依赖它等于
        /// 拖拽永远不改值。
        f32 drag_start_x = 0.0f;
        f32 drag_start_value = 0.0f;
    };

    // ── 焦点管理 ───────────────────────────────────────────────────

    /// 键盘焦点在每个"层"内只有一个控件 id，这样 Tab 循环干净，
    /// 弹出层可以接管焦点而不从父层抢走。
    class FocusStack
    {
    public:
        void begin_frame();
        /// 按 Tab 顺序注册本帧可获焦的控件。
        void register_widget(Id id, const Rect &r, bool focusable);
        /// 把焦点移到 id（并记录其 Tab 序号）。
        void set_focus(Id id);
        Id focused() const { return focused_; }
        bool has_focus(Id id) const { return id != 0 && id == focused_; }
        /// 焦点移到下一个可获焦控件。有变化返回 true。
        bool focus_next(const Rect &bounds);
        bool focus_prev(const Rect &bounds);
        /// 清除焦点（如点击空白处）。
        void clear_focus() { focused_ = 0; }

        /// 本帧最后注册的控件——即指针下最上层的那个（按绘制顺序注册，
        /// 越晚越靠上）。
        ///
        /// 只有**指针下**的控件才会注册（见 Context::hover），
        /// 所以栈为空表示"指针下没有东西"。
        Id topmost_registered() const
        {
            return order_.empty() ? 0 : order_.back().id;
        }

    private:
        struct Widget
        {
            Id id;
            Rect rect;
        };
        std::vector<Widget> order_;
        Id focused_ = 0;
        int focus_index_ = -1;
        int frame_ = 0;
    };

    // ── Context ────────────────────────────────────────────────────────

    class Context
    {
    public:
        /// 用默认尺寸创建。绘制前先调 frame_begin()。
        bool init(const char *title, int width, int height);
        void shutdown();

        /// 开始一帧：重置显示列表 + 布局，刷新输入。
        void frame_begin();
        /// 结束一帧：把显示列表提交给后端 + 清除输入边沿。
        void frame_end();
        /// 宿主请求 Fox 关闭（通过 fox_set_should_close）。
        ///
        /// 与"Fox 应该关闭"不同（见 should_close()）。
        bool close_requested() const { return close_requested_; }
        /// init() 时传入的窗口标题（不会是 NULL）。
        const char *title() const { return title_; }
        void set_close_requested(bool on) { close_requested_ = on; }

        /// 宿主应当关闭时为 true：宿主请求了，或后端窗口请求了。
        ///
        /// 无头上下文（无窗口——文本度量、离屏渲染）是**合法**模式，
        /// 永远不算"应该关闭"："未初始化"和"用户想退出"是两回事。
        bool should_close() const { return close_requested_ || backend_close_requested_; }

        /// 窗口发出关闭信号时由后端设置。
        void set_backend_close_requested(bool on) { backend_close_requested_ = on; }

        // ── 控件用的访问器 ───────────────────────────────────────
        DisplayList &dl() { return dl_; }
        LayoutSystem &layout() { return layout_; }
        InputState &input() { return input_; }
        FocusStack &focus() { return focus_; }
        Theme &theme() { return fox::theme(); }
        f32 time() const { return time_; }
        f32 dt() const { return dt_; }
        int frame_index() const { return frame_; }

        /// 把 dt 固定为指定值，不再读墙上时钟。
        ///
        /// 两种用途，都需要每帧完全一致：
        ///
        //  · 无头渲染。fox_headless 紧循环驱动 frame_begin，实测 dt 只有
        //    微秒级，动画都停在起始值——开关控件的参考图里旋钮永远在
        //    OFF 位置，跟实际状态无关。
        //
        //  · 确定性回放 / 基准图测试：真实时钟会让每次运行的画面都不同，
        //    diff 就没有意义。
        ///
        /// 传 <= 0 恢复读墙上时钟。dt 仍会截断到正常上限，长时间卡顿也
        /// 不会让动画瞬移。
        void set_fixed_dt(f32 seconds) { fixed_dt_ = seconds; }
        f32 fixed_dt() const { return fixed_dt_; }

        /// 物理帧缓冲尺寸，单位是设备像素。尺寸变化时更新。
        ///
        /// ⚠ 这不是控件布局用的尺寸。布局用 width()/height()，是逻辑尺寸。
        ///   两者差 scale() 倍：2x 显示屏上 1000x680 的窗口逻辑尺寸是
        ///   500x340、设备尺寸是 1000x680。把帧缓冲尺寸传给控件，HiDPI
        ///   屏上每个元素都会变成预期的两倍大，控件会溢出自己的窗口——
        ///   看起来像布局 bug，实际是单位错了。
        void set_framebuffer_size(int w, int h);

        /// 设置逻辑尺寸——API 里所有矩形用的单位。
        ///
        /// Fox 内部会乘 scale()，按逻辑单位思考的宿主不需要知道 DPI。
        /// 原始的物理尺寸 setter 仍保留，给已经持有设备像素的宿主用。
        void set_size_logical(int w, int h)
        {
            const f32 s = scale_ > 0.0f ? scale_ : 1.0f;
            set_framebuffer_size((int)((f32)w * s + 0.5f), (int)((f32)h * s + 0.5f));
        }

        /// 逻辑（DIP）尺寸。所有控件、布局和命中测试都在这个单位下工作，
        /// 与显示密度无关。
        int width() const { return win_w_; }
        int height() const { return win_h_; }

        int framebuffer_width() const { return fb_w_; }
        int framebuffer_height() const { return fb_h_; }

        /// 每逻辑像素对应的物理像素数：普通屏 1.0，典型 Retina 屏 2.0，
        /// Windows 分数缩放 1.5 / 1.25。
        f32 scale() const { return scale_; }

        /// 设置光栅化密度并重新推导逻辑尺寸。
        ///
        /// 把缩放应用到每个长度类主题 token，并让字体管理器按新密度
        /// 光栅化，显示切换后文字和度量是清晰的，而不只是尺寸正确。
        /// 缩放确实变化时返回 true。
        bool set_scale(f32 s);

        // ── Id 生成 ────────────────────────────────────────────
        /// 由 salt + 调用方提供的 token 确定性生成 id。
        /// 给每个控件跨帧稳定的标识。
        Id make_id(Id salt, const char *label) const;
        Id make_id(Id salt, Id n) const { return id_hash(salt, n); }

        // ── 控件状态缓存 ───────────────────────────────────────
        /// 取控件 id 的保留状态（不存在则创建）。
        WidgetState &state(Id id);
        /// 取状态并标记本帧已触碰。
        WidgetState &state_touch(Id id);

        /// 这个矩形是不是指针下最上层的？用于悬停判定。
        /// 控件调用它认领指针；后画的控件拿到 hover=0。
        ///
        /// ⚠ 这个语义现在是**真的**：hover() 自己就实现了「后画的抢走
        /// hover 权」（被压住的控件收回 hover 与按下态）。之前 hover_stack_
        /// 是个死字段、claim_hover 无人调用，于是两个重叠的控件会同时报告
        /// hover=true，被面板盖住的按钮照样高亮。
        bool claim_hover(const Rect &r, Id id);
        /// 点击落在本控件上、且没有被更上层的控件消费掉时，消费这次点击。
        ///
        /// 只有 hover 认领栈的**栈顶**（= 最上层）拿得到点击。
        bool claim_click(const Rect &r, Id id);

        /// 丢弃最近未触碰的控件的保留状态。
        ///
        /// 控件 id 由位置推导（见 text_field），滚动的列表每经过一个行
        /// 位置就发一个新 id，不清理的话每滚一像素就多一个 WidgetState，
        /// 永远涨下去。保留时长足够撑过关闭的标签页或折叠的面板；见
        /// kStateStaleSeconds。
        void prune_state_cache();

        // ── 文本编辑 ──────────────────────────────────────────────

        /// 文本框的编辑模型（光标、选区、撤销历史）。
        ///
        /// 刻意不放进 WidgetState。WidgetState 是所有控件都有的，而这个
        /// 带一个 std::string 和两个历史 vector——几百字节，绝大多数控件
        /// 根本用不到。用同一个控件 id 作 key，按同一水位清理。
        TextBuffer &text_edit(Id id);

        /// 这个 id 有没有文本编辑模型？
        ///
        /// 只查询不创建，与 text_edit() 不同。只想知道控件是不是文本框的
        /// 调用方必须用这个——用 text_edit() 会给一个按钮也分配 TextBuffer
        /// 和两个撤销 vector，还一直留到进程结束。
        bool has_text_edit(Id id) const
        {
            return id != 0 && text_edits_.find(id) != text_edits_.end();
        }

        /// 查看已有的编辑模型，不存在返回 nullptr。
        ///
        /// text_edit() 会**创建** buffer（和撤销栈）——只读查询若用它，
        /// 宿主只是问过的每个 id 都会分配一份。
        const TextBuffer *text_edit_peek(Id id) const
        {
            auto it = text_edits_.find(id);
            return it == text_edits_.end() ? nullptr : &it->second;
        }

        // ── IME ──────────────────────────────────────────────────────
        //
        // 是 Context 的成员方法，不是全局对象上的自由函数。这不是风格
        // 偏好：Fox 进程里有两个 Context——C++ demo 和无头渲染器用的全局
        // 单例，以及 C ABI 每个 fox_ctx 各自持有的一个。IME 入口如果去拿
        // 全局的，会把输入组合送进另一个完全不同的、未初始化的 Context，
        // 症状就是"fox_ime_begin 什么都没做"，而且没有任何报错。
        //
        // `caret_bytes` 是合成中文本内部的偏移，不是文档里的偏移，
        // 会被 buffer 截断。
        void ime_begin(const char *utf8, size_t caret_bytes);
        void ime_update(const char *utf8, size_t caret_bytes);
        void ime_commit();
        void ime_cancel();
        bool ime_composing() const;

        /// 拥有焦点的文本框的编辑模型，可能为 null。设为私有，让"找到
        /// 焦点字段、没有就什么都不做"只有一份实现——五个 IME 入口各写
        /// 一遍就是五次可能忘掉 null 的机会。
        TextBuffer *focused_text_buffer();

        /// 宿主绕过 Fox 改了 buffer。用调用方的字节重新同步编辑模型并
        /// 丢弃历史，因为这是一个新值，不是用户敲出来的。
        ///
        /// text_field 每帧都调用，这就是 `char* buffer` API 还能安全保留
        /// 的原因：直接写 buffer 的宿主仍能覆盖过期的光标/选区状态。
        void sync_text_from_host(TextBuffer &tb, const char *host, size_t host_len);

        /// 请求重绘 / 标记动画处于活动状态。
        void request_redraw() { wants_redraw_ = true; }

        // ── 命中测试（最上层优先）──────────────────────────────
        /// 指针在 r 上方且 r 是最上层悬停控件时返回 true。
        bool hover(const Rect &r, Id id);

        f32 time_seconds() const { return time_; }

        // 后端钩子——Vulkan 后端覆盖这些。
        void (*on_resize)(int w, int h) = nullptr;

        // ── 纹理 ──────────────────────────────────────────────────
        //
        // Fox 的纹理注册表刻意与后端无关：只持有 RGBA8 字节，别的不管。
        // Vulkan 后端每帧把标记为 dirty 的内容同步进 VRAM。控件从不接触
        // VkImage，再加一个后端也不用改这一层以上的代码。
        struct Texture
        {
            u32 w = 0, h = 0;
            std::vector<u8> rgba; ///< w*h*4，直通（非预乘）alpha
            bool dirty = true;    ///< 置位表示后端需重新上传
            bool uploaded = false;///< 数据进入 VRAM 后由后端置位
            u32 generation = 0;   ///< 每次写入递增，供后端使用
            bool alive = false;   ///< destroy_texture 时清零

            /// 可选的 mip 金字塔（image/png.hpp），levels[0] == rgba。
            ///
            /// 常规纹理（字形图集、程序生成的图集）为空：图集每来一个字形
            /// 就重新上传，逐字形重建金字塔会给热路径加负担，而这种纹理
            /// 本来就是 1:1 采样的。PNG 纹理会建金字塔，CPU 采样器会用它
            /// （见 sample_rgba_lod）。
            img::MipChain mips;

            u32 mip_levels() const { return mips.levels.empty() ? 1u : (u32)mips.levels.size(); }
        };

        /// 上传新的 RGBA8 纹理。w/h 为零或注册表满时返回 TEX_NONE。
        /// 像素会被复制，调用方可保留自己的 buffer。
        TexId create_texture(u32 w, u32 h, const u8 *rgba);

        /// 解码 PNG（image/png.cpp）并上传，同时构建 mip 金字塔。
        /// 解码出错返回 TEX_NONE 并打印原因——加载失败的文件不能不吭声
        /// 地变成一块黑矩形。
        TexId create_texture_png(const u8 *png, size_t n, const char *name = nullptr);

        /// 上传 RGBA8 纹理并构建 mip 金字塔（image/png.hpp）。
        ///
        /// 刻意与 create_texture 分开：字形图集每来一个新字形就重新上传、
        /// 1:1 采样，建金字塔只会拖累热路径，没人会去缩小它。金字塔纹理
        /// 用于以未知或变化的缩放绘制的内容（PNG 素材、图集里以多种尺寸
        /// 绘制的图标）。
        TexId create_texture_mipped(u32 w, u32 h, const u8 *rgba);

        /// 覆写已有纹理的像素并标记需要重新上传。
        bool update_texture(TexId id, u32 w, u32 h, const u8 *rgba);

        /// 释放纹理。句柄保持失效；id 不复用，显示列表里过期的 TexId
        /// 最多只是什么都画不出来。
        void destroy_texture(TexId id);

        const Texture *texture(TexId id) const
        {
            if (id == TEX_NONE || id > textures_.size()) return nullptr;
            return textures_[id - 1].alive ? &textures_[id - 1] : nullptr;
        }

        /// 可变版本，供后端确认上传用。单独提供而不是在调用处 const_cast，
        /// 这样"谁可以改它"在签名上就看得见。
        Texture *texture_mut(TexId id)
        {
            if (id == TEX_NONE || id > textures_.size()) return nullptr;
            return textures_[id - 1].alive ? &textures_[id - 1] : nullptr;
        }

        /// 在归一化 (u,v) 处采样 RGBA8 纹理。边缘截断，与采样器的
        /// VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE 一致。
        ///
        /// CPU 光栅化器因此能和 GPU 逐位一致，而不必重新实现采样器：
        /// 1:1 情况下最近邻采样是精确的，边缘截断让越界情况有明确的
        /// 定义，而不是"通常没问题"。
        const u8 *sample_rgba(TexId id, f32 u, f32 v) const
        {
            const Texture *t = texture(id);
            if (!t || t->w == 0 || t->h == 0 || t->rgba.empty()) return nullptr;
            int x = (int)(u * (f32)t->w);
            int y = (int)(v * (f32)t->h);
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= (int)t->w) x = (int)t->w - 1;
            if (y >= (int)t->h) y = (int)t->h - 1;
            return &t->rgba[((size_t)y * t->w + x) * 4];
        }

        /// 带 LOD 采样，即在 mip 金字塔上做三线性插值。
        ///
        /// 纹理没有金字塔时（图集、手写纹理），退化为与 sample_rgba 相同
        /// 的最近邻采样，已有内容的像素不变；带 mip 的纹理（PNG）走
        /// img::sample_trilinear。
        ///
        /// `lod` 是"一个像素覆盖多少纹素的 log2"——由调用方计算，只有
        /// 调用方知道像素足迹（img::mip_lod）。GPU 路径自己的 LOD 选择
        /// 见 backend/vk_backend.cpp 里的 sampler。
        void sample_rgba_lod(TexId id, f32 u, f32 v, f32 lod, u8 out[4]) const
        {
            const Texture *t = texture(id);
            if (!t || t->w == 0 || t->h == 0 || t->rgba.empty())
            {
                out[0] = out[1] = out[2] = out[3] = 0;
                return;
            }
            if (t->mips.levels.size() > 1)
            {
                img::sample_trilinear(t->mips, u, v, lod, out);
                return;
            }
            const u8 *s = sample_rgba(id, u, v);
            out[0] = s[0];
            out[1] = s[1];
            out[2] = s[2];
            out[3] = s[3];
        }

        /// 纹理集合变化时递增，后端据此知道要重写描述符。
        /// 比后端每帧轮询所有纹理的改动更省也更可靠。
        u32 texture_generation() const { return tex_generation_; }

        size_t texture_count() const { return textures_.size(); }

        // ── 字体 ────────────────────────────────────────────────────

        /// 按给定像素大小惰性加载字体（按大小缓存）。控件用它，调用方
        /// 不用操心 FontId 的生命周期。
        ///
        /// `pixel_size` 是逻辑单位。缓存按 DPI 换算后的大小作 key，显示
        /// 切换后拿到的是按新密度光栅化的字面，而不是尺寸对了但发虚的。
        FontId font(f32 pixel_size);

        /// 遮挡剔除开关（默认开）。见 draw/instance.cpp：画家算法下，被一个
        /// **后续的不透明实心矩形**整块盖住的实例可以直接删掉 —— 那些像素
        /// 必然被覆盖。关掉它用于验证："剔除前后的画面必须逐字节相同"。
        bool cull_hidden() const { return cull_hidden_; }
        void set_cull_hidden(bool on) { cull_hidden_ = on; }

        // ── 质量档（热压力下调；宿主经 utils/A2a.hpp 决定）──────────────
        //
        // 三档而不是两个布尔：降级是**分级**的（全 / 半 / 关），"半"是真实
        // 存在的中间状态。fox 自己不读温度、不做策略 —— 那是宿主的事；
        // fox 只负责"被要求降级时行为正确"。
        //
        //   blur      0=全分辨率模糊  1=半分辨率（更省带宽）  2=关（玻璃不再采样背景）
        //   shadows   0=全            1=半径减半            2=关（不生成阴影实例）
        //   animations true=正常       false=动画直接到目标（不插值）
        struct Quality
        {
            int blur = 0;
            int shadows = 0;
            bool animations = true;
        };

        const Quality &quality() const { return quality_; }
        void set_quality(int blur, int shadows, bool animations)
        {
            quality_.blur = blur < 0 ? 0 : (blur > 2 ? 2 : blur);
            quality_.shadows = shadows < 0 ? 0 : (shadows > 2 ? 2 : shadows);
            quality_.animations = animations;
        }

        /// dt to feed animation interpolation.
        ///
        /// With animations off it is astronomically large rather than 0:
        /// approach() is exponential (1 - exp(-dt/tau)), so a huge dt lands
        /// exactly on the target in ONE frame while dt=0 would freeze the
        /// value wherever it happens to be - i.e. "animations off" would mean
        /// "widgets stuck mid-transition", which is the opposite of the intent.
        f32 anim_dt() const { return quality_.animations ? dt_ : 1.0e9f; }

        /// 上一帧剔除的实例数，供统计和测试用。
        u32 hidden_culled() const { return hidden_culled_; }
        /// 由 build_instances() 每帧调用一次。调用方不要用。
        void note_hidden_culled(u32 n) { hidden_culled_ = n; }

        /// 用 `path` 作为主 UI 字体，替代平台默认搜索。传 nullptr 恢复默认。
        ///
        /// 必须在第一个控件请求字体之前调用。
        ///
        /// 值得提供的两个理由：应用可能想自带字体；而且这是唯一能让字体
        /// 回退可观察的手段——平台搜索优先选支持 CJK 的字面（雅黑、
        /// Noto CJK、苹方），多数机器上主字体自己就覆盖了 CJK，回退链
        /// 从不触发，"回退可用"就成了一句从没被测试验证过的话。把主字体
        /// 指向只含拉丁字母的字面，回退链才会真正干活。
        void set_font_file(const char *path) { fonts().set_primary_path(path); }
        /// 当前主题字号的默认正文字体。
        FontId default_font() { return font(theme().font_body); }
        /// 已加载字体的字形图集（由上下文持有）。
        GlyphAtlas *font_atlas(FontId id);

        /// 按给定外观/强调色应用 Golden Gate 主题预设，然后把当前 DPI
        /// 缩放重新应用到每个长度 token。
        void set_theme(Appearance app, Accent ac);

        // ── 弹出层仲裁 ───────────────────────────────────────────
        //
        // 下拉类（菜单弹层、下拉列表）互斥：打开一个必须关掉其他，点击
        // 外部必须关掉打开的那个。这条规则需要一条上下文级状态——"哪个
        // 弹出层开着"——因为每个弹出层只知道它自己。没有它，两个菜单
        // 可以同时开着，谁也不知道该关。
        //
        // 存的是锚点的控件 id（菜单标题 / 下拉框），不是弹层自己的：
        // 锚点才是调用方跨帧持有的东西。

        /// 当前打开的弹出层锚点 id（0 = 没有打开的）。
        Id open_popup() const { return open_popup_; }
        /// 打开弹出层（关掉其他）。传 0 关闭全部。
        void set_open_popup(Id anchor) { open_popup_ = anchor; }
        /// `anchor` 持有当前打开的弹出层时返回 true。
        bool popup_is_open(Id anchor) const { return anchor != 0 && anchor == open_popup_; }

    private:
        DisplayList dl_;
        LayoutSystem layout_;
        InputState input_;
        FocusStack focus_;

        std::unordered_map<Id, WidgetState> state_cache_;

        /// 文本框的编辑模型。与 state_cache_ 分开，因为载荷重且只有
        /// 文本框用——见 text_edit()。
        std::unordered_map<Id, TextBuffer> text_edits_;

        /// 打开的下拉（菜单 / 下拉框）的锚点 id，0 = 没有。
        /// 见上面的弹出层仲裁说明。
        Id open_popup_ = 0;

        // 惰性加载的系统字体，按（像素大小 * 10）作 key。
        std::unordered_map<int, FontId> fonts_;

        /// 回退字面在第一次请求字体时加载一次。为什么是"一次"而不是
        /// "每个字号"，见 FontManager::load_fallback_faces()。
        bool fallbacks_tried_ = false;

        // 纹理注册表。用 deque 而不是 vector：TexId 是下标，vector 重新
        // 分配会挪动后端已持有指针的 Texture 对象（FontFace 的同款教训）。
        std::deque<Texture> textures_;
        u32 tex_generation_ = 0;

        int win_w_ = 800;  ///< 逻辑宽度（framebuffer_width / scale）
        int win_h_ = 600;  ///< 逻辑高度
        int fb_w_ = 800;   ///< 物理宽度（交换链的尺寸）
        int fb_h_ = 600;   ///< 物理高度
        f32 scale_ = 1.0f; ///< 每逻辑像素的物理像素数
        /// 宿主请求关闭（fox_set_should_close）。
        bool close_requested_ = false;
        /// 上一帧指针下的顶层命中控件（点击仲裁用，见 frame_end / claim_click）。
        Id topmost_hit_ = 0;
        /// 窗口标题（init 时传入；之前被直接丢弃）。
        const char *title_ = "Fox";
        /// 窗口自己请求关闭（由后端在 poll_events 里同步进来）。
        bool backend_close_requested_ = false;
        bool wants_redraw_ = false;
        bool cull_hidden_ = true;   ///< 遮挡剔除（见 cull_hidden()）
        u32 hidden_culled_ = 0;     ///< 上一帧剔除掉的实例数
        Quality quality_{};         ///< 热压力下的质量档（见 quality()）

        int frame_ = 0;
        f32 time_ = 0.0f;
        f32 dt_ = 0.0f;
        f32 last_time_ = 0.0f;
        f32 fixed_dt_ = 0.0f; ///< > 0 表示固定 dt、绕过时钟

        // 悬停认领：每帧控件注册，指针下最后认领的"赢"。
        // 每帧跟踪最上层的认领。
        std::vector<std::pair<Id, Rect>> hover_stack_;
    };

    /// 全局唯一上下文。
    Context &ctx();

} // namespace fox