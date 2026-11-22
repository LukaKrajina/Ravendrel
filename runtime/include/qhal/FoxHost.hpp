#pragma once

#include "ciUd/fox/fox.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace qhal
{
    struct FoxApi
    {
        fox_ctx *(*create)(const fox_config *) = nullptr;
        void (*destroy)(fox_ctx *) = nullptr;
        int (*init_backend_vulkan)(fox_ctx *, void *) = nullptr;
        int (*backend_ready)(fox_ctx *) = nullptr;
        const char *(*error)(fox_ctx *) = nullptr;

        int (*should_close)(fox_ctx *) = nullptr;
        void (*poll_events)(fox_ctx *) = nullptr;
        void (*begin_frame)(fox_ctx *) = nullptr;
        void (*end_frame)(fox_ctx *) = nullptr;
        int32_t (*width)(fox_ctx *) = nullptr;
        int32_t (*height)(fox_ctx *) = nullptr;
        uint32_t (*frame_faults)(fox_ctx *) = nullptr;
        const char *(*last_fault)(fox_ctx *) = nullptr;
        uint32_t (*last_frame_dropped)(fox_ctx *) = nullptr;

        float (*mouse_x)(fox_ctx *) = nullptr;
        float (*mouse_y)(fox_ctx *) = nullptr;
        int (*mouse_down)(fox_ctx *, fox_mouse_button) = nullptr;

        fox_color (*theme_color)(fox_ctx *, fox_color_token) = nullptr;
        float (*theme_metric)(fox_ctx *, int32_t) = nullptr;

        float (*label)(fox_ctx *, fox_rect, const char *, fox_color, fox_align) = nullptr;
        int (*button)(fox_ctx *, fox_rect, const char *, fox_button_style, int) = nullptr;
        void (*panel)(fox_ctx *, fox_rect, fox_color, float, int) = nullptr;
        void (*fill_rect)(fox_ctx *, fox_rect, fox_color) = nullptr;
        void (*fill_ellipse)(fox_ctx *, fox_rect, fox_color) = nullptr;
        void (*fill_triangle)(fox_ctx *, float, float, float, float, float, float,
                              fox_color) = nullptr;
        int (*path)(fox_ctx *, const float *, int32_t, float, fox_color, int, int) = nullptr;

        const char *(*gpu_name)(fox_ctx *) = nullptr;
        uint32_t (*gpu_vendor)(fox_ctx *) = nullptr;

        void (*set_quality)(fox_ctx *, int, int, int) = nullptr;
    };

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

    class BackendPolicy
    {
    public:
        enum class Backend
        {
            Uninitialized,
            Fox,
            Nuklear,
        };

        enum class InitResult
        {
            Ok,
            CreateFailed,
            BackendFailed,
            NotReady,
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

        void note_frame_faults(uint32_t cumulative, const char *why)
        {
            if (backend_ != Backend::Fox) return;
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
        bool fox_tried() const { return tried_fox_; }
        int faults() const { return faults_; }
        int threshold() const { return threshold_; }
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

    class FoxHost
    {
    public:
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
            int fault_threshold = 3;
            void (*report)(const char *line, void *user) = nullptr;
            void *report_user = nullptr;
        };

        FoxHost() = default;
        FoxHost(const FoxApi &api, const WindowFactory &wf) : api_(&api), wf_(wf) {}
        FoxHost(const FoxHost &) = delete;
        FoxHost &operator=(const FoxHost &) = delete;
        ~FoxHost() { shutdown(); }

        bool init(const FoxApi &api, const WindowFactory &wf, const Config &cfg)
        {
            api_ = &api;
            wf_ = wf;
            return init(cfg);
        }

        bool init(const Config &cfg)
        {
            if (ctx_) return true;
            if (attempted_)
            {
                if (policy_.reason()[0]) report(policy_.reason());
                else report("FoxHost::init called twice (one attempt per host)");
                return false;
            }
            attempted_ = true;
            cfg_ = cfg;
            policy_ = BackendPolicy(cfg.fault_threshold);

            if (!api_ || !wf_.create || !wf_.destroy)
            {
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


        bool begin_frame()
        {
            if (!ctx_ || !policy_.use_fox()) return false;
            api_->poll_events(ctx_);
            api_->begin_frame(ctx_);

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
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "fox: %s; switching to Nuklear on the next frame",
                                  reason());
                    report(buf);
                }
            }
        }

        bool active() const { return ctx_ && policy_.use_fox(); }
        int faults() const { return policy_.faults(); }
        const char *reason() const { return policy_.reason(); }
        uint32_t last_dropped() const
        {
            return (ctx_ && api_ && api_->last_frame_dropped) ? api_->last_frame_dropped(ctx_) : 0;
        }
        fox_ctx *fox_ctx_handle() const { return ctx_; }

        bool should_close() const
        {
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

        void row(int cols, int height)
        {
            if (!active()) return;
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

}