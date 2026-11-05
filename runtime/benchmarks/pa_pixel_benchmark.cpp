// ============================================================================
// Pa.hpp（像素级加速器）基准
//
// 本基准只回答一个问题：**加速器到底省了多少**，以及省下来的部分在这个
// 工作负载里占多大比例。它不测"最快能到多少"，因为它跑的是 fox 的 CPU
// 后端 —— 一条用来判定正确性的参考路径，不是用来跑得快的路径。
//
// 场景（每个场景都先与关掉加速的结果对比过，见 runtime/tests/pa_parity_test.cpp）：
//
//   [1] 静态帧        内容完全不变：damage 应当跳过几乎全部像素
//   [2] 局部动画      只有一个小控件在动：只该重画它经过的 tile
//   [3] 全屏动画      所有内容每帧都在变：加速应当基本无收益（且不该变慢太多）
//   [4] 遮挡剔除      后画的不透明整屏面板盖住下面的控件：实例数直接减少
//
// 三个数字必须一起读：**跳过像素比例**（damage 的有效性）、**每帧毫秒**
// （净收益）、**实例数**（遮挡剔除的有效性）。只看毫秒会被"哈希本身也
// 要钱"掩盖：全屏动画场景里 damage 的净收益应当是负的或接近 0，那是
// 诚实的结论。
//
// 运行：
//   clang++ -std=c++20 -O2 -I runtime/include -I runtime/include/ciUd/fox \
//       runtime/benchmarks/pa_pixel_benchmark.cpp <fox 源文件列表> -o pa_pixel_benchmark
//   ./pa_pixel_benchmark
//
// 见 docs/gfx-acceleration.md §2。
// ============================================================================

#include "core/context.hpp"
#include "widget/primitives.hpp"
#include "window/window.hpp"
#include "backend/cpu_backend.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace fox;

namespace
{
    using Clock = std::chrono::steady_clock;

    const int W = 1000, H = 680;
    const int FRAMES = 120;

    /// 一帧的内容。`anim` 决定有多少东西在动：
    ///   0 = 全静止，1 = 局部动，2 = 全屏动。
    void build_frame(Context &c, TexId icon, int frame, int anim)
    {
        c.frame_begin();
        Theme &th = c.theme();
        const int f = anim == 0 ? 0 : frame;
        // 场景 [3] 是"整屏都在动"：把所有内容整体平移，几乎所有 tile 都会脏。
        // 这与场景 [2]（只有一个控件在动）形成对照，而不是靠一个抖动的小方块
        // 伪装成全屏动画 —— 那样测出来的"跳过 95%"是自欺。
        const f32 ox = anim == 2 ? (f32)(f % 20) * 4.0f : 0.0f;
        const f32 oy = anim == 2 ? (f32)(f % 12) * 3.0f : 0.0f;

        Window w;
        w.begin(c, Rect{0, 0, (f32)W, (f32)H}, "Pa 加速基准", true);
        Rect cr = w.content();

        label(c, Rect{cr.x + 16, cr.y + 12, cr.w - 32, 26}, "像素加速器基准", th.text, Align::Start);

        // 面板：大块实心/圆角矩形 —— 核心区快速填充的主要受益者
        for (int i = 0; i < 6; ++i)
        {
            const f32 x = cr.x + 16.0f + (f32)i * 158.0f + ox;
            const f32 y = cr.y + 56.0f + oy;
            c.dl().push_rounded(Rect{x, y, 146, 110}, Color{52, 58, 72, 255},
                                CornerRadii(10.0f), 0.0f);
            c.dl().push_gradient(Rect{x + 8, y + 8, 130, 60}, Color{90, 150, 240, 255},
                                 Color{40, 60, 160, 255}, i % 2 == 0, 0.0f);
            label(c, Rect{x + 8, y + 74, 130, 26}, "Panel", th.text_secondary, Align::Start);
            c.dl().push(Rect{x, y + 122, 130, 40}, Color{255, 200, 60, 200}, 0.0f);
            c.dl().push_border(Rect{x, y, 146, 110}, Color{120, 130, 150, 255}, 2.0f,
                               CornerRadii(10.0f), 0.0f);
        }

        // 文字块：字形图集 + 惰性栅格化
        for (int line = 0; line < 8; ++line)
        {
            label(c, Rect{cr.x + 16 + ox, cr.y + 240 + (f32)line * 26 + oy, cr.w - 32, 24},
                  "The quick brown fox jumps over the lazy dog 0123456789", th.text,
                  Align::Start);
        }

        // 控件：勾选框用描边折线画对勾（PT_PATH）
        for (int i = 0; i < 5; ++i)
        {
            bool v = (i % 2) == 0;
            checkbox(c, Rect{cr.x + 16 + (f32)i * 150 + ox, cr.y + 460 + oy, 140, 26},
                     "polyline check", v);
            static f32 sv[5] = {0.2f, 0.4f, 0.5f, 0.6f, 0.8f};
            slider(c, Rect{cr.x + 16 + (f32)i * 150 + ox, cr.y + 496 + oy, 140, 24}, sv[i], 0.0f,
                   1.0f);
        }

        // 局部动画：一个跟随帧号移动的小方块（场景 [2]）
        const f32 mx = (anim == 1 ? cr.x + 16.0f + (f32)(f % 300) * 2.0f : cr.x + 16.0f) + ox;
        c.dl().push_rounded(Rect{mx, cr.y + 540, 90, 60}, Color{240, 90, 90, 255},
                            CornerRadii(12.0f), 0.0f);

        // 图片 + 玻璃
        c.dl().push_image(Rect{cr.x + 700 + ox, cr.y + 540 + oy, 64, 64}, icon, IMAGE_TINT,
                          Rect{0, 0, 1, 1}, 0.0f);
        c.dl().push_glass(Rect{cr.x + 200 + ox, cr.y + 530 + oy, 400, 84}, CornerRadii(16.0f),
                          Color{240, 245, 255, 255}, 12.0f, 0.35f, 1.1f, 0.0f);

        c.frame_end();
    }

    std::vector<u8> make_icon(int w, int h)
    {
        std::vector<u8> px((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const f32 dx = (f32)x - (f32)w * 0.5f + 0.5f;
                const f32 dy = (f32)y - (f32)h * 0.5f + 0.5f;
                const bool on = (dx * dx + dy * dy) <= (f32)h * 0.45f * (f32)h * 0.45f;
                const size_t o = ((size_t)y * w + x) * 4;
                px[o] = on ? 255 : 0;
                px[o + 1] = on ? 200 : 0;
                px[o + 2] = on ? 120 : 0;
                px[o + 3] = on ? 255 : 0;
            }
        return px;
    }

    struct Result
    {
        double ms_per_frame = 0.0;
        double skipped_pct = 0.0;
        double core_pct = 0.0;
        u64 instances_drawn = 0;
        u64 instances_skipped = 0;
        u64 hidden_culled = 0;
    };

    /// 跑一个场景。`accel` 决定加速器开关；两者渲出的像素必须逐字节相同
    /// （由 pa_parity_test 保证，这里只测时间）。
    Result run(Context &c, CpuBackend &cpu, TexId icon, int anim, bool accel, int frames)
    {
        cpu.set_accel(accel);
        cpu.reset_stats();
        // ⚠ 必须逐帧累加：stats 里的 pixels_skipped 是**每帧被赋值**的（不是
        //   累加器），循环结束后只剩最后一帧的值。只读一次会把"最后一帧
        //   恰好全屏重画"当成"整个场景都没跳过"。
        double skipped_sum = 0.0;
        const auto t0 = Clock::now();
        for (int i = 0; i < frames; ++i)
        {
            build_frame(c, icon, i, anim);
            cpu.render(c.dl(), c);
            skipped_sum += (double)cpu.stats().pixels_skipped;
        }
        const auto t1 = Clock::now();

        Result r;
        r.ms_per_frame = std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
        const pa::Stats &st = cpu.stats();
        const double total = (double)W * (double)H * (double)frames;
        r.skipped_pct = 100.0 * skipped_sum / total;
        const u64 shaded = st.shaded_pixels + st.core_pixels;
        r.core_pct = shaded > 0 ? 100.0 * (double)st.core_pixels / (double)shaded : 0.0;
        r.instances_drawn = st.instances_drawn;
        r.instances_skipped = st.instances_skipped;
        r.hidden_culled = st.hidden_instances;
        return r;
    }
} // namespace

int main()
{
    Context &c = ctx();
    c.init("pa benchmark", W, H);
    c.set_fixed_dt(1.0f / 60.0f);

    std::vector<u8> icon_px = make_icon(32, 32);
    TexId icon = c.create_texture(32, 32, icon_px.data());

    CpuBackend cpu;
    if (!cpu.init(W, H))
    {
        std::printf("缓冲区分配失败\n");
        return 1;
    }

    std::printf("Pa.hpp 像素加速基准 · %dx%d · %d 帧/场景 · CPU 后端（参考路径）\n\n", W, H,
                FRAMES);

    const char *names[] = {"[1] 静态帧（内容不变）", "[2] 局部动画（一个控件在动）",
                           "[3] 全屏动画（每帧都变）"};

    std::printf("%-28s %10s %10s %8s %9s %9s %10s\n", "场景", "参考(ms)", "加速(ms)", "加速比",
                "跳过像素", "核心区", "剔除实例");
    for (int anim = 0; anim <= 2; ++anim)
    {
        const Result ref = run(c, cpu, icon, anim, false, FRAMES);
        const Result fast = run(c, cpu, icon, anim, true, FRAMES);
        std::printf("%-28s %10.3f %10.3f %7.2fx %8.1f%% %8.1f%% %10llu\n", names[anim],
                    ref.ms_per_frame, fast.ms_per_frame,
                    fast.ms_per_frame > 0.0 ? ref.ms_per_frame / fast.ms_per_frame : 0.0,
                    fast.skipped_pct, fast.core_pct, (unsigned long long)fast.hidden_culled);
    }

    // [4] 遮挡剔除：整屏不透明面板画在所有控件之后 —— 它的核心区完整盖住
    // 下面的实例，于是那些实例可以直接从显示列表里删掉（两个后端共用）。
    {
        auto overlay_frame = [&](bool overlay)
        {
            c.frame_begin();
            Window w;
            w.begin(c, Rect{0, 0, (f32)W, (f32)H}, "遮挡剔除", true);
            Rect cr = w.content();
            for (int i = 0; i < 400; ++i)
            {
                const f32 x = cr.x + 8.0f + (f32)(i % 20) * 48.0f;
                const f32 y = cr.y + 8.0f + (f32)(i / 20) * 32.0f;
                c.dl().push_rounded(Rect{x, y, 44, 28}, Color{60, 70, 90, 255}, CornerRadii(6.0f),
                                    0.0f);
                label(c, Rect{x + 2, y + 4, 40, 20}, "item", c.theme().text_secondary,
                      Align::Start);
            }
            if (overlay)
            {
                c.dl().push(Rect{0, 0, (f32)W, (f32)H}, Color{24, 26, 34, 255}, 0.0f);
            }
            c.frame_end();
        };

        c.set_cull_hidden(false);
        overlay_frame(true);
        cpu.set_accel(false);
        cpu.render(c.dl(), c);
        const u32 total_instances = c.dl().cmd_count();
        const u32 drawn_no_cull = cpu.last_instance_count();

        c.set_cull_hidden(true);
        overlay_frame(true);
        cpu.render(c.dl(), c);
        const u32 drawn_cull = cpu.last_instance_count();
        const u32 culled = c.hidden_culled();

        std::printf("\n[4] 遮挡剔除（400 个控件 + 整屏不透明面板）\n");
        std::printf("    命令数 %u · 不剔除时光栅化实例 %u · 剔除后 %u（删掉 %u = %.1f%%）\n",
                    total_instances, drawn_no_cull, drawn_cull, culled,
                    100.0 * (double)culled / (double)(drawn_no_cull ? drawn_no_cull : 1));
        c.set_cull_hidden(true);
        std::printf("    剔除前后像素逐字节相同（由 pa_parity_test 第 [4] 节断言）\n");
    }

    // ── 分段时间：回答"为什么某个场景没有加速" ──────────────────────────
    std::printf("\n分段时间（每帧毫秒，只统计加速器自己的三段）：\n");
    std::printf("%-28s %10s %10s %10s %10s\n", "场景", "登记哈希", "清屏", "光栅化", "三段合计");
    for (int anim = 0; anim <= 2; ++anim)
    {
        const Result r = run(c, cpu, icon, anim, true, 40);
        const pa::Stats &st = cpu.stats();
        // run() 内每帧都在累加 ns_*，所以要按帧数平均；而 run 内部又调用了
        // reset_stats() 一次，这里直接读被累加的值即可。
        const double n = 40.0;
        const double a = (double)st.ns_track / 1e6 / n;
        const double b = (double)st.ns_clear / 1e6 / n;
        const double d = (double)st.ns_draw / 1e6 / n;
        std::printf("%-28s %10.3f %10.3f %10.3f %10.3f\n", names[anim], a, b, d, a + b + d);
        (void)r;
    }

    std::printf("\n注：加速比只在\"内容有部分不变\"时才有意义。全屏动画场景里 damage 的\n"
                "    净收益接近 0 —— 此时省下的像素为零，哈希开销仍然存在。这是设计\n"
                "    选择：加速器宁可白算哈希，也不允许漏画一个像素。\n");
    return 0;
}
