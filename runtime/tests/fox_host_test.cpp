// ============================================================
// fox_host_test.cpp —— FoxHost + BackendPolicy：纯逻辑驱动，不需要 GPU
//
// 这个测试是 ④（fox 优先 / Nuklear 兜底）的验收标准，它证明的是三件事：
//
//   [1] BackendPolicy 是**纯状态机**：初始化成功 / 三条 init 失败门 /
//       运行中帧故障达到阈值 / 失败后不再重试 —— 全部可以直接断言。
//   [2] FoxHost 只在**真 fox** 上做 fox 调用：用假 FoxApi 记录每一次调用，
//       失败路径上必须"一个 fox 调用都不做"。
//   [3] 失败**绝不静默**：每一条路径都要经过 report()，测试逐条读回来。
//
// 为什么值得这么测：静默回退是本轮最想避免的东西 —— 用户会以为自己正在看
// fox。所以"回退了"这件事不只要发生，还要有可断言的理由字符串。
//
// 运行：
//   clang++ -std=c++20 -O2 -I runtime/include \
//     runtime/tests/fox_host_test.cpp -o /tmp/opencode/foxbuild/fox_host_test
// ============================================================

#include "qhal/FoxHost.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace qhal;

namespace
{

    int g_fail = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) ++g_fail;
    }

    void section(const char *name) { std::printf("\n[%s]\n", name); }

    // 假上下文：永远不解引用，只作为非空指针流转。
    char g_ctx_token = 0;
    fox_ctx *fake_ctx() { return (fox_ctx *)&g_ctx_token; }

    // ── 可编程的假 fox ───────────────────────────────────────────────────
    struct FakeFx
    {
        bool create_null = false;
        bool init_backend_ok = true;
        bool ready = true;
        int backend_fail_kind = 0; // 0=init 返回 0, 1=ready 为 0

        uint32_t faults = 0;
        std::string fault_reason = "vkQueuePresentKHR failed";
        int width = 800, height = 480;
        float mx = 100.0f, my = 60.0f;
        bool left_down = false;
        std::string gpu = "FakeGPU 3000";
        uint32_t vendor = 0xF00D;

        // 记录到的调用
        int create_calls = 0, destroy_calls = 0, init_calls = 0, ready_calls = 0;
        int poll_calls = 0, begin_calls = 0, end_calls = 0;
        int label_calls = 0, button_calls = 0, panel_calls = 0;
        int rect_calls = 0, path_calls = 0, ellipse_calls = 0, tri_calls = 0;
        int quality[3] = {-1, -1, -1};

        fox_rect last_rect{};
        fox_color last_color{};
        fox_color label_color{};
        float label_align = -1.0f;
        fox_rect btn_rects[8]{};
        int btn_count = 0;
        fox_rect label_rects[8]{};
        int label_count = 0;
        fox_rect panel_rects[4]{};
        int panel_count = 0;
        float path_xy[8]{};
        int path_n = 0;
        float path_thickness = 0.0f;
        float tri[6]{};
    };

    FakeFx g;

    fox_color fkc(uint8_t r, uint8_t gg, uint8_t b, uint8_t a) { return fox_color{r, gg, b, a}; }

    // 假窗口：非空即成功。
    char g_win_token = 0;
    int g_win_create_calls = 0, g_win_destroy_calls = 0;

    // ── 假 API 函数 ─────────────────────────────────────────────────────
    fox_ctx *fake_create(const fox_config *) { ++g.create_calls; return g.create_null ? nullptr : fake_ctx(); }
    void fake_destroy(fox_ctx *) { ++g.destroy_calls; }
    int fake_init_backend(fox_ctx *, void *) { ++g.init_calls; return g.init_backend_ok ? 1 : 0; }
    int fake_backend_ready(fox_ctx *) { ++g.ready_calls; return g.ready ? 1 : 0; }
    const char *fake_error(fox_ctx *) { return "create_instance failed: no Vulkan driver"; }
    int fake_should_close(fox_ctx *) { return 0; }
    void fake_poll(fox_ctx *) { ++g.poll_calls; }
    void fake_begin(fox_ctx *) { ++g.begin_calls; }
    void fake_end(fox_ctx *) { ++g.end_calls; }
    int32_t fake_width(fox_ctx *) { return g.width; }
    int32_t fake_height(fox_ctx *) { return g.height; }
    float fake_mouse_x(fox_ctx *) { return g.mx; }
    float fake_mouse_y(fox_ctx *) { return g.my; }
    int fake_mouse_down(fox_ctx *, fox_mouse_button b) { return (b == FOX_MOUSE_LEFT && g.left_down) ? 1 : 0; }
    uint32_t fake_faults(fox_ctx *) { return g.faults; }
    const char *fake_last_fault(fox_ctx *) { return g.fault_reason.c_str(); }
    uint32_t fake_dropped(fox_ctx *) { return 0; }
    fox_color fake_theme_color(fox_ctx *, fox_color_token) { return fkc(20, 20, 25, 255); }
    float fake_theme_metric(fox_ctx *, int32_t token)
    {
        switch (token)
        {
        case FOX_METRIC_CONTROL_HEIGHT: return 28.0f;
        case FOX_METRIC_CONTROL_HEIGHT_SMALL: return 22.0f;
        case FOX_METRIC_SPACING_SM: return 6.0f;
        case FOX_METRIC_CORNER_PANEL: return 10.0f;
        default: return 0.0f;
        }
    }
    float fake_label(fox_ctx *, fox_rect r, const char *text, fox_color c, fox_align a)
    {
        ++g.label_calls;
        if (g.label_count < 8)
        {
            g.label_rects[g.label_count++] = r;
        }
        g.label_color = c;
        g.label_align = (float)a;
        return (float)std::strlen(text ? text : "");
    }
    int fake_button(fox_ctx *, fox_rect r, const char *, fox_button_style, int)
    {
        ++g.button_calls;
        if (g.btn_count < 8) g.btn_rects[g.btn_count++] = r;
        return 0;
    }
    void fake_panel(fox_ctx *, fox_rect r, fox_color fill, float, int)
    {
        ++g.panel_calls;
        if (g.panel_count < 4) g.panel_rects[g.panel_count++] = r;
        g.last_color = fill;
    }
    void fake_fill_rect(fox_ctx *, fox_rect r, fox_color c) { ++g.rect_calls; g.last_rect = r; g.last_color = c; }
    void fake_fill_ellipse(fox_ctx *, fox_rect r, fox_color c) { ++g.ellipse_calls; g.last_rect = r; g.last_color = c; }
    void fake_fill_triangle(fox_ctx *, float x0, float y0, float x1, float y1, float x2, float y2, fox_color c)
    {
        ++g.tri_calls;
        g.tri[0] = x0; g.tri[1] = y0; g.tri[2] = x1; g.tri[3] = y1; g.tri[4] = x2; g.tri[5] = y2;
        g.last_color = c;
    }
    int fake_path(fox_ctx *, const float *xy, int32_t count, float thickness, fox_color c, int, int)
    {
        ++g.path_calls;
        g.path_n = count < 4 ? (int)count : 4;
        for (int i = 0; i < g.path_n * 2; ++i) g.path_xy[i] = xy[i];
        g.path_thickness = thickness;
        g.last_color = c;
        return 1;
    }
    const char *fake_gpu_name(fox_ctx *) { return g.gpu.c_str(); }
    uint32_t fake_gpu_vendor(fox_ctx *) { return g.vendor; }
    void fake_set_quality(fox_ctx *, int blur, int shadows, int anim)
    {
        g.quality[0] = blur; g.quality[1] = shadows; g.quality[2] = anim;
    }

    void *fk_window_create(int, int, const char *, void *) { ++g_win_create_calls; return &g_win_token; }
    void fk_window_destroy(void *, void *) { ++g_win_destroy_calls; }

    // 收集 report 的字符串，逐条断言"绝不静默"。
    std::vector<std::string> g_reports;
    void collect_report(const char *line, void *) { g_reports.push_back(line ? line : ""); }

    bool reported_containing(const char *needle)
    {
        for (const std::string &s : g_reports)
            if (s.find(needle) != std::string::npos) return true;
        return false;
    }

    FoxApi fake_api()
    {
        FoxApi a;
        a.create = fake_create;
        a.destroy = fake_destroy;
        a.init_backend_vulkan = fake_init_backend;
        a.backend_ready = fake_backend_ready;
        a.error = fake_error;
        a.should_close = fake_should_close;
        a.poll_events = fake_poll;
        a.begin_frame = fake_begin;
        a.end_frame = fake_end;
        a.width = fake_width;
        a.height = fake_height;
        a.frame_faults = fake_faults;
        a.last_fault = fake_last_fault;
        a.last_frame_dropped = fake_dropped;
        a.mouse_x = fake_mouse_x;
        a.mouse_y = fake_mouse_y;
        a.mouse_down = fake_mouse_down;
        a.theme_color = fake_theme_color;
        a.theme_metric = fake_theme_metric;
        a.label = fake_label;
        a.button = fake_button;
        a.panel = fake_panel;
        a.fill_rect = fake_fill_rect;
        a.fill_ellipse = fake_fill_ellipse;
        a.fill_triangle = fake_fill_triangle;
        a.path = fake_path;
        a.gpu_name = fake_gpu_name;
        a.gpu_vendor = fake_gpu_vendor;
        a.set_quality = fake_set_quality;
        return a;
    }

    FoxHost::WindowFactory fake_window_factory()
    {
        FoxHost::WindowFactory wf;
        wf.create = fk_window_create;
        wf.destroy = fk_window_destroy;
        return wf;
    }

    void reset_fake()
    {
        const std::string gpu = g.gpu;
        g = FakeFx{};
        g.gpu = gpu;
        g_reports.clear();
        g_win_create_calls = 0;
        g_win_destroy_calls = 0;
    }

    FoxHost::Config test_config()
    {
        FoxHost::Config cfg;
        cfg.width = 800;
        cfg.height = 480;
        cfg.title = "fox host test";
        cfg.report = collect_report;
        return cfg;
    }

} // namespace

int main()
{
    // ── 1. 策略状态机（纯逻辑）──────────────────────────────────────────
    section("1. BackendPolicy：纯状态机");
    {
        BackendPolicy p;
        check(!p.use_fox() && !p.fox_tried(), "初始：未尝试、不用 fox");
        check(std::strcmp(p.reason(), "") == 0, "初始 reason 为空串（永不为 null）");

        p.note_init(BackendPolicy::InitResult::Ok, nullptr);
        check(p.use_fox() && p.fox_tried(), "init 成功 -> Fox");

        p.note_frame_faults(1, "a"); // 未达阈值（默认 3）
        check(p.use_fox(), "1 次故障仍在 fox");
        p.note_frame_faults(3, "b"); // 累计到阈值
        check(!p.use_fox(), "累计 3 次故障 -> Nuklear");
        check(std::string(p.reason()).find("3") != std::string::npos &&
                  std::string(p.reason()).find("b") != std::string::npos,
              "切走原因带帧数与具体故障");
        check(p.faults() == 3, "故障计数 = 3");
    }
    {
        BackendPolicy p;
        p.note_init(BackendPolicy::InitResult::CreateFailed, "fox_create returned null");
        check(!p.use_fox() && p.fox_tried(), "create 失败 -> Nuklear，且不再重试");
        check(std::strcmp(p.reason(), "fox_create returned null") == 0, "原因原样保留");
        BackendPolicy q;
        q.note_init(BackendPolicy::InitResult::BackendFailed, nullptr);
        check(std::string(q.reason()).find("fox_init_backend_vulkan") != std::string::npos,
              "backend 失败：有默认措辞（不是空）");
        BackendPolicy r;
        r.note_init(BackendPolicy::InitResult::NotReady, nullptr);
        check(std::string(r.reason()).find("fox_backend_ready") != std::string::npos,
              "not-ready：有默认措辞（不是空）");
    }
    {
        BackendPolicy p(1);
        p.note_init(BackendPolicy::InitResult::Ok, nullptr);
        p.note_frame_faults(1, nullptr);
        check(!p.use_fox(), "阈值 1：一次故障即切");
        // 切走之后不再记账
        p.note_frame_faults(9, nullptr);
        check(p.faults() == 1 && !p.use_fox(), "切走后不再累计（不会\"复活\"）");
    }

    // ── 2. 正常路径 ─────────────────────────────────────────────────────
    section("2. FoxHost 正常路径（假 fox 记录调用）");
    {
        reset_fake();
        FoxApi api = fake_api();
        FoxHost::WindowFactory wf = fake_window_factory();
        FoxHost host(api, wf);

        check(host.init(test_config()), "init -> true");
        check(g.create_calls == 1 && g.init_calls == 1 && g.ready_calls == 1,
              "create / init_backend / backend_ready 各一次");
        check(g_win_create_calls == 1, "窗口创建一次");
        check(host.active(), "host.active()");
        check(std::strcmp(host.gpu_name(), "FakeGPU 3000") == 0 && host.gpu_vendor() == 0xF00D,
              "GPU 型号/厂商可读（A2a 要的）");
        check(reported_containing("fox backend active"), "成功也 report（不静默）");

        check(host.begin_frame(), "begin_frame -> true");
        check(g.poll_calls == 1 && g.begin_calls == 1, "poll_events 在 begin_frame 里各一次");

        // 布局：row(2, 40) → 两列，第三格换行
        host.row(2, 40);
        host.button("A");
        host.button("B");
        host.button("C");
        check(g.button_calls == 3 && g.btn_count == 3, "三个按钮都转发");
        check(g.btn_rects[0].x == 0.0f && g.btn_rects[0].w == 400.0f && g.btn_rects[0].h == 40.0f,
              "格子 1 = 左半幅");
        check(g.btn_rects[1].x == 400.0f, "格子 2 = 右半幅");
        check(g.btn_rects[2].x == 0.0f && g.btn_rects[2].y == 46.0f,
              "第 3 格换行：y = 40 + 间距 6");

        // panel + row 在面板内容区里
        host.panel(10, 20, 300, 200, "Title");
        check(g.panel_calls == 1 && g.panel_rects[0].x == 10.0f && g.panel_rects[0].w == 300.0f,
              "panel 矩形原样");
        check(g.label_calls == 1, "面板标题是一个 label");
        host.row(1, 30);
        host.button("D");
        check(g.btn_rects[3].x == 16.0f && g.btn_rects[3].w == 288.0f && g.btn_rects[3].y == 54.0f,
              "面板内格子：内缩 6px，标题占 22+6");
        host.panel_end();
        host.row(1, 30);
        host.button("E");
        check(g.btn_rects[4].x == 0.0f && g.btn_rects[4].w == 800.0f, "panel_end 恢复窗口域");

        // 文本 + 输入 + 尺寸
        host.text("hi");
        check(g.label_calls == 2 && g.label_align == 0.0f, "text -> label(START)");
        check(host.width() == 800 && host.height() == 480, "逻辑尺寸");
        check(host.mouse_x() == 100.0f && host.mouse_y() == 60.0f, "鼠标位置");
        g.left_down = true;
        check(host.mouse_left_down(), "左键按下");
        g.left_down = false;

        // cgfx 原语转发
        host.fill_rect(1, 2, 30, 40, 0x336699, 128);
        check(g.rect_calls == 1 && g.last_rect.w == 30.0f, "fill_rect 转发矩形");
        check(g.last_color.r == 0x33 && g.last_color.g == 0x66 && g.last_color.b == 0x99 &&
                  g.last_color.a == 128,
              "0xRRGGBB + alpha -> fox_color 拆解正确");

        host.stroke_line(5, 6, 50, 60, 3, 0xFF0000, 255);
        check(g.path_calls == 1 && g.path_n == 2 && g.path_xy[0] == 5.0f && g.path_xy[3] == 60.0f,
              "stroke_line -> 2 点 path");
        check(g.path_thickness == 3.0f, "线宽原样");

        host.fill_ellipse(100, 100, 20, 10, 0x00FF00, 255);
        check(g.ellipse_calls == 1 && g.last_rect.x == 80.0f && g.last_rect.y == 90.0f &&
                  g.last_rect.w == 40.0f && g.last_rect.h == 20.0f,
              "fill_ellipse：圆心+半径 -> 内接矩形");

        host.fill_triangle(0, 0, 10, 0, 5, 8, 0x0000FF, 200);
        check(g.tri_calls == 1 && g.tri[4] == 5.0f && g.tri[5] == 8.0f, "fill_triangle 转发");
        check(g.last_color.b == 255 && g.last_color.a == 200, "三角形颜色/alpha");

        host.set_quality(1, 2, 0);
        check(g.quality[0] == 1 && g.quality[1] == 2 && g.quality[2] == 0, "set_quality 三档转发");

        host.end_frame();
        check(g.end_calls == 1, "end_frame 提交一次");
        host.end_frame();
        check(g.end_calls == 1, "重复 end_frame 是 no-op（没有开帧就不提交）");

        host.shutdown();
        check(g.destroy_calls == 1 && g_win_destroy_calls == 1, "shutdown：销毁上下文 + 窗口");
        check(!host.active(), "shutdown 后 active=false");
    }

    // ── 3. 三条初始化失败门 ─────────────────────────────────────────────
    section("3. 初始化失败：三条门都走 Nuklear，并且说出原因");
    {
        // 3a. fox_create null
        reset_fake();
        g.create_null = true;
        FoxApi api = fake_api();
        FoxHost::WindowFactory wf = fake_window_factory();
        FoxHost host(api, wf);
        check(!host.init(test_config()), "create 返回 null -> init false");
        check(!host.active(), "不 active");
        check(g.destroy_calls == 0, "fox_create 都没成功，没有上下文可销毁");
        check(g_win_destroy_calls == 1, "窗口被销毁（不留半开的窗口）");
        check(reported_containing("fox_create"), "报告点名 fox_create");

        // 失败后：一个 fox 调用都不做
        const int begin0 = g.begin_calls, poll0 = g.poll_calls;
        check(!host.begin_frame(), "begin_frame -> false");
        host.end_frame();
        host.button("x");
        host.fill_rect(0, 0, 1, 1, 0, 255);
        check(g.begin_calls == begin0 && g.poll_calls == poll0 && g.button_calls == 0 &&
                  g.rect_calls == 0,
              "失败路径上零 fox 调用");

        // 二次 init：不重试
        check(!host.init(test_config()), "二次 init 也拒绝");
        check(g.create_calls == 1, "二次 init 不再调用 fox_create（不重试）");
        check(reported_containing("fox_create returned null"), "二次 init 仍给出原因");
    }
    {
        // 3b. init_backend 返回 0 —— 原因逐字来自 fox_error()
        reset_fake();
        g.init_backend_ok = false;
        FoxApi api = fake_api();
        FoxHost::WindowFactory wf = fake_window_factory();
        FoxHost host(api, wf);
        check(!host.init(test_config()), "init_backend=0 -> init false");
        check(std::string(host.reason()) == "create_instance failed: no Vulkan driver",
              "reason 逐字来自 fox_error()");
        check(reported_containing("create_instance failed: no Vulkan driver"),
              "报告里也有这条原因（用户能看懂）");
        check(g.destroy_calls == 1 && g_win_destroy_calls == 1, "上下文与窗口都回收");

        // 3c. backend_ready 为 0（init 声称成功却没就绪）
        reset_fake();
        g.ready = false;
        FoxApi api2 = fake_api();
        FoxHost::WindowFactory wf2 = fake_window_factory();
        FoxHost host2(api2, wf2);
        check(!host2.init(test_config()), "backend_ready=0 -> init false");
        check(reported_containing("fox_backend_ready"), "报告点名 fox_backend_ready");
    }

    // ── 4. 运行中故障：下一帧切走，不回头 ───────────────────────────────
    section("4. 运行中帧故障 -> 下一帧切 Nuklear（不重试）");
    {
        reset_fake();
        FoxApi api = fake_api();
        FoxHost::WindowFactory wf = fake_window_factory();
        FoxHost host(api, wf);
        FoxHost::Config cfg = test_config();
        cfg.fault_threshold = 3;
        check(host.init(cfg), "init 成功");

        // 两帧故障：仍在 fox
        for (int i = 1; i <= 2; ++i)
        {
            check(host.begin_frame(), "第 " + std::to_string(i) + " 帧由 fox 画");
            g.faults = (uint32_t)i;
            host.end_frame();
            check(host.active(), "故障 " + std::to_string(i) + "/3：仍在 fox");
        }
        // 第三个故障达到阈值：本帧已经提交，下一帧开始切
        check(host.begin_frame(), "第 3 帧仍由 fox 画");
        g.faults = 3;
        host.end_frame();
        check(!host.active(), "达到阈值：策略切到 Nuklear");
        check(reported_containing("vkQueuePresentKHR failed"), "报告带具体故障原因");
        check(reported_containing("switching to Nuklear on the next frame"),
              "报告说明下一帧切换（帧边界切换，不中断帧）");

        // 下一帧：begin_frame=false，并且**不再有任何 fox 调用**
        const int begin0 = g.begin_calls, poll0 = g.poll_calls, end0 = g.end_calls;
        check(!host.begin_frame(), "下一帧 begin_frame=false（调用方改走 Nuklear）");
        check(g.begin_calls == begin0 && g.poll_calls == poll0, "切走后不 poll / 不 begin");
        host.end_frame();
        check(g.end_calls == end0, "切走后不提交 fox 帧");

        // 再多次 begin：仍然不回到 fox（不回头）
        host.begin_frame();
        host.begin_frame();
        check(g.begin_calls == begin0, "反复 begin 也不再碰 fox");
    }
    {
        // 阈值可配：1 次故障即切
        reset_fake();
        FoxApi api = fake_api();
        FoxHost::WindowFactory wf = fake_window_factory();
        FoxHost host(api, wf);
        FoxHost::Config cfg = test_config();
        cfg.fault_threshold = 1;
        host.init(cfg);
        host.begin_frame();
        g.faults = 1;
        host.end_frame();
        check(!host.active(), "阈值 1：一次故障即切");
    }

    std::printf(g_fail == 0 ? "\n全部通过\n" : "\n%d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}