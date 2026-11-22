#include "instance.hpp"
#include "text/font.hpp"
#include "utils/Pa.hpp"

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
        ctx.note_hidden_culled(0);
        if (out.instances.capacity() < 1024) out.instances.reserve(1024);
        if (out.batches.capacity() < 32) out.batches.reserve(32);

        const Cmd *cmds = dl.cmds();
        const GlyphQuad *glyphs = dl.glyphs();
        const u32 n = dl.cmd_count();

        const Context::Quality &quality = ctx.quality();

        const size_t kMaxInstances = 16384;

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

            if (cmd.type == CmdType::Text)
            {
                if (cmd.glyph_count == 0) continue;

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
                d.color2[0] = cmd.src.x;
                d.color2[1] = cmd.src.y;
                d.color2[2] = cmd.src.x + cmd.src.w;
                d.color2[3] = cmd.src.y + cmd.src.h;
                out.instances.push_back(d);
                run.count++;
                continue;
            }

            if (out.instances.size() + 2 > kMaxInstances) { out.dropped++; break; }

            InstanceData d{};
            d.rect[0] = cmd.rect.x;
            d.rect[1] = cmd.rect.y;
            d.rect[2] = cmd.rect.w;
            d.rect[3] = cmd.rect.h;


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
                    const f32 blur = quality.shadows == 1 ? cmd.shadow.blur * 0.5f : cmd.shadow.blur;
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

                        const f32 dir_x = dx / len;
                        const f32 dir_y = dy / len;

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

                it.cx0 = it.cy0 = it.cx1 = it.cy1 = 0.0f;

                const bool is_rect = d.params[0] == PT_RECT;
                if (!is_rect || d.color[3] < 1.0f) continue;

                it.cx0 = std::ceil(it.x0) + 1.0f;
                it.cy0 = std::ceil(it.y0) + 1.0f;
                it.cx1 = std::floor(it.x1 - 1.0f) - 1.0f;
                it.cy1 = std::floor(it.y1 - 1.0f) - 1.0f;
            }

            const u32 dropped = pa::drop_occluded(items.data(), n, keep.data(), 64);
            if (dropped > 0)
            {
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
                        out.instances[write++] = out.instances[idx];
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

}