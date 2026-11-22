#pragma once

#include "core/types.hpp"
#include "draw/font_ids.hpp"
#include <vector>

namespace fox
{

    enum class CmdType : u8
    {
        Rect,
        Border,
        Text,
        Glass,
        Image,
        Path,
        Ellipse,
        Triangle,
        ClipPush,
        ClipPop,
    };

    using TexId = u32;
    inline constexpr TexId TEX_NONE = 0;

    struct GlyphQuad
    {
        Rect dst;
        Vec2 uv0, uv1;
        Color color;
    };

    struct CornerRadii
    {
        f32 tl = 0.0f, tr = 0.0f, br = 0.0f, bl = 0.0f;

        CornerRadii() = default;
        CornerRadii(f32 all) : tl(all), tr(all), br(all), bl(all) {}
        CornerRadii(f32 tl_, f32 tr_, f32 br_, f32 bl_) : tl(tl_), tr(tr_), br(br_), bl(bl_) {}
    };

    struct Shadow
    {
        f32 offset_x = 0.0f;
        f32 offset_y = 0.0f;
        f32 blur = 0.0f;
        f32 spread = 0.0f;
        Color color{0, 0, 0, 0};
    };

    struct Cmd
    {
        CmdType type = CmdType::Rect;
        f32 z = 0.0f;

        Rect rect;

        Color color{0, 0, 0, 255};
        Color color2{0, 0, 0, 0};

        CornerRadii radii;
        Shadow shadow;

        f32 blur = 0.0f;
        f32 tint = 0.0f;
        f32 saturation = 1.0f;

        f32 thickness = 1.0f;

        bool gradient_horizontal = false;

        FontId font = FONT_INVALID;
        u32 glyph_start = 0;
        u32 glyph_count = 0;

        TexId tex = TEX_NONE;
        Rect src{0.0f, 0.0f, 1.0f, 1.0f};

        u32 path_start = 0;
        u32 path_count = 0;
        f32 path_thickness = 1.0f;
        u8 path_join = 0;
        u8 path_cap = 0;
        u8 path_flags = 0;

        Rect clip;
    };


    class DisplayList
    {
    public:
        void reset()
        {
            cmds_.clear();
            glyphs_.clear();
            clip_stack_.clear();
        }

        void push(const Cmd &c) { cmds_.push_back(c); }
        void push(Rect r, Color c, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Rect;
            cmd.rect = r;
            cmd.color = c;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void push_rounded(Rect r, Color c, const CornerRadii &rad, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Rect;
            cmd.rect = r;
            cmd.color = c;
            cmd.radii = rad;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void push_border(Rect r, Color c, f32 thickness, const CornerRadii &rad, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Border;
            cmd.rect = r;
            cmd.color = c;
            cmd.thickness = thickness;
            cmd.radii = rad;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void push_gradient(Rect r, Color from, Color to, bool horizontal = false, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Rect;
            cmd.rect = r;
            cmd.color = from;
            cmd.color2 = to;
            cmd.color2.a = to.a > 0 ? to.a : 255;
            cmd.gradient_horizontal = horizontal;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void push_glass(Rect r, const CornerRadii &rad, Color tint, f32 blur, f32 tint_amt,
                        f32 saturation, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Glass;
            cmd.rect = r;
            cmd.color = tint;
            cmd.radii = rad;
            cmd.blur = blur;
            cmd.tint = tint_amt;
            cmd.saturation = saturation;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void push_image(const Rect &r, TexId tex, Color c, const Rect &src = Rect{0, 0, 1, 1},
                        f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Image;
            cmd.rect = r;
            cmd.tex = tex;
            cmd.color = c;
            cmd.src = src;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        u32 push_text(Rect r, Color c, FontId font, u32 glyph_start, u32 glyph_count, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Text;
            cmd.rect = r;
            cmd.color = c;
            cmd.font = font;
            cmd.glyph_start = glyph_start;
            cmd.glyph_count = glyph_count;
            cmd.z = z;
            cmds_.push_back(cmd);
            return (u32)(cmds_.size() - 1);
        }

        void push_clip(const Rect &r)
        {
            Cmd cmd;
            cmd.type = CmdType::ClipPush;
            cmd.rect = r;
            cmds_.push_back(cmd);
            clip_stack_.push_back(r);
        }

        void push_clip_pop()
        {
            if (clip_stack_.empty()) return;
            clip_stack_.pop_back();
            Cmd cmd;
            cmd.type = CmdType::ClipPop;
            cmds_.push_back(cmd);
        }

        Rect current_clip() const
        {
            if (clip_stack_.empty()) return Rect{};
            Rect r = clip_stack_.back();
            for (size_t i = 0; i + 1 < clip_stack_.size(); ++i)
                r = r.intersected(clip_stack_[i]);
            return r;
        }

        u32 push_path(const Vec2 *points, u32 count, f32 thickness, Color color,
                      u8 cap = 1, u8 join = 1, u8 flags = 0, f32 z = 0.0f)
        {
            if (count < 2) return (u32)-1;

            const u32 start = (u32)path_vertices_.size();
            for (u32 i = 0; i < count; ++i)
                path_vertices_.push_back(points[i]);

            Cmd cmd;
            cmd.type = CmdType::Path;
            const f32 r = (thickness > 0.0f ? thickness : 1.0f) * 0.5f;
            f32 minx = points[0].x, miny = points[0].y;
            f32 maxx = points[0].x, maxy = points[0].y;
            for (u32 i = 1; i < count; ++i)
            {
                if (points[i].x < minx) minx = points[i].x;
                if (points[i].y < miny) miny = points[i].y;
                if (points[i].x > maxx) maxx = points[i].x;
                if (points[i].y > maxy) maxy = points[i].y;
            }
            cmd.rect = Rect{minx - r, miny - r, (maxx - minx) + 2.0f * r,
                            (maxy - miny) + 2.0f * r};
            cmd.color = color;
            cmd.path_thickness = thickness;
            cmd.path_cap = cap;
            cmd.path_join = join;
            cmd.path_flags = flags;
            cmd.path_start = start;
            cmd.path_count = count;
            cmd.z = z;
            cmds_.push_back(cmd);
            return (u32)(cmds_.size() - 1);
        }

        void push_ellipse(const Rect &r, Color c, f32 z = 0.0f)
        {
            if (r.w < 1.0f || r.h < 1.0f) return;
            Cmd cmd;
            cmd.type = CmdType::Ellipse;
            cmd.rect = r;
            cmd.color = c;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void push_triangle(const Vec2 &a, const Vec2 &b, const Vec2 &c, Color color, f32 z = 0.0f)
        {
            const f32 area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (area2 > -1e-4f && area2 < 1e-4f) return;

            const u32 start = (u32)path_vertices_.size();
            path_vertices_.push_back(a);
            path_vertices_.push_back(b);
            path_vertices_.push_back(c);

            Cmd cmd;
            cmd.type = CmdType::Triangle;
            const f32 minx = a.x < b.x ? a.x : b.x;
            const f32 miny = a.y < b.y ? a.y : b.y;
            const f32 maxx = a.x > b.x ? a.x : b.x;
            const f32 maxy = a.y > b.y ? a.y : b.y;
            cmd.rect = Rect{minx < c.x ? minx : c.x, miny < c.y ? miny : c.y,
                            (maxx > c.x ? maxx : c.x) - (minx < c.x ? minx : c.x),
                            (maxy > c.y ? maxy : c.y) - (miny < c.y ? miny : c.y)};
            cmd.color = color;
            cmd.path_start = start;
            cmd.path_count = 3;
            cmd.z = z;
            cmds_.push_back(cmd);
        }

        void add_glyph(const GlyphQuad &g) { glyphs_.push_back(g); }
        u32 glyph_count() const { return (u32)glyphs_.size(); }
        const Cmd *cmds() const { return cmds_.data(); }
        u32 cmd_count() const { return (u32)cmds_.size(); }
        const GlyphQuad *glyphs() const { return glyphs_.data(); }
        const Vec2 *path_vertices() const { return path_vertices_.data(); }
        u32 path_vertices_size() const { return (u32)path_vertices_.size(); }

        Cmd &cmd_at(u32 i) { return cmds_[i]; }

        size_t capacity_bytes() const
        {
            return cmds_.capacity() * sizeof(Cmd)
                 + glyphs_.capacity() * sizeof(GlyphQuad)
                 + path_vertices_.capacity() * sizeof(Vec2);
        }

    private:
        std::vector<Cmd> cmds_;
        std::vector<GlyphQuad> glyphs_;
        std::vector<Rect> clip_stack_;
        std::vector<Vec2> path_vertices_;
    };

}