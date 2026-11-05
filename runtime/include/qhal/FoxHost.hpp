#pragma once
//
// qhal/FoxHost.hpp —— rvdl 经典 GUI 的 fox 宿主 + 后端选择策略
//
// 目标：`rvdl_cgui_*` / `rvdl_cgfx_*` 优先用 fox，fox 不行才用 Nuklear。
//
// 为什么是**运行时**判定，而不是编译期二选一
// --------------------------------------------
// 需要回退的那些原因 —— 驱动崩溃、扩展缺失、窗口创建失败、swapchain 重建
// 失败 —— 全都只在运行时才知道。编译期开关只能表达"我们**猜**这台机器不
// 行"，那既不能覆盖真实故障，又会让"fox 可用"这件事失去证据。
//
// 于是这里是三个部件：
//
//   1. `FoxApi`  —— 宿主用到的 fox C ABI 那一小片，函数指针表。真实默认值
//      就是真 fox（`fox_real_api()`），测试可以给假实现。
//   2. `BackendPolicy` —— **纯状态机**。不碰 fox、不碰窗口、不碰 GPU，
//      只处理"初始化结果"和"帧故障计数"两类输入，输出"这一帧谁来画"。
//   3. `FoxHost` —— 把窗口、fox 上下文、布局游标和策略缝在一起。
//
// 只依赖 `fox.h`（C ABI）：不引入 Vulkan 类型、不引入 GLFW 类型，因此可以
// 在没有 GPU / 没有窗口 / 没有 Vulkan 头文件的机器上被纯逻辑测试驱动。
// 窗口本身是一个接缝（`WindowFactory`）：真实实现是 GLFW，测试给假的。
//
// 三条不可退让的规则
// ------------------
//   · **失败即失败**：`fox_create` 返回 null、`fox_init_backend_vulkan`
//     返回 0、`fox_backend_ready` 为 0 —— 三条都是失败，都走 Nuklear。
//   · **绝不静默**：每一次"为什么走这条路"的决定都会经过 `report`（默认
//     打到 stderr）。静默回退比没有回退更糟：用户会以为自己正在看 fox。
//   · **不回头**：一旦判死 fox 就不再重试。"再试一次"意味着用户可能在 fox
//     确实不可渲染时看到一块空白窗口，而没有任何可观测的理由。
//
// 切走时那一帧怎么办：显示列表在 rvdl 侧**每帧重建**，所以不需要迁移任何
// 状态 —— 下一帧的绘制调用全部落到 Nuklear 上，内容自然重画一遍。策略因此
// 只在帧边界上翻转，绝不在帧中途换渲染器。

#include "ciUd/fox/fox.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace qhal
{
    // ── 宿主用到的 fox C ABI ────────────────────────────────────────────
    //
    // 函数指针表，默认值不在这里给：默认值会**引用**真 fox 的符号，于是任何
    // include 了这个头文件的测试都必须链接整个 fox（含 Vulkan）。要真实
    // 实现就调 `fox_real_api()`，测试自己拼一张表。
    struct FoxApi
    {
        // 生命周期
        fox_ctx *(*create)(const fox_config *) = nullptr;
        void (*destroy)(fox_ctx *) = nullptr;
        int (*init_backend_vulkan)(fox_ctx *, void *) = nullptr;
        int (*backend_ready)(fox_ctx *) = nullptr;
        const char *(*error)(fox_ctx *) = nullptr;

        // 帧
        int (*should_close)(fox_ctx *) = nullptr;
        void (*poll_events)(fox_ctx *) = nullptr;
        void (*begin_frame)(fox_ctx *) = nullptr;
        void (*end_frame)(fox_ctx *) = nullptr;
        int32_t (*width)(fox_ctx *) = nullptr;
        int32_t (*height)(fox_ctx *) = nullptr;
        uint32_t (*frame_faults)(fox_ctx *) = nullptr;
        const char *(*last_fault)(fox_ctx *) = nullptr;
        uint32_t (*last_frame_dropped)(fox_ctx *) = nullptr;

        // 输入
        float (*mouse_x)(fox_ctx *) = nullptr;
        float (*mouse_y)(fox_ctx *) = nullptr;
        int (*mouse_down)(fox_ctx *, fox_mouse_button) = nullptr;

        // 主题
        fox_color (*theme_color)(fox_ctx *, fox_color_token) = nullptr;
        float (*theme_metric)(fox_ctx *, int32_t) = nullptr;

        // 控件与图元
        float (*label)(fox_ctx *, fox_rect, const char *, fox_color, fox_align) = nullptr;
        int (*button)(fox_ctx *, fox_rect, const char *, fox_button_style, int) = nullptr;
        void (*panel)(fox_ctx *, fox_rect, fox_color, float, int) = nullptr;
        void (*fill_rect)(fox_ctx *, fox_rect, fox_color) = nullptr;
        void (*fill_ellipse)(fox_ctx *, fox_rect, fox_color) = nullptr;
        void (*fill_triangle)(fox_ctx *, float, float, float, float, float, float,
                              fox_color) = nullptr;
        int (*path)(fox_ctx *, const float *, int32_t, float, fox_color, int, int) = nullptr;

        /// 宿主还要知道 GPU 是谁：A2a 的策略按 GPU 类别分档（Tile / Immediate），
        /// 而"这是哪块 GPU"只有驱动知道。空串 / 0 = 还没后端。
        const char *(*gpu_name)(fox_ctx *) = nullptr;
        uint32_t (*gpu_vendor)(fox_ctx *) = nullptr;

        /// 质量档（热压力下由宿主通过 A2a 下调）。见 utils/A2a.hpp。
        void (*set_quality)(fox_ctx *, int, int, int) = nullptr;
    };

    /// 真实（链接 fox 的）实现的 API 表。只有真的要用 fox 的翻译单元才
    /// ODR-use 这个函数 —— 测试不调用它，因此不需要链接 fox。
    inline FoxApi fox_real_api()
    {
        FoxApi a;
        a.create = ::fox_create;
        a.destroy = ::fox_destroy;
        a.init_backend_vulkan = ::fox_init_backend_vulkan;
        a.backend_ready = ::fox_backend_ready;
        a.error = ::fox_error;
        a.should_close = ::fox_should_close;
        a.poll_events = ::fox_poll_events;
        a.begin_frame = ::fox_begin_frame;
        a.end_frame = ::fox_end_frame;
        a.width = ::fox_width;
        a.height = ::fox_height;
        a.frame_faults = ::fox_frame_faults;
        a.last_fault = ::fox_last_fault;
        a.last_frame_dropped = ::fox_last_frame_dropped;
        a.mouse_x = ::fox_mouse_x;
        a.mouse_y = ::fox_mouse_y;
        a.mouse_down = ::fox_mouse_down;
        a.theme_color = ::fox_theme_color;
        a.theme_metric = ::fox_theme_metric;
        a.label = ::fox_label;
        a.button = ::fox_button;
        a.panel = ::fox_panel;
        a.fill_rect = ::fox_fill_rect;
        a.fill_ellipse = ::fox_fill_ellipse;
        a.fill_triangle = ::fox_fill_triangle;
        a.path = ::fox_path;
        a.gpu_name = ::fox_gpu_name;
        a.gpu_vendor = ::fox_gpu_vendor;
        a.set_quality = ::fox_set_quality;
        return a;
    }

    // ── 后端选择策略：纯状态机 ──────────────────────────────────────────
    //
    // 输入只有两类：初始化结果、帧故障计数。没有 fox 调用、没有窗口、没有
    // GPU —— 所以"正常路径 / init 失败 / 运行中失败 / 失败后不再重试"这四条
    // 全都可以用假 API 精确驱动。
    class BackendPolicy
    {
    public:
        enum class Backend
        {
            Uninitialized, ///< 还没试过 fox
            Fox,           ///< fox 负责本帧
            Nuklear,       ///< 已判定 fox 不可用；原因在 reason()
        };

        /// 初始化失败分成四类，因为对用户的意义完全不同：
        /// 驱动/Vulkan 不可用 vs 窗口建不出来，是很不一样的两件事。
        enum class InitResult
        {
            Ok,
            CreateFailed,  ///< fox_create 返回 null
            BackendFailed, ///< fox_init_backend_vulkan 返回 0
            NotReady,      ///< fox_backend_ready 为 0（声称成功却没就绪）
        };

        explicit BackendPolicy(int fault_threshold = 3)
            : threshold_(fault_threshold < 1 ? 1 : fault_threshold) {}

        void note_init(InitResult r, const char *why)
        {
            tried_fox_ = true;
            if (r == InitResult::Ok)
            {
                backend_ = Backend::Fox;
                reason_[0] = '\0';
                return;
            }
            const char *fallback = "fox initialization failed";
            switch (r)
            {
            case InitResult::CreateFailed: fallback = "fox_create returned null"; break;
            case InitResult::BackendFailed: fallback = "fox_init_backend_vulkan returned 0"; break;
            case InitResult::NotReady: fallback = "fox_backend_ready returned 0"; break;
            case InitResult::Ok: break;
            }
            set_reason(why && *why ? why : fallback);
            backend_ = Backend::Nuklear;
        }

        /// fox 报告的**累计**故障帧数（不是本帧增量 —— 累计值不会因为调用
        /// 时机不同而丢事件）。
        void note_frame_faults(uint32_t cumulative, const char *why)
        {
            if (backend_ != Backend::Fox) return; // 已经切走，不再记账
            const int c = (int)cumulative;
            if (c <= seen_faults_) return;
            faults_ += c - seen_faults_;
            seen_faults_ = c;
            if (faults_ >= threshold_)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "fox failed to submit %d frame(s)%s%s",
                              faults_, (why && *why) ? ": " : "",
                              (why && *why) ? why : "");
                set_reason(buf);
                backend_ = Backend::Nuklear;
            }
        }

        Backend backend() const { return backend_; }
        bool use_fox() const { return backend_ == Backend::Fox; }
        /// fox 试过了吗。试过就不该再试 —— 见文件头"不回头"。
        bool fox_tried() const { return tried_fox_; }
        int faults() const { return faults_; }
        int threshold() const { return threshold_; }
        /// 为什么不是 fox。fox 在用时为空串，永远不为 null。
        const char *reason() const { return reason_; }

    private:
        void set_reason(const char *why)
        {
            std::snprintf(reason_, sizeof(reason_), "%s", why ? why : "");
        }

        Backend backend_ = Backend::Uninitialized;
        bool tried_fox_ = false;
        int threshold_ = 3;
        int faults_ = 0;
        int seen_faults_ = 0;
        char reason_[256] = {0};
    };

    // ── 宿主 ────────────────────────────────────────────────────────────
    class FoxHost
    {
    public:
        /// 窗口接缝。函数指针 + user 指针，而不是接口类：没有虚表、没有
        /// 分配，测试提供两个函数就能驱动。GLFW 只是**其中一种**实现。
        struct WindowFactory
        {
            void *(*create)(int w, int h, const char *title, void *user) = nullptr;
            void (*destroy)(void *window, void *user) = nullptr;
            void *user = nullptr;
        };

        struct Config
        {
            int width = 640;
            int height = 480;
            const char *title = "Classic GUI";
            /// 连续多少帧提交失败就切走。3 是个折中：一次偶发（比如 resize
            /// 竞态）不至于切走，而三帧（~50ms）之后继续画是没有意义的。
            int fault_threshold = 3;
            /// 每一次决定都经过这里。默认写 stderr —— 绝不静默。
            void (*report)(const char *line, void *user) = nullptr;
            void *report_user = nullptr;
        };

        FoxHost() = default;
        FoxHost(const FoxApi &api, const WindowFactory &wf) : api_(&api), wf_(wf) {}
        FoxHost(const FoxHost &) = delete;
        FoxHost &operator=(const FoxHost &) = delete;
        ~FoxHost() { shutdown(); }

        // ── 生命周期 ────────────────────────────────────────────────────
        bool init(const FoxApi &api, const WindowFactory &wf, const Config &cfg)
        {
            api_ = &api;
            wf_ = wf;
            return init(cfg);
        }

        bool init(const Config &cfg)
        {
            if (ctx_) return true; // 已经在跑
            if (attempted_)
            {
                // 只允许一次初始化。两个理由：fox 失败之后**不重试**（见文件头
                // "不回头"），而成功之后再 init 属于"换一个宿主"的语义 —— 没有
                // 可观测的理由支持第二种读法，所以拒绝，并且说明为什么。
                if (policy_.reason()[0]) report(policy_.reason());
                else report("FoxHost::init called twice (one attempt per host)");
                return false;
            }
            attempted_ = true;
            cfg_ = cfg;
            policy_ = BackendPolicy(cfg.fault_threshold);

            if (!api_ || !wf_.create || !wf_.destroy)
            {
                // ⚠ 必须连 wf_.destroy 一起查：下面失败路径会**无条件**调
                // wf_.destroy(window_, ...)，一个只提供 create 不提供 destroy
                // 的工厂 → 空函数指针调用 → 崩溃。
                policy_.note_init(BackendPolicy::InitResult::CreateFailed,
                                  "no window factory configured");
                report("fox unavailable: no window factory configured; using Nuklear");
                return false;
            }

            window_ = wf_.create(cfg.width, cfg.height, cfg.title, wf_.user);
            if (!window_)
            {
                policy_.note_init(BackendPolicy::InitResult::CreateFailed,
                                  "window creation failed");
                report(policy_.reason());
                return false;
            }

            fox_config fc;
            fc.title = cfg.title;
            fc.width = cfg.width;
            fc.height = cfg.height;
            ctx_ = api_->create(&fc);
            if (!ctx_)
            {
                policy_.note_init(BackendPolicy::InitResult::CreateFailed,
                                  "fox_create returned null");
                report(policy_.reason());
                if (wf_.destroy) wf_.destroy(window_, wf_.user);
                window_ = nullptr;
                return false;
            }

            if (!api_->init_backend_vulkan(ctx_, window_))
            {
                // fox 自己已经把具体原因写进了 fox_error()（哪一步失败、
                // 是扩展缺失还是驱动拒绝），照搬比重新措辞准确。
                policy_.note_init(BackendPolicy::InitResult::BackendFailed,
                                  api_->error ? api_->error(ctx_) : nullptr);
                report(policy_.reason());
                shutdown();
                return false;
            }

            if (!api_->backend_ready || api_->backend_ready(ctx_) == 0)
            {
                policy_.note_init(BackendPolicy::InitResult::NotReady, nullptr);
                report(policy_.reason());
                shutdown();
                return false;
            }

            policy_.note_init(BackendPolicy::InitResult::Ok, nullptr);
            report("fox backend active");
            return true;
        }

        void shutdown()
        {
            // 顺序是规范要求，不是习惯：后端销毁 VkSurfaceKHR 与 VkInstance，
            // 窗口是它们赖以存在的对象，必须先走。
            if (ctx_ && api_)
            {
                if (api_->destroy) api_->destroy(ctx_);
                ctx_ = nullptr;
            }
            if (window_ && wf_.destroy)
            {
                wf_.destroy(window_, wf_.user);
                window_ = nullptr;
            }
            frame_open_ = false;
        }

        // ── 帧 ──────────────────────────────────────────────────────────

        /// 尝试用 fox 开始这一帧。
        ///
        /// true  = fox 画这一帧（已经 poll 事件、已经 begin_frame，布局域
        ///         重置为整个窗口）。
        /// false = fox 已被判死（原因在 reason()），调用方**必须**改用
        ///         Nuklear；此时宿主一个 fox 调用都没做。
        bool begin_frame()
        {
            if (!ctx_ || !policy_.use_fox()) return false;
            api_->poll_events(ctx_);
            api_->begin_frame(ctx_);

            // 每帧重建布局域：窗口尺寸可能已经变了（poll_events 里更新）。
            const float w = (float)api_->width(ctx_);
            const float h = (float)api_->height(ctx_);
            area_ = Area{0.0f, 0.0f, w > 0.0f ? w : 0.0f, h > 0.0f ? h : 0.0f};
            depth_ = 0;
            overflow_ = 0;
            cursor_y_ = area_.y;
            col_ = 0;
            cols_ = 1;
            cell_w_ = area_.w;
            row_h_ = metric(FOX_METRIC_CONTROL_HEIGHT, 28.0f);
            spacing_ = metric(FOX_METRIC_SPACING_SM, 6.0f);
            frame_open_ = true;
            return true;
        }

        void end_frame()
        {
            if (!frame_open_) return;
            frame_open_ = false;
            api_->end_frame(ctx_);

            const uint32_t total = api_->frame_faults ? api_->frame_faults(ctx_) : 0;
            if ((int)total != faults_seen_)
            {
                faults_seen_ = (int)total;
                const char *why = api_->last_fault ? api_->last_fault(ctx_) : "";
                policy_.note_frame_faults(total, why);
                if (!policy_.use_fox())
                {
                    // 切走发生在**下一帧的 begin_frame**：这一帧已经提交过，
                    // 而 rvdl 每帧重建显示列表，所以下一帧在 Nuklear 上重画
                    // 一遍就是完整的恢复，不需要搬任何状态。
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "fox: %s; switching to Nuklear on the next frame",
                                  reason());
                    report(buf);
                }
            }
        }

        // ── 状态查询 ────────────────────────────────────────────────────
        bool active() const { return ctx_ && policy_.use_fox(); }
        int faults() const { return policy_.faults(); }
        const char *reason() const { return policy_.reason(); }
        uint32_t last_dropped() const
        {
            return (ctx_ && api_ && api_->last_frame_dropped) ? api_->last_frame_dropped(ctx_) : 0;
        }
        /// 底层 fox 上下文（后端未启动时为 nullptr）。
        ///
        /// 宿主需要它：只有这个上下文带窗口、带后端，它的显示列表才会被
        /// 呈现。宿主自建的那个 fox 上下文是**无窗**的，画进去的东西没人画。
        fox_ctx *fox_ctx_handle() const { return ctx_; }

        bool should_close() const
        {
            // ⚠ 旧代码：`!ctx_ || !api_->should_close || ...` ——
            //    「没有 fox 上下文」被当成「用户要关闭」。
            //
            //    在回退到 Nuklear 的路径上（fox 创建失败 → ctx_ == nullptr），
            //    fox_should_close() 恒为真 → 应用第一帧就退出。这与文件头
            //    「fox 不行才用 Nuklear」的策略直接矛盾。
            //
            //    「没有 fox」不是「用户要关闭」—— 两件事，分开。
            if (!ctx_) return false;
            if (!api_->should_close) return false;
            return api_->should_close(ctx_) != 0;
        }
        int width() const { return (ctx_ && api_->width) ? (int)api_->width(ctx_) : cfg_.width; }
        int height() const { return (ctx_ && api_->height) ? (int)api_->height(ctx_) : cfg_.height; }
        float mouse_x() const { return (ctx_ && api_->mouse_x) ? api_->mouse_x(ctx_) : 0.0f; }
        float mouse_y() const { return (ctx_ && api_->mouse_y) ? api_->mouse_y(ctx_) : 0.0f; }
        bool mouse_left_down() const
        {
            return ctx_ && api_->mouse_down && api_->mouse_down(ctx_, FOX_MOUSE_LEFT) != 0;
        }
        const char *gpu_name() const
        {
            const char *n = (ctx_ && api_->gpu_name) ? api_->gpu_name(ctx_) : nullptr;
            return n ? n : "";
        }
        uint32_t gpu_vendor() const
        {
            return (ctx_ && api_->gpu_vendor) ? api_->gpu_vendor(ctx_) : 0u;
        }
        void set_quality(int blur, int shadows, int animations)
        {
            if (ctx_ && api_ && api_->set_quality)
                api_->set_quality(ctx_, blur, shadows, animations);
        }

        // ── 布局（对应 cgui_row / cgui_panel 的语义）────────────────────
        //
        // Nuklear 的行布局是"给一个行高和列数，控件从左到右填满一行，填满
        // 自动换行"。这里保持同一套语义，因为 rvdl 侧的调用序列就是照它写
        // 的（panel → row(N,H) → N 个控件 → …）。
        void row(int cols, int height)
        {
            if (!active()) return;
            // 上一行没填满就换行：游标先推进一行，否则两组控件会叠在一起。
            if (col_ > 0) cursor_y_ += row_h_ + spacing_;
            cols_ = cols > 0 ? cols : 1;
            cell_w_ = area_.w / (float)cols_;
            row_h_ = height > 0 ? (float)height : metric(FOX_METRIC_CONTROL_HEIGHT, 28.0f);
            col_ = 0;
        }

        void panel(int x, int y, int w, int h, const char *title)
        {
            if (!active()) return;
            push_area();

            const float pad = metric(FOX_METRIC_SPACING_SM, 6.0f);
            const float radius = metric(FOX_METRIC_CORNER_PANEL, 10.0f);
            const float fw = (float)w, fh = (float)h;
            if (api_->panel)
                api_->panel(ctx_, fox_rect{(float)x, (float)y, fw, fh}, theme(FOX_COLOR_CONTROL_BG),
                            radius, 0);

            float title_h = 0.0f;
            if (title && *title && api_->label)
            {
                title_h = metric(FOX_METRIC_CONTROL_HEIGHT_SMALL, 22.0f);
                api_->label(ctx_, fox_rect{(float)x + pad, (float)y + pad, fw - 2.0f * pad, title_h},
                            title, theme(FOX_COLOR_TEXT), FOX_ALIGN_START);
            }

            area_ = Area{(float)x + pad, (float)y + pad + title_h + pad, fw - 2.0f * pad,
                         fh - 2.0f * pad - title_h - pad};
            cursor_y_ = area_.y;
            col_ = 0;
            cols_ = 1;
            cell_w_ = area_.w;
            row_h_ = metric(FOX_METRIC_CONTROL_HEIGHT, 28.0f);
        }

        void panel_end()
        {
            if (!active()) return;
            pop_area();
        }

        // ── 控件 ────────────────────────────────────────────────────────

        /// 画一个按钮，返回本帧是否被点击。矩形由行布局给。
        bool button(const char *label)
        {
            fox_rect r;
            if (!active() || !next_cell(r)) return false;
            return api_->button && api_->button(ctx_, r, label ? label : "",
                                                FOX_BUTTON_TINTED, 1) != 0;
        }

        void text(const char *text)
        {
            fox_rect r;
            if (!active() || !next_cell(r)) return;
            if (api_->label)
                api_->label(ctx_, r, text ? text : "", theme(FOX_COLOR_TEXT), FOX_ALIGN_START);
        }

        void text_int(int32_t value)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", value);
            text(buf);
        }

        // ── cgfx 原语 ───────────────────────────────────────────────────
        //
        // 颜色约定与 Nuklear 路径一致：0xRRGGBB + alpha 0..255。
        void fill_rect(int x, int y, int w, int h, int rgb, int alpha)
        {
            if (!active() || !api_->fill_rect) return;
            api_->fill_rect(ctx_, fox_rect{(float)x, (float)y, (float)w, (float)h},
                            rgba(rgb, alpha));
        }

        void stroke_line(float x0, float y0, float x1, float y1, float thickness, int rgb,
                         int alpha)
        {
            if (!active() || !api_->path) return;
            const float pts[4] = {x0, y0, x1, y1};
            api_->path(ctx_, pts, 2, thickness > 0.0f ? thickness : 1.0f, rgba(rgb, alpha), 1, 1);
        }

        /// 折线（多段连续路径，圆帽 + 圆角连接）。
        ///
        /// 为什么不能是「调用方自己循环调 stroke_line」：逐段独立描边会在
        /// 每个接缝处留下对接误差（两段各画各的帽，夹角处出现缺口或重叠），
        /// 且每段一个实例；push_path 一条命令展开全部段，接缝由圆角连接
        /// 统一处理。这正是勾选框从「16 个圆点」变成 2 个实例的那条路。
        /// `xy` 是 2*count 个 float（x0,y0,x1,y1,…），count < 2 时什么都不画。
        void stroke_polyline(const float *xy, int count, float thickness, int rgb, int alpha)
        {
            if (!active() || !api_->path || !xy || count < 2) return;
            api_->path(ctx_, xy, count, thickness > 0.0f ? thickness : 1.0f, rgba(rgb, alpha), 1, 1);
        }

        void fill_ellipse(float cx, float cy, float rx, float ry, int rgb, int alpha)
        {
            if (!active() || !api_->fill_ellipse) return;
            api_->fill_ellipse(ctx_, fox_rect{cx - rx, cy - ry, rx * 2.0f, ry * 2.0f},
                               rgba(rgb, alpha));
        }

        void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, int rgb,
                           int alpha)
        {
            if (!active() || !api_->fill_triangle) return;
            api_->fill_triangle(ctx_, x0, y0, x1, y1, x2, y2, rgba(rgb, alpha));
        }

        const BackendPolicy &policy() const { return policy_; }

    private:
        struct Area
        {
            float x, y, w, h;
        };

        void report(const char *line)
        {
            if (cfg_.report) cfg_.report(line, cfg_.report_user);
            else std::fprintf(stderr, "[cgui] %s\n", line);
        }

        float metric(int32_t token, float fallback) const
        {
            if (!ctx_ || !api_->theme_metric) return fallback;
            const float v = api_->theme_metric(ctx_, token);
            return v > 0.0f ? v : fallback;
        }

        fox_color theme(fox_color_token token) const
        {
            if (ctx_ && api_->theme_color) return api_->theme_color(ctx_, token);
            return fox_color{0, 0, 0, 255};
        }

        static fox_color rgba(int rgb, int alpha)
        {
            fox_color c;
            c.r = (uint8_t)((rgb >> 16) & 0xFF);
            c.g = (uint8_t)((rgb >> 8) & 0xFF);
            c.b = (uint8_t)(rgb & 0xFF);
            c.a = (uint8_t)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
            return c;
        }

        bool next_cell(fox_rect &out)
        {
            if (cols_ <= 0 || cell_w_ <= 0.0f) return false;
            out = fox_rect{area_.x + (float)col_ * cell_w_, cursor_y_, cell_w_, row_h_};
            ++col_;
            if (col_ >= cols_)
            {
                col_ = 0;
                cursor_y_ += row_h_ + spacing_;
            }
            return true;
        }

        void push_area()
        {
            // 面板嵌套超过 8 层是调用方的错误（cgui_panel 的用法是"顶层分区"），
            // 但**不能静默错位**：记下"这次 push 没成功"，对应的 panel_end
            // 就不要弹栈 —— 否则后面的面板会弹掉更外层的内容区。
            if (depth_ >= (int)(sizeof(stack_) / sizeof(stack_[0])))
            {
                ++overflow_;
                report("cgui_panel nesting deeper than 8; this panel is not popped by "
                       "its panel_end");
                return;
            }
            stack_[depth_++] = area_;
        }

        void pop_area()
        {
            if (overflow_ > 0)
            {
                --overflow_;
                return;
            }
            if (depth_ <= 0) return;
            const Area saved = stack_[--depth_];
            area_ = saved;
            cursor_y_ = area_.y;
            col_ = 0;
            cols_ = 1;
            cell_w_ = area_.w;
            row_h_ = metric(FOX_METRIC_CONTROL_HEIGHT, 28.0f);
        }

        const FoxApi *api_ = nullptr;
        WindowFactory wf_{};
        Config cfg_{};
        fox_ctx *ctx_ = nullptr;
        void *window_ = nullptr;
        BackendPolicy policy_{};
        int faults_seen_ = 0;
        bool frame_open_ = false;
        bool attempted_ = false;

        // 布局状态
        Area area_{0.0f, 0.0f, 0.0f, 0.0f};
        Area stack_[8] = {};
        int depth_ = 0;
        int overflow_ = 0;
        float cursor_y_ = 0.0f;
        float spacing_ = 6.0f;
        int cols_ = 1;
        int col_ = 0;
        float cell_w_ = 0.0f;
        float row_h_ = 28.0f;
    };

} // namespace qhal
