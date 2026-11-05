// Fox draw/instance.cpp - 显示列表 -> 实例
//
// 为什么这是共享层而不是 Vulkan 后端的一部分，见 instance.hpp。

#include "instance.hpp"
#include "text/font.hpp"
#include "utils/Pa.hpp"   // 遮挡剔除（项目提供的像素加速器）

namespace fox
{
    static inline void put_color(f32 *dst, const Color &c)
    {
        dst[0] = c.r / 255.0f;
        dst[1] = c.g / 255.0f;
        dst[2] = c.b / 255.0f;
        dst[3] = c.a / 255.0f;
    }

    void build_instances(Context &ctx, const DisplayList &dl, InstanceSet &out)
    {
        out.clear();
        // 每帧重置统计：剔除关掉时必须是 0，而不是上一帧的残留值
        // （测试就是靠这个数判定"剔除确实发生了"）。
        ctx.note_hidden_culled(0);
        if (out.instances.capacity() < 1024) out.instances.reserve(1024);
        if (out.batches.capacity() < 32) out.batches.reserve(32);

        const Cmd *cmds = dl.cmds();
        const GlyphQuad *glyphs = dl.glyphs();
        const u32 n = dl.cmd_count();

        // 热压力降级（见 core/context.hpp 的 Quality）：阴影是**最贵且最可
        // 牺牲**的一层 —— 它不承载信息，只是视觉层。半档 = 半径减半，
        // 关档 = 连实例都不生成（不是"生成后画成透明"：那样白付一次带宽）。
        const Context::Quality &quality = ctx.quality();

        // 硬上限。两个后端都必须能装下一帧而不必在渲染中途分配，
        // 所以集合封顶而不是动态增长。
        const size_t kMaxInstances = 16384;

        // ── 裁剪栈 ──────────────────────────────────────────────────────
        //
        // 之前 ClipPush/ClipPop 在这里被直接 `continue` 掉 —— 也就是说
        // fox_push_clip/fox_clip_pop 忠实地记了命令，却在渲染管线里**整个
        // 丢弃**：fox.h 承诺的「画到 pop 之前的一切都被限制在那个矩形里」
        // 从来不成立，连带 group_begin 的裁剪也失效（滚动组的内容会溢出、
        // 盖住边框和相邻控件）。
        //
        // 现在按 batch 生效：裁剪变化处切批，于是「一个 batch 一个 scissor」
        // 就是精确的（Vulkan 后端按 batch 设 scissor，CPU 后端按 batch 求交）。
        //
        // 不做**几何**裁剪（把实例矩形本身裁短）：那会把字形切成半个、也会
        // 破坏 path 的旋转坐标系。裁剪是像素级的事，交给光栅化。
        std::vector<Rect> clips;
        Rect cur_clip{};
        bool has_clip = false;
        auto recompute_clip = [&]() {
            if (clips.empty())
            {
                has_clip = false;
                cur_clip = Rect{};
                return;
            }
            cur_clip = clips.back();
            for (size_t i = 0; i + 1 < clips.size(); ++i)
                cur_clip = cur_clip.intersected(clips[i]);
            has_clip = true;
        };

        Batch run;
        auto flush_run = [&]() {
            if (run.count)
            {
                out.batches.push_back(run);
                run = Batch{};
            }
        };
        auto open_run = [&](FontId f, TexId t) {
            if (run.count == 0) run.first = (u32)out.instances.size();
            run.font = f;
            run.tex = t;
            run.clip = cur_clip;
        };

        for (u32 i = 0; i < n; ++i)
        {
            const Cmd &cmd = cmds[i];
            if (cmd.type == CmdType::ClipPush)
            {
                // 先切批：batch 不得跨裁剪边界，否则一个 scissor 盖不住两段。
                flush_run();
                clips.push_back(cmd.clip);
                recompute_clip();
                continue;
            }
            if (cmd.type == CmdType::ClipPop)
            {
                flush_run();
                if (!clips.empty()) clips.pop_back();
                recompute_clip();
                continue;
            }

            // ── Text：每字形一个实例 ──────────────────────────
            if (cmd.type == CmdType::Text)
            {
                if (cmd.glyph_count == 0) continue;

                // 图集不存在的字形串谁也画不对，跳过是唯一诚实的选择。
                // 采样别的字体的字形比不画还糟。
                GlyphAtlas *atlas = fonts().atlas(cmd.font);
                if (!atlas) { out.dropped++; continue; }

                if (run.font != cmd.font || run.tex != TEX_NONE || run.count == 0)
                {
                    flush_run();
                    open_run(cmd.font, TEX_NONE);
                }
                for (u32 g = 0; g < cmd.glyph_count; ++g)
                {
                    if (out.instances.size() >= kMaxInstances) { out.dropped++; break; }
                    const GlyphQuad &gq = glyphs[cmd.glyph_start + g];
                    if (gq.dst.w <= 0.0f || gq.dst.h <= 0.0f) continue;
                    InstanceData d{};
                    d.rect[0] = gq.dst.x;
                    d.rect[1] = gq.dst.y;
                    d.rect[2] = gq.dst.w;
                    d.rect[3] = gq.dst.h;
                    put_color(d.color, cmd.color);
                    d.params[0] = PT_TEXT;
                    d.params2[2] = gq.uv0.x;
                    d.params2[3] = gq.uv0.y;
                    d.color2[0] = gq.uv1.x;
                    d.color2[1] = gq.uv1.y;
                    out.instances.push_back(d);
                    run.count++;
                }
                continue;
            }

            if (cmd.rect.w <= 0.0f || cmd.rect.h <= 0.0f) continue;

            // ── Image ────────────────────────────────────────────────
            if (cmd.type == CmdType::Image)
            {
                if (out.instances.size() >= kMaxInstances) { out.dropped++; continue; }
                const Context::Texture *t = ctx.texture(cmd.tex);
                if (!t) { out.dropped++; continue; }

                if (run.tex != cmd.tex || run.font != FONT_INVALID || run.count == 0)
                {
                    flush_run();
                    open_run(FONT_INVALID, cmd.tex);
                }

                InstanceData d{};
                d.rect[0] = cmd.rect.x;
                d.rect[1] = cmd.rect.y;
                d.rect[2] = cmd.rect.w;
                d.rect[3] = cmd.rect.h;
                put_color(d.color, cmd.color);
                d.params[0] = PT_IMAGE;
                // color2 装归一化源矩形。与文本用它装 uv1 是同一约定：
                // 都是"第二组四个 float，不是颜色"。
                d.color2[0] = cmd.src.x;
                d.color2[1] = cmd.src.y;
                d.color2[2] = cmd.src.x + cmd.src.w;
                d.color2[3] = cmd.src.y + cmd.src.h;
                out.instances.push_back(d);
                run.count++;
                continue;
            }

            // ── 形状 ───────────────────────────────────────────────
            // 一个阴影最多压两个实例，所以空间检查按 2 算。
            if (out.instances.size() + 2 > kMaxInstances) { out.dropped++; break; }

            InstanceData d{};
            d.rect[0] = cmd.rect.x;
            d.rect[1] = cmd.rect.y;
            d.rect[2] = cmd.rect.w;
            d.rect[3] = cmd.rect.h;

            // NOTE: 这里不做外扩。阴影的 quad 在下面构建阴影实例时才外扩，
            // 在这里给 `d` 做外扩会把那份扩张漏进形状自己的填充——每个带
            // 阴影的矩形四边都多画 `blur` 像素。之前没被发现，是因为阴影
            // 先画、基本盖住了膨胀的填充；真正露出来的是超出预期边缘的
            // 那圈多出来的环。

            d.radii[0] = cmd.radii.tl;
            d.radii[1] = cmd.radii.tr;
            d.radii[2] = cmd.radii.br;
            d.radii[3] = cmd.radii.bl;

            if (run.font != FONT_INVALID || run.tex != TEX_NONE || run.count == 0)
            {
                flush_run();
                open_run(FONT_INVALID, TEX_NONE);
            }

            switch (cmd.type)
            {
            case CmdType::Rect:
                if (cmd.shadow.blur > 0.0f && quality.shadows != 2)
                {
                    InstanceData sh = d;
                    // 降级半档：半径减半，falloff 随之变窄（同一段代码，没有
                    // 第二套阴影实现 —— 否则降级路径就是一条没人验证过的路）。
                    const f32 blur = quality.shadows == 1 ? cmd.shadow.blur * 0.5f : cmd.shadow.blur;
                    // 只外扩阴影的 quad，让衰减有空间渲染到形状外面。
                    // 形状本身保持自己的矩形。见上面的说明。
                    sh.rect[0] -= blur;
                    sh.rect[1] -= blur;
                    sh.rect[2] += blur * 2.0f;
                    sh.rect[3] += blur * 2.0f;
                    sh.rect[0] += cmd.shadow.offset_x;
                    sh.rect[1] += cmd.shadow.offset_y;
                    put_color(sh.color, cmd.shadow.color);
                    sh.params[0] = PT_SHADOW;
                    sh.params[2] = blur;
                    out.instances.push_back(sh);
                    run.count++;
                }
                put_color(d.color, cmd.color);
                put_color(d.color2, cmd.color2);
                d.params[0] = PT_RECT;
                d.params2[1] = cmd.gradient_horizontal ? 1.0f : 0.0f;
                out.instances.push_back(d);
                run.count++;
                break;

            case CmdType::Border:
                put_color(d.color, cmd.color);
                d.params[0] = PT_BORDER;
                d.params[1] = cmd.thickness;
                out.instances.push_back(d);
                run.count++;
                break;

            case CmdType::Glass:
                put_color(d.color, cmd.color);
                d.params[0] = PT_GLASS;
                d.params[3] = cmd.tint;
                d.params2[0] = cmd.saturation;
                out.instances.push_back(d);
                run.count++;
                out.used_glass = true;
                break;

            case CmdType::Path:
                // 把路径展开成线段。每段变成一个 quad，矩形是线段的包围盒，
                // 方向单独携带，光栅化器据此把 v_local 旋转到线段的坐标系。
                //
                // ⚠ 为什么要带方向：顶点着色器沿屏幕轴展开 quad，轴对齐盒子
                //   表达不了旋转的矩形。以前斜线段被画成横贯自己盒子的水平
                //   条——在一列等长线段里看着还行，画对勾或任何带斜臂的
                //   折线就完全错了。
                if (cmd.path_count >= 2 && cmd.path_start + cmd.path_count <= dl.path_vertices_size())
                {
                    const Vec2 *verts = dl.path_vertices() + cmd.path_start;
                    for (u32 i = 1; i < cmd.path_count; ++i)
                    {
                        if (out.instances.size() >= kMaxInstances) { out.dropped++; break; }

                        const Vec2 &p0 = verts[i - 1];
                        const Vec2 &p1 = verts[i];
                        const f32 dx = p1.x - p0.x;
                        const f32 dy = p1.y - p0.y;
                        const f32 len = sqrtf(dx * dx + dy * dy);
                        if (len < 0.001f) continue;

                        // 单位方向；垂直方向 (-dir.y, dir.x) 由着色器/
                        // 光栅化器从这两个值算出。
                        const f32 dir_x = dx / len;
                        const f32 dir_y = dy / len;

                        // 描边是胶囊形，quad 必须盖住圆头——两端各凸出半个
                        // 粗细。贴紧的盒子会把圆头裁掉，这就是以前圆头看着
                        // 是平的原因。
                        const f32 r = cmd.path_thickness * 0.5f;

                        InstanceData seg = d;
                        const f32 minx = (p0.x < p1.x ? p0.x : p1.x) - r;
                        const f32 miny = (p0.y < p1.y ? p0.y : p1.y) - r;
                        const f32 maxx = (p0.x > p1.x ? p0.x : p1.x) + r;
                        const f32 maxy = (p0.y > p1.y ? p0.y : p1.y) + r;
                        seg.rect[0] = minx;
                        seg.rect[1] = miny;
                        seg.rect[2] = maxx - minx;
                        seg.rect[3] = maxy - miny;

                        // 不设 radii：路径线段没有圆角，圆角盒字段在这里
                        // 没有意义。旧代码往里写 thickness/2，看着像有用
                        // 其实没有。
                        put_color(seg.color, cmd.color);
                        seg.params[0] = PT_PATH;
                        seg.params[1] = cmd.path_thickness;
                        seg.params[2] = len;
                        seg.params[3] = dir_x;
                        seg.params2[0] = (f32)cmd.path_cap;
                        seg.params2[1] = (f32)cmd.path_join;
                        seg.params2[2] = (f32)cmd.path_flags;
                        seg.params2[3] = dir_y;

                        out.instances.push_back(seg);
                        run.count++;
                    }
                }
                break;

            case CmdType::Ellipse:
                put_color(d.color, cmd.color);
                d.params[0] = PT_ELLIPSE;
                out.instances.push_back(d);
                run.count++;
                break;

            case CmdType::Triangle:
                // 顶点在压入时已校验（零面积三角形不会被记录），
                // 这里只做转换。
                if (cmd.path_count == 3 && cmd.path_start + 3 <= dl.path_vertices_size())
                {
                    const Vec2 *v = dl.path_vertices() + cmd.path_start;
                    const f32 cx = cmd.rect.x + cmd.rect.w * 0.5f;
                    const f32 cy = cmd.rect.y + cmd.rect.h * 0.5f;
                    put_color(d.color, cmd.color);
                    d.params[0] = PT_TRIANGLE;
                    d.params[1] = v[0].x - cx;
                    d.params[2] = v[0].y - cy;
                    d.params[3] = v[1].x - cx;
                    d.params2[0] = v[1].y - cy;
                    d.params2[1] = v[2].x - cx;
                    d.params2[2] = v[2].y - cy;
                    out.instances.push_back(d);
                    run.count++;
                }
                break;

            default:
                break;
            }
        }
    flush_run();

        // ── 遮挡剔除 ────────────────────────────────────────────────────
        //
        // 画家算法：若实例 j 在 i 之后绘制，且 j 的"必然不透明覆盖区"
        // （圆的内部区域，|p| <= half - r - 0.5）完整包含 i 的整个 quad，
        // 那么 i 的每一个像素都会被 j 覆盖 —— i 可以直接删掉。
        //
        // 这不是近似，也不是启发式：条件是"必然覆盖"，判不出来就不删。
        // 它同时省掉 CPU 后端的光栅化与 GPU 后端的顶点/片元，所以放在
        // 实例层（两个后端共用），而不是放在某一个后端里。
        //
        // 反面情形都写在这里，因为它们才是真正会出错的地方：
        //   · PT_GLASS 采样背景 ⇒ 不能当遮挡物（删掉它下面的东西会改变
        //     它自己采样到的内容），也不能因为被玻璃盖住而删除 —— 玻璃
        //     下面的实例**看得见**。
        //   · 阴影/边框/文字/图片的覆盖率处处小于 1，core 为空 ⇒ 不当遮挡物。
        //   · 半透明矩形（color.a < 1）同理。
        if (ctx.cull_hidden() && out.instances.size() > 1)
        {
            static thread_local std::vector<pa::Item> items;
            static thread_local std::vector<u8> keep;
            const size_t n = out.instances.size();
            items.resize(n);
            keep.resize(n);

            for (size_t i = 0; i < n; ++i)
            {
                const InstanceData &d = out.instances[i];
                pa::Item &it = items[i];
                it.x0 = std::floor(d.rect[0]);
                it.y0 = std::floor(d.rect[1]);
                it.x1 = std::ceil(d.rect[0] + d.rect[2]);
                it.y1 = std::ceil(d.rect[1] + d.rect[3]);

                // core 默认空 = 不能遮挡
                it.cx0 = it.cy0 = it.cx1 = it.cy1 = 0.0f;

                const bool is_rect = d.params[0] == PT_RECT;
                if (!is_rect || d.color[3] < 1.0f) continue;

                // 完全不透明覆盖区 = 矩形的整数像素范围（四边各留 1 像素余量）。
                //
                // ⚠ 与圆角半径**无关**：参考实现的 SDF 在 q 之外减 r，于是
                //   alpha == 1 的条件是 |p| <= half - 0.5 —— 半径被消掉。
                //   写全推导见 utils/Pa.hpp 的 core_span_row。
                it.cx0 = std::ceil(it.x0) + 1.0f;
                it.cy0 = std::ceil(it.y0) + 1.0f;
                it.cx1 = std::floor(it.x1 - 1.0f) - 1.0f;
                it.cy1 = std::floor(it.y1 - 1.0f) - 1.0f;
            }

            const u32 dropped = pa::drop_occluded(items.data(), n, keep.data(), 64);
            if (dropped > 0)
            {
                // 就地压缩，并按"连续被保留的运行段"重建 batch 列表：
                // batch 是 (first,count) 连续区间，删掉中间的元素必须拆 batch，
                // 否则采样源（字体/纹理）与实例会错位 —— 那是"文字变成别处
                // 的纹理"这类错误，不是画得差一点。
                u32 write = 0;
                Batch run;
                bool run_open = false;
                std::vector<Batch> rebuilt;
                rebuilt.reserve(out.batches.size() + 8);

                for (const Batch &b : out.batches)
                {
                    for (u32 k = 0; k < b.count; ++k)
                    {
                        const u32 idx = b.first + k;
                        if (!keep[idx])
                        {
                            if (run_open) { rebuilt.push_back(run); run_open = false; }
                            continue;
                        }
                        if (!run_open)
                        {
                            run = b;
                            run.first = write;
                            run.count = 0;
                            run_open = true;
                        }
                        out.instances[write++] = out.instances[idx]; // write <= idx 恒成立
                        run.count++;
                    }
                    if (run_open) { rebuilt.push_back(run); run_open = false; }
                }
                out.instances.resize(write);
                out.batches.swap(rebuilt);
                out.culled = dropped;
                ctx.note_hidden_culled(dropped);
            }
        }
}

} // namespace fox
