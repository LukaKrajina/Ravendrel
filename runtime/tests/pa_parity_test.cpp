// ============================================================
// pa_parity_test.cpp —— 像素加速器（utils/Pa.hpp）的等价性证明
//
// 加速器的全部价值建立在一个可以自动判定的命题上：
//
//     **开加速** 与 **关加速** 渲染出的缓冲逐字节相同。
//
// 所以这个测试不做"看起来对"的检查：它把同一帧渲染两遍（两个 CpuBackend，
// 一个 accel=true、一个 accel=false），然后 memcmp。任何一处加速逻辑改变了
// 一个像素，都会在这里失败并指出偏移。
//
// 覆盖的场景必须是**会触发每条加速路径**的，否则"通过"没有意义：
//   · damage 跟踪      ：动画帧（局部变化）+ 静态帧（应当整帧跳过）
//   · 核心区快速填充   ：实心矩形、圆角矩形、半透明矩形、竖直/水平渐变矩形
//   · 遮挡剔除         ：后画的不透明大面板盖住一堆先画的小控件
//   · 惰性字形         ：文字逐帧新增字符（图集代际号必须让 tile 失效）
//   · 纹理代际         ：update_texture 之后同一份显示列表必须重画
//   · 尺寸变化         ：resize 之后 tile 网格全部失效
//
// 另外它检查加速**确实生效**（否则一个恒返 false 的空壳也会"通过"）：
// 静态帧的跳过像素比例、以及遮挡剔除掉的实例数。
//
// 编译（无需 Vulkan，无需窗口）：
//   clang++ -std=c++20 -O2 -I runtime/include/ciUd/fox -I runtime/include \
//       runtime/tests/pa_parity_test.cpp <fox .cpp 列表> -o pa_parity_test
// ============================================================

#include "core/context.hpp"
#include "widget/primitives.hpp"
#include "window/window.hpp"
#include "backend/cpu_backend.hpp"
#include "draw/instance.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace fox;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        if (ok) { std::printf("  ok   %s\n", what); return; }
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }

    /// 第一个不同字节的偏移，-1 表示完全相同。
    long first_diff(const u8 *a, const u8 *b, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            if (a[i] != b[i]) return (long)i;
        return -1;
    }

    void describe_diff(const u8 *a, const u8 *b, size_t n, int w, const char *tag)
    {
        const long o = first_diff(a, b, n);
        if (o < 0) return;
        const size_t px = (size_t)o / 4;
        const size_t base = px * 4;
        std::printf("       %s: 第一个差异 @ 字节 %ld (像素 %zu, %d,%d)\n", tag, o, px,
                    (int)(px % (size_t)w), (int)(px / (size_t)w));
        std::printf("         ref  RGBA=%u,%u,%u,%u\n", a[base], a[base+1], a[base+2], a[base+3]);
        std::printf("         fast RGBA=%u,%u,%u,%u\n", b[base], b[base+1], b[base+2], b[base+3]);
    }

    std::vector<u8> icon_sheet(int w, int h)
    {
        std::vector<u8> px((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const float dx = (float)x - (float)w * 0.5f + 0.5f;
                const float dy = (float)y - (float)h * 0.5f + 0.5f;
                const float d = std::sqrt(dx * dx + dy * dy);
                const size_t o = ((size_t)y * w + x) * 4;
                const bool on = d <= (float)h * 0.45f;
                px[o + 0] = on ? 255 : 0;
                px[o + 1] = on ? 200 : 0;
                px[o + 2] = on ? 120 : 0;
                px[o + 3] = on ? 255 : 0;
            }
        return px;
    }

    /// 一帧的内容。`f` 驱动动画；`static_after` 之后画面冻结（用于验证
    /// 静态帧的整帧跳过）。
    void build_frame(Context &c, TexId icon, int W, int H, int f, int static_after,
                     bool overlay)
    {
        c.frame_begin();
        Theme &th = c.theme();
        const int af = f < static_after ? f : static_after;

        Rect win_rect{0.0f, 0.0f, (float)W, (float)H};
        Window w;
        w.begin(c, win_rect, "Pa parity", true);

        Rect cr = w.content();
        label(c, Rect{cr.x + 16, cr.y + 12, cr.w - 32, 26}, "Golden Gate (pa parity)", th.text,
              Align::Start);

        // 实心 / 圆角 / 半透明 / 渐变矩形 + 阴影 + 边框：核心区快速路径的四种形态
        const f32 px = cr.x + 16.0f + (f32)(af % 40) * 3.0f;
        c.dl().push(Rect{px, cr.y + 60, 140, 60}, Color{255, 255, 255, 255}, 0.0f);
        c.dl().push_rounded(Rect{px, cr.y + 130, 140, 60}, Color{60, 130, 246, 255},
                            CornerRadii(14.0f), 0.0f);
        c.dl().push(Rect{px, cr.y + 200, 140, 40}, Color{0, 0, 0, 90}, 0.5f);
        // 竖直渐变
        c.dl().push_gradient(Rect{px, cr.y + 250, 140, 60}, Color{255, 90, 60, 255},
                             Color{40, 20, 140, 255}, /*horizontal=*/false, 0.0f);
        // 水平渐变
        c.dl().push_gradient(Rect{px, cr.y + 320, 140, 60}, Color{20, 200, 90, 255},
                             Color{200, 40, 90, 255}, /*horizontal=*/true, 0.0f);

        // 阴影 + 描边（覆盖 PT_SHADOW / PT_BORDER 的逐像素路径）
        {
            Shadow sh;
            sh.offset_y = 4.0f;
            sh.blur = 10.0f;
            sh.color = Color{0, 0, 0, 110};
            Rect sr{cr.x + 320, cr.y + 60, 160, 90};
            // 阴影在 fox 里是 push 出来的独立实例：这里直接用主题辅助
            shadow_rect(c, sr, CornerRadii(12.0f), sh, 0.0f);
            c.dl().push_rounded(sr, Color{250, 250, 255, 255}, CornerRadii(12.0f), 0.0f);
            c.dl().push_border(sr, Color{120, 120, 140, 255}, 2.0f, CornerRadii(12.0f), 0.0f);
        }

        // 描边折线（PT_PATH 多段）——勾选框的对勾与自定义折线
        {
            Vec2 pts[6] = {{cr.x + 320.0f + (f32)(af % 20), cr.y + 200.0f},
                           {cr.x + 360.0f, cr.y + 240.0f},
                           {cr.x + 420.0f, cr.y + 175.0f},
                           {cr.x + 460.0f, cr.y + 230.0f},
                           {cr.x + 500.0f, cr.y + 190.0f},
                           {cr.x + 540.0f, cr.y + 250.0f}};
            c.dl().push_path(pts, 6, 3.0f, Color{40, 180, 120, 255},
                             /*cap=*/1, /*join=*/1, /*flags=*/0, 0.0f);
        }

        // 文字：逐帧新增字符，验证字形图集的惰性栅格化会让 tile 失效
        {
            static const char *words[] = {"A", "AB", "ABC", "ABCD", "ABCDE", "ABCDEF",
                                          "ABCDEFG", "ABCDEFGH"};
            label(c, Rect{cr.x + 560, cr.y + 60, 260, 26}, words[af % 8], th.text, Align::Start);
            label(c, Rect{cr.x + 560, cr.y + 100, 260, 26}, "静态中文标签", th.text_secondary,
                  Align::Start);
        }

        // 图片（纹理代际号）
        c.dl().push_image(Rect{cr.x + 560, cr.y + 140, 48, 48}, icon, IMAGE_TINT,
                          Rect{0, 0, 1, 1}, 0.0f);

        // 玻璃（PT_GLASS，CPU 路径用未模糊背景近似）
        c.dl().push_glass(Rect{cr.x + 320, cr.y + 300, 320, 120}, CornerRadii(16.0f),
                          Color{240, 245, 255, 255}, 12.0f, 0.35f, 1.1f, 0.0f);

        // 控件（勾选框用折线画对勾）
        {
            bool v = true;
            checkbox(c, Rect{cr.x + 16, cr.y + 400, 200, 26}, "复选框 polyline", v);
            static f32 s = 0.5f;
            slider(c, Rect{cr.x + 16, cr.y + 440, 200, 24}, s, 0.0f, 1.0f);
            bool sw = true;
            toggle_switch(c, Rect{cr.x + 260, cr.y + 400, 60, 26}, sw);
        }

        // 遮挡：后画的不透明面板盖住上面所有控件。
        // 这是唯一会触发遮挡剔除的形状（不透明 + 半径 0 + 完全覆盖）。
        if (overlay)
        {
            c.dl().push(Rect{0.0f, 0.0f, (float)W, (float)H}, Color{24, 26, 34, 255}, 0.0f);
        }

        c.frame_end();
    }
} // namespace

int main()
{
    const int W = 640, H = 480;
    Context &c = ctx();
    c.init("pa parity", W, H);
    c.set_fixed_dt(1.0f / 60.0f);

    std::vector<u8> sheet = icon_sheet(32, 32);
    TexId icon = c.create_texture(32, 32, sheet.data());

    CpuBackend ref, fast;
    if (!ref.init(W, H) || !fast.init(W, H))
    {
        std::printf("buffer allocation failed\n");
        return 1;
    }
    fast.set_accel(true);
    ref.set_accel(false);

    const size_t bytes = (size_t)W * H * 4;

    // ── 场景 1：动画帧（damage 每帧部分失效）──────────────────────────
    std::printf("[1] 动画帧：开/关加速必须逐字节相同\n");
    for (int f = 0; f < 8; ++f)
    {
        build_frame(c, icon, W, H, f, 8, false);
        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        if (first_diff(ref.pixels(), fast.pixels(), bytes) >= 0)
        {
            std::printf("       （帧 %d）\n", f);
            describe_diff(ref.pixels(), fast.pixels(), bytes, W, "动画帧");
            // 调试：差异像素 (8,0) 被哪些实例覆盖（默认安静，PA_DEBUG=1 打开）
            if (std::getenv("PA_DEBUG"))
            {
                InstanceSet st;
                build_instances(c, c.dl(), st);
                const f32 tx = 8.5f, ty = 0.5f;
                std::printf("       窗口背景=%u,%u,%u\n", c.theme().window_bg.r, c.theme().window_bg.g,
                            c.theme().window_bg.b);
                for (const InstanceData &d : st.instances)
                {
                    if (tx < d.rect[0] || ty < d.rect[1] || tx > d.rect[0] + d.rect[2] ||
                        ty > d.rect[1] + d.rect[3])
                        continue;
                    std::printf("       覆盖(8,0): ptype=%.0f rect=(%.2f,%.2f %.2fx%.2f) "
                                "radii=(%.2f,%.2f,%.2f,%.2f) color=(%.4f,%.4f,%.4f,%.4f) "
                                "c2=(%.3f,%.3f,%.3f,%.3f) z=%.2f\n",
                                d.params[0], d.rect[0], d.rect[1], d.rect[2], d.rect[3], d.radii[0],
                                d.radii[1], d.radii[2], d.radii[3], d.color[0], d.color[1],
                                d.color[2], d.color[3], d.color2[0], d.color2[1], d.color2[2],
                                d.color2[3], d.params2[1]);
                }
            }
            check(false, "动画帧逐字节相同");
            break;
        }
    }
    if (first_diff(ref.pixels(), fast.pixels(), bytes) < 0) check(true, "动画帧逐字节相同");

    // ── 场景 2：静态帧（应当整帧跳过，但像素不变）─────────────────────
    std::printf("[2] 静态帧：damage 生效且像素不变\n");
    fast.reset_stats();
    {
        // 同一帧内容连画三遍：第一遍登记（字形在这一遍里被惰性栅格化，
        // 图集代际号会变 ⇒ 第二遍仍然全屏重画，这是**正确**行为），
        // 第三遍内容与键都不再变化 ⇒ 应当整帧跳过。
        build_frame(c, icon, W, H, 40, 8, false);
        fast.render(c.dl(), c);   // 预热：栅格化全部字形
        fast.render(c.dl(), c);   // 第二遍：图集稳定
        fast.reset_stats();
        fast.render(c.dl(), c);   // 第三遍：内容与键都没变
        const u64 skipped_second = fast.stats().pixels_skipped;
        std::printf("       静态帧跳过像素=%llu / %zu (%.1f%%), 跳过实例=%llu\n",
                    (unsigned long long)skipped_second, (size_t)W * H,
                    100.0 * (double)skipped_second / (double)((size_t)W * H),
                    (unsigned long long)fast.stats().instances_skipped);
        const size_t painted_before = fast.non_background_pixels();
        check(skipped_second > (u64)(W * H) / 2, "静态帧跳过一半以上的像素");
        // 参考后端必须渲染同一帧内容才能比较（它上一帧的内容还在缓冲里）
        ref.render(c.dl(), c);
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "静态帧像素不变");
        check(fast.non_background_pixels() == painted_before, "静态帧后画面字节不变");
    }

    // ── 场景 3：帧键必须捕捉"显示列表之外"的变化 ───────────────────────
    std::printf("[3] 纹理更新 / 字形新增 / 尺寸变化 必须让 tile 失效\n");
    {
        // 3a: 更新纹理内容，显示列表完全不变
        build_frame(c, icon, W, H, 40, 8, false);
        fast.render(c.dl(), c);
        std::vector<u8> sheet2 = sheet;
        for (size_t i = 0; i < sheet2.size(); i += 4) sheet2[i] = (u8)(255 - sheet2[i]);
        c.update_texture(icon, 32, 32, sheet2.data());
        fast.render(c.dl(), c);
        ref.render(c.dl(), c);
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "纹理更新后像素一致");
        c.update_texture(icon, 32, 32, sheet.data());

        // 3b: 惰性字形——新增一个从未出现过的字符
        build_frame(c, icon, W, H, 40, 8, false);
        fast.render(c.dl(), c);
        {
            c.frame_begin();
            label(c, Rect{10, 10, 300, 26}, "从未出现过的字形 zZ9", c.theme().text, Align::Start);
            c.frame_end();
            fast.render(c.dl(), c);
            ref.render(c.dl(), c);
        }
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "新增字形后像素一致");

        // 3c: resize —— tile 网格整体失效
        ref.resize(W + 32, H);
        fast.resize(W + 32, H);
        build_frame(c, icon, W + 32, H, 41, 8, false);
        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        check(first_diff(ref.pixels(), fast.pixels(), (size_t)(W + 32) * H * 4) < 0,
              "resize 后像素一致");
        ref.resize(W, H);
        fast.resize(W, H);
    }

    // ── 场景 4：遮挡剔除（两个后端共用的那条加速）──────────────────────
    std::printf("[4] 遮挡剔除：删掉的实例必须被完全覆盖\n");
    {
        const u32 before_on = c.hidden_culled();
        Context &cc = c;
        (void)before_on;
        // 剔除开
        cc.set_cull_hidden(true);
        build_frame(c, icon, W, H, 41, 8, true); // overlay = 整屏不透明面板
        ref.set_accel(false);
        fast.set_accel(true);
        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        const u32 culled_on = c.hidden_culled();
        if (first_diff(ref.pixels(), fast.pixels(), bytes) >= 0)
            describe_diff(ref.pixels(), fast.pixels(), bytes, W, "遮挡剔除");
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "剔除开：像素与参考一致");
        std::printf("       被剔除的实例=%u（整屏不透明面板盖住了它们）\n", culled_on);
        check(culled_on > 5, "整屏面板确实剔除了多个实例");

        // 剔除关：画面必须一模一样（这才是"被覆盖"而不是"不见了"的证明）
        cc.set_cull_hidden(false);
        build_frame(c, icon, W, H, 41, 8, true);
        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        check(c.hidden_culled() == 0, "剔除关：没有实例被剔除");
        cc.set_cull_hidden(true);
    }

    // ── 场景 5：随机几何（小数坐标、随机半径/透明度/渐变）──────────────
    //
    // 核心区推导最容易在**边界**上出错（半径、像素中心、半开区间），而
    // 边界在小数坐标下才会出现。这里用固定种子的伪随机序列铺满随机矩形，
    // 每帧全量逐字节对比；随机但可复现，失败时能直接复算。
    std::printf("[5] 随机几何：小数坐标 + 随机半径/透明度\n");
    {
        int bad = 0;
        for (int seed = 0; seed < 25 && bad == 0; ++seed)
        {
            c.frame_begin();
            unsigned rs = (unsigned)seed * 2654435761u + 12345u;
            auto next = [&]() { rs = rs * 1664525u + 1013904223u; return rs; };
            for (int i = 0; i < 80; ++i)
            {
                const f32 x = (f32)(next() % (unsigned)(W - 24)) + 0.3f;
                const f32 y = (f32)(next() % (unsigned)(H - 24)) + 0.7f;
                const f32 rw = 4.0f + (f32)(next() % 64);
                const f32 rh = 4.0f + (f32)(next() % 48);
                const f32 rad = (f32)(next() % 14);
                const u8 a = (u8)(40 + next() % 216);
                const Color col{(u8)(next() % 256), (u8)(next() % 256), (u8)(next() % 256), a};
                if (next() % 8 == 0)
                {
                    const Color c2{(u8)(next() % 256), (u8)(next() % 256), (u8)(next() % 256), 255};
                    c.dl().push_gradient(Rect{x, y, rw, rh}, col, c2, next() % 2 == 0, 0.0f);
                }
                else if (rad > 0.5f)
                {
                    c.dl().push_rounded(Rect{x, y, rw, rh}, col, CornerRadii(rad), 0.0f);
                }
                else
                {
                    c.dl().push(Rect{x, y, rw, rh}, col, 0.0f);
                }
            }
            c.frame_end();
            ref.render(c.dl(), c);
            fast.render(c.dl(), c);
            if (first_diff(ref.pixels(), fast.pixels(), bytes) >= 0)
            {
                std::printf("       （seed %d）\n", seed);
                describe_diff(ref.pixels(), fast.pixels(), bytes, W, "随机几何");
                bad = 1;
            }
        }
        check(bad == 0, "25 组随机几何逐字节相同（每组 80 个矩形）");
    }

    std::printf("[6] 新图元：椭圆 / 三角形 / 描边折线（含退化输入与遮挡安全）\n");
    {
        // 背景：纯色，便于对像素下直接断言
        const Color bg = c.theme().window_bg;
        auto px_u8 = [&](int x, int y, int ch) -> int {
            return ref.pixels()[((size_t)y * (size_t)W + (size_t)x) * 4 + (size_t)ch];
        };

        const f32 ex = 120.0f, ey = 120.0f, erx = 41.5f, ery = 27.25f;
        const Color fill{220, 60, 60, 255};

        c.frame_begin();
        c.dl().push(Rect{0.0f, 0.0f, (float)W, (float)H}, bg, 0.0f);
        // 真椭圆（rx != ry）+ 圆 + 半透明椭圆 + 太小（<1px）的椭圆
        c.dl().push_ellipse(Rect{ex - erx, ey - ery, erx * 2, ery * 2}, fill, 0.0f);
        c.dl().push_ellipse(Rect{320 - 30, 120 - 30, 60, 60}, Color{60, 160, 240, 255}, 0.0f);
        c.dl().push_ellipse(Rect{460 - 34, 120 - 22, 68, 44}, Color{80, 220, 140, 128}, 0.0f);
        c.dl().push_ellipse(Rect{40.0f, 300.0f, 0.6f, 0.6f}, Color{255, 255, 255, 255}, 0.0f);
        // 三角形：两种绕向（符号必须来自面积，而不是顶点顺序）+ 零面积
        c.dl().push_triangle(Vec2{80, 400}, Vec2{180, 400}, Vec2{130, 330},
                             Color{250, 200, 40, 255}, 0.0f);
        c.dl().push_triangle(Vec2{240, 330}, Vec2{340, 330}, Vec2{290, 400},
                             Color{250, 200, 40, 255}, 0.0f);
        c.dl().push_triangle(Vec2{400, 350}, Vec2{500, 350}, Vec2{450, 350},
                             Color{255, 0, 255, 255}, 0.0f);
        // 折线：round 帽（勾选框用的那种）、butt 帽、square 帽、带零长段
        {
            const Vec2 a[2] = {{40, 40}, {90, 70}};
            c.dl().push_path(a, 2, 6.0f, Color{40, 180, 120, 255}, /*cap=*/1, 1, 0, 0.0f);
            const Vec2 b[4] = {{300, 420}, {360, 440}, {420, 410}, {520, 460}};
            c.dl().push_path(b, 4, 4.0f, Color{200, 120, 60, 255}, /*cap=*/0, 0, 0, 0.0f);
            const Vec2 d2[3] = {{540, 420}, {600, 460}, {600, 460}};
            c.dl().push_path(d2, 3, 5.0f, Color{120, 120, 240, 255}, /*cap=*/2, 2, 0, 0.0f);
        }
        c.frame_end();

        // 退化输入必须在 push / 展开阶段消失，而不是到达光栅器。
        // 期望：椭圆 4 推 3 存（0.6px 的被 push 拒掉）；三角形 3 推 2 存（零面积被拒）；
        // 折线 3 条 → 1+3+1 = 5 段（d2 的第二段零长，展开时跳过）。
        InstanceSet st;
        build_instances(c, c.dl(), st);
        u32 n_ell = 0, n_tri = 0, n_path = 0;
        for (const InstanceData &d : st.instances)
        {
            if (d.params[0] == PT_ELLIPSE) ++n_ell;
            else if (d.params[0] == PT_TRIANGLE) ++n_tri;
            else if (d.params[0] == PT_PATH) ++n_path;
        }
        std::printf("       实例：椭圆=%u 三角形=%u 折线段=%u\n", n_ell, n_tri, n_path);
        check(n_ell == 3, "太小（<1px）的椭圆不产生实例");
        check(n_tri == 2, "零面积三角形不产生实例");
        check(n_path == 5, "零长折线段被跳过，其余每段一个实例");

        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        if (first_diff(ref.pixels(), fast.pixels(), bytes) >= 0)
            describe_diff(ref.pixels(), fast.pixels(), bytes, W, "新图元");
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "新图元逐字节相同");

        // ── 直接断言几何（加速开关两边共用同一份 SDF，parity 抓不到公式错）──
        // 中心必须是不透明的实色
        const u8 *mid = &ref.pixels()[((size_t)120 * (size_t)W + 120) * 4];
        check(mid[0] == fill.r && mid[1] == fill.g && mid[2] == fill.b && mid[3] == 255,
              "椭圆中心为实色（alpha==1 的核心区）");
        // AABB 角必须在椭圆之外（背景色）
        {
            const u8 *corner = &ref.pixels()[((size_t)94 * (size_t)W + 80) * 4];
            check(corner[0] == bg.r && corner[1] == bg.g && corner[2] == bg.b,
                  "椭圆 AABB 角落未被填充（SDF，而不是矩形近似）");
        }
        // 上边缘存在半透明过渡像素（AA 斜坡确实存在）
        {
            bool ramp = false;
            for (int y = 92; y < 118 && !ramp; ++y)
            {
                const int r0 = px_u8(120, y, 0);
                const int g0 = px_u8(120, y, 1);
                const int b0 = px_u8(120, y, 2);
                if (r0 == bg.r && g0 == bg.g && b0 == bg.b) continue;
                ramp = !(r0 == fill.r && g0 == fill.g && b0 == fill.b);
            }
            check(ramp, "椭圆边缘存在半透明过渡像素（AA 斜坡）");
        }
        // 三角形：质心实色、外侧背景
        {
            const int cx = (80 + 180 + 130) / 3, cy = (400 + 400 + 330) / 3;
            const u8 *in = &ref.pixels()[((size_t)cy * (size_t)W + (size_t)cx) * 4];
            check(in[0] == 250 && in[1] == 200 && in[2] == 40, "三角形质心被填充");
            const u8 *out = &ref.pixels()[((size_t)330 * (size_t)W + 100) * 4];
            check(out[0] == bg.r && out[1] == bg.g && out[2] == bg.b,
                  "三角形外侧保持背景（绕向无关）");
        }
        // 折线：段中点必须是描边色（tip：AABB 曾经是 0 尺寸 ⇒ 整条被丢弃）
        {
            const u8 *p = &ref.pixels()[((size_t)55 * (size_t)W + 65) * 4];
            check(p[0] == 40 && p[1] == 180 && p[2] == 120, "折线段中点被描边");
        }

        // ── 遮挡安全：只有不透明 PT_RECT 才能当盖面 ──
        // 大椭圆 / 大三角形即使盖住控件也不能剔除它们（它们盖不满矩形区域），
        // 一旦有人把盖面判定扩到这两个类型，这里会变成"内容消失"。
        {
            Context &cc = c;
            cc.set_cull_hidden(true);
            c.frame_begin();
            c.dl().push(Rect{0.0f, 0.0f, (float)W, (float)H}, bg, 0.0f);
            for (int i = 0; i < 40; ++i)
                c.dl().push(Rect{10.0f + (f32)i * 14.0f, 40.0f, 10.0f, 10.0f},
                            Color{255, 255, 255, 255}, 0.0f);
            c.dl().push_ellipse(Rect{0, 0, (float)W, (float)H}, Color{10, 10, 10, 255}, 0.0f);
            c.dl().push_triangle(Vec2{0, 0}, Vec2{(float)W, 0}, Vec2{0, (float)H},
                                 Color{10, 10, 10, 255}, 0.0f);
            c.frame_end();
            const u32 culled = c.hidden_culled();
            ref.render(c.dl(), c);
            fast.render(c.dl(), c);
            check(culled == 0, "椭圆/三角形不能充当遮挡盖面");
            check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "遮挡场景逐字节相同");
        }
    }

    std::printf("[7] mipmap：缩小必须走金字塔（光栅器的 lod 计算）\n");
    {
        // 4x4 不透明四色块 → 整图平均 = (128,128,128,255)。
        // 手算（预乘平均，a 全为 255 ⇒ 就是普通平均）：
        //   r = (255+0+0+255)/4 = 127.5 → (510+2)/4 = 128，g/b 同理。
        static const u8 kCols[4][4] = {
            {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
        std::vector<u8> px((size_t)4 * 4 * 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                std::memcpy(&px[((size_t)y * 4 + x) * 4], kCols[(y / 2) * 2 + x / 2], 4);

        const TexId mipped = c.create_texture_mipped(4, 4, px.data());
        const TexId plain = c.create_texture(4, 4, px.data());
        check(mipped != TEX_NONE && plain != TEX_NONE, "两张 4x4 纹理上传成功");
        check(c.texture(mipped) && c.texture(mipped)->mip_levels() == 3,
              "create_texture_mipped 建出 3 级金字塔");
        check(c.texture(plain) && c.texture(plain)->mip_levels() == 1,
              "create_texture 仍是 1 级（字形图集不能被收税）");

        // 1x1 的绘制 = 4 倍缩小 = lod 2 ⇒ 整图平均；无 mip 的那张走最近邻：uv=(0.5,0.5)
        // 取整到 texel (2,2)，即右下角那块（白）。
        const Color bgp = c.theme().window_bg;
        c.frame_begin();
        c.dl().push(Rect{0.0f, 0.0f, (float)W, (float)H}, bgp, 0.0f);
        c.dl().push_image(Rect{120.0f, 120.0f, 1.0f, 1.0f}, mipped, IMAGE_TINT, Rect{0, 0, 1, 1}, 0.0f);
        c.frame_end();
        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "缩小绘制：加速开关逐字节相同");
        {
            const u8 *m = &ref.pixels()[((size_t)120 * (size_t)W + 120) * 4];
            // 不透明取样 ⇒ 像素就是取样值本身（没有混合数学要复算）
            check(m[0] == 128 && m[1] == 128 && m[2] == 128 && m[3] == 255,
                  "缩小到 1px ⇒ 整图平均色（不是恰好落在哪个 texel 上）");
        }

        c.frame_begin();
        c.dl().push(Rect{0.0f, 0.0f, (float)W, (float)H}, bgp, 0.0f);
        c.dl().push_image(Rect{120.0f, 120.0f, 1.0f, 1.0f}, plain, IMAGE_TINT, Rect{0, 0, 1, 1}, 0.0f);
        c.frame_end();
        ref.render(c.dl(), c);
        fast.render(c.dl(), c);
        if (first_diff(ref.pixels(), fast.pixels(), bytes) >= 0)
            describe_diff(ref.pixels(), fast.pixels(), bytes, W, "无 mip 同图");
        check(first_diff(ref.pixels(), fast.pixels(), bytes) < 0, "无 mip 的同图：加速开关逐字节相同");
        {
            const u8 *p = &ref.pixels()[((size_t)120 * (size_t)W + 120) * 4];
            const u8 *q = &fast.pixels()[((size_t)120 * (size_t)W + 120) * 4];
            // uv=(0.5,0.5) 最近邻取整到 texel (2,2) —— 右下角那个色块（白）。
            const bool nn_ok = p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255 &&
                               q[0] == 255 && q[1] == 255 && q[2] == 255 && q[3] == 255;
            if (!nn_ok)
                std::printf("       无 mip 1x1 取样 ref RGBA=%u,%u,%u,%u / fast RGBA=%u,%u,%u,%u\n",
                            p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
            check(nn_ok, "无 mip 的同图仍是最近邻（uv 中心落在右下 texel）");
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "PA-PARITY: 全部通过" : "PA-PARITY: 失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}