#pragma once
// Fox draw/display_list.hpp - 原子绘制命令缓冲
//
// Fox 自己的图元层。不用 Nuklear 固定的"矩形 + 纹理"绘制调用，而是记录
// 一条带类型的命令显示列表，由 Vulkan 后端用 SDF（有向距离场）着色器
// 渲染。圆角、阴影、渐变和玻璃模糊这些 Nuklear 后端表达不了的东西由此而来。
//
// 原子性：每条命令自包含（颜色、矩形、参数），列表是扁平数组。控件每帧
// 追加命令，按 z 排序，由 draw() 消费。
//
// 列表跨帧复用（reset 而不重新分配），稳态下每帧分配为零。

#include "core/types.hpp"
#include "draw/font_ids.hpp"
#include <vector>

namespace fox
{
    // ── 命令类型 ──────────────────────────────────────────────────

    enum class CmdType : u8
    {
        Rect,   ///< 填充矩形（可带圆角/渐变/阴影）
        Border, ///< 描边圆角矩形轮廓
        Text,   ///< 字形串（字形缓冲的起始下标、数量）
        Glass,  ///< 磨砂材质（模糊其背景）
        Image,  ///< 采样纹理，用 `color` 着色
        Path,   ///< 描边路径（带圆头/圆角的折线）
        Ellipse,  ///< 内切于 `rect` 的填充椭圆
        Triangle, ///< 填充三角形；顶点存在路径顶点缓冲里
        ClipPush,
        ClipPop,
    };

    /// 指向 Context 持有的纹理的句柄。TEX_NONE 什么都不画。
    ///
    /// 刻意与 FontId 分开：字形图集和图标图集都是"可采样的纹理"，但它们
    /// 的上传、更新、释放节奏完全不同。并进一个 id 空间会让字体图集被
    /// 图标挤掉，或反过来。
    using TexId = u32;
    inline constexpr TexId TEX_NONE = 0;

    /// 命令列表里的一段文本。字形单元放在并行数组里，命令本身保持小巧、
    /// 对缓存友好。
    struct GlyphQuad
    {
        Rect dst;        ///< 目标矩形，窗口像素单位
        Vec2 uv0, uv1;   ///< 图集 UV
        Color color;     ///< 绘制时调制（用于选区/悬停着色）
    };

    /// 圆角半径：4 个角 [TL, TR, BR, BL]。
    struct CornerRadii
    {
        f32 tl = 0.0f, tr = 0.0f, br = 0.0f, bl = 0.0f;

        CornerRadii() = default;
        CornerRadii(f32 all) : tl(all), tr(all), br(all), bl(all) {}
        CornerRadii(f32 tl_, f32 tr_, f32 br_, f32 bl_) : tl(tl_), tr(tr_), br(br_), bl(bl_) {}
    };

    /// 投影参数（后端作为 Rect 的一部分渲染）。
    struct Shadow
    {
        f32 offset_x = 0.0f;
        f32 offset_y = 0.0f;
        f32 blur = 0.0f;   ///< 近似高斯半径，单位像素
        f32 spread = 0.0f; ///< 模糊前先放大形状
        Color color{0, 0, 0, 0};
    };

    /// 一条记录的绘制命令。
    struct Cmd
    {
        CmdType type = CmdType::Rect;
        f32 z = 0.0f; ///< 排序键：小的先画。

        Rect rect;

        Color color{0, 0, 0, 255};
        Color color2{0, 0, 0, 0}; ///< 渐变终点 / 第二个色标

        CornerRadii radii;
        Shadow shadow;

        // 仅 Glass 用的旋钮
        f32 blur = 0.0f;         ///< 背景模糊半径
        f32 tint = 0.0f;         ///< 着色强度
        f32 saturation = 1.0f;   ///< 背景饱和度提升

        // 仅 Border 用
        f32 thickness = 1.0f;

        /// 带第二个色标（color2.a > 0）的 Rect 命令的渐变方向。
        /// false = 垂直（from -> to 从上到下），true = 水平。
        bool gradient_horizontal = false;

        // 仅 Text 用
        FontId font = FONT_INVALID; ///< 这些单元属于哪个字形图集
        u32 glyph_start = 0;
        u32 glyph_count = 0;

        // 仅 Image 用
        TexId tex = TEX_NONE;      ///< 采样哪个纹理
        /// 要采样的归一化子矩形，默认整个纹理。
        ///
        /// 图标图集（sheet）靠它才实用。没有它，每个图标都要自己的纹理、
        /// 自己的描述符集，20 个图标的工具栏画 20 个方块就要烧 20 个集。
        Rect src{0.0f, 0.0f, 1.0f, 1.0f};

        // 仅 Path 用
        u32 path_start = 0;       ///< 路径顶点缓冲的起始下标
        u32 path_count = 0;       ///< 顶点数
        f32 path_thickness = 1.0f; ///< 描边宽度，逻辑像素
        u8  path_join = 0;        ///< 0=miter, 1=round, 2=bevel
        u8  path_cap = 0;         ///< 0=butt, 1=round, 2=square
        u8  path_flags = 0;       ///< 预留（虚线等）
        // 仅 Triangle 用：path_start/path_count 指向同一个顶点缓冲
        // （总是 3 个顶点）；`type == CmdType::Triangle` 时使用。

        // 仅 Clip 用
        Rect clip;
    };

    // ── 显示列表 ───────────────────────────────────────────────────

    /// 扁平命令缓冲，每帧复用。
    class DisplayList
    {
    public:
        void reset()
        {
            cmds_.clear();
            glyphs_.clear();
            // 裁剪栈也必须清 —— 它是**本帧**的嵌套状态。
            //
            // ⚠ 之前没清，于是栈跨帧累积：第二帧的 push_clip_pop 弹出的是
            // **上一帧**留下的条目，发出去的 ClipPop 命令与真实的嵌套对不上，
            // 裁剪区域完全错乱（表现为「界面内容被整片裁掉、窗口空白」）。
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

        /// 双色标线性渐变矩形。
        ///
        /// `horizontal` 选方向：默认从上（from）到下（to）；true 从左到右。
        ///
        /// 渐变需要两个色标和方向，所以它是一个具名入口而不是 push() 上多
        /// 两个参数：只写一半的渐变（有第二个色标没方向）正是那种会没有
        /// 任何提示地画成纯色矩形的东西。instance.cpp 把第二个色标的 alpha
        /// 当启用标志，方向读 Cmd::gradient_horizontal，与 shaders.frag
        /// （params2[1]）对应。
        void push_gradient(Rect r, Color from, Color to, bool horizontal = false, f32 z = 0.0f)
        {
            Cmd cmd;
            cmd.type = CmdType::Rect;
            cmd.rect = r;
            cmd.color = from;
            cmd.color2 = to;
            cmd.color2.a = to.a > 0 ? to.a : 255; // a > 0 is the enable flag
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

        /// 把 `tex` 采样到 `r`。`c` 与采样到的 RGBA 相乘，不透明白色显示
        /// 原纹理，{255,255,255,0} 让它不可见。`src` 是归一化子矩形；
        /// 整个纹理传 {0,0,1,1}。
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

        /// 关闭最内层裁剪。
        ///
        /// **不配对**的 pop 是空操作（不发命令、不改状态）——fox.h 就是
        /// 这么承诺的。以前它不管情况都发 ClipPop 命令，多余的 pop 会让
        /// 渲染器的栈下溢（或破坏下一个裁剪的范围）。
        void push_clip_pop()
        {
            if (clip_stack_.empty()) return;
            clip_stack_.pop_back();
            Cmd cmd;
            cmd.type = CmdType::ClipPop;
            cmds_.push_back(cmd);
        }

        /// 当前生效的裁剪区域（所有打开的裁剪的交集）。
        /// 空矩形表示"没有裁剪"。
        Rect current_clip() const
        {
            if (clip_stack_.empty()) return Rect{};
            Rect r = clip_stack_.back();
            for (size_t i = 0; i + 1 < clip_stack_.size(); ++i)
                r = r.intersected(clip_stack_[i]);
            return r;
        }

        /// 压入一条描边路径（带端点/连接样式的折线）。
        ///
        /// 路径由一串逻辑像素坐标点定义。返回所压入命令的下标。
        ///
        /// `cap`: 0=butt, 1=round, 2=square
        /// `join`: 0=miter, 1=round, 2=bevel
        /// `flags`: 预留（虚线样式等）
        u32 push_path(const Vec2 *points, u32 count, f32 thickness, Color color,
                      u8 cap = 1, u8 join = 1, u8 flags = 0, f32 z = 0.0f)
        {
            if (count < 2) return (u32)-1;

            const u32 start = (u32)path_vertices_.size();
            for (u32 i = 0; i < count; ++i)
                path_vertices_.push_back(points[i]);

            Cmd cmd;
            cmd.type = CmdType::Path;
            // ⚠ 包围盒在这里算，这不是优化。
            //   build_instances() 会丢掉所有 rect 非正的命令，损坏跟踪 /
            //   裁剪 / 遮挡剔除都依赖 cmd.rect。旧代码把它留成 {0,0,0,0}
            //   并注释"由后端计算"——而从没有后端重算过，于是每条路径
            //   命令没到光栅化器就被丢了。这就是描边路径"存在"、但复选框
            //   还在用一串点的原因。
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

        /// 压入一个内切于 `r` 的填充椭圆。
        ///
        /// 具名入口而不是 push() 多几个参数：矩形本身就是形状（中心 =
        /// 矩形中心，半径 = rect.w/2、rect.h/2），不存在会被 SDF 忽略的
        /// 多余半径/粗细字段。`r` 小于 1x1 像素时不画——与 Nuklear 的
        /// nk_fill_circle 用的截断一致，保留它让两个后端对"小到不画"的
        /// 判定一致。
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

        /// 压入一个填充三角形，顶点为逻辑像素坐标。
        ///
        /// 顶点放进路径共用的同一个顶点缓冲：三角形是多边形的退化情形，
        /// 再开一个缓冲就多一份要同步的东西。`rect` 是精确的 AABB，裁剪、
        /// 损坏跟踪和遮挡剔除都按它判断——存得比实际大就会多画。
        ///
        /// 退化三角形（面积为零）不画：没有覆盖可算，Nuklear 在这种情形
        /// 下也不画。
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

        // 诊断
        size_t capacity_bytes() const
        {
            return cmds_.capacity() * sizeof(Cmd)
                 + glyphs_.capacity() * sizeof(GlyphQuad)
                 + path_vertices_.capacity() * sizeof(Vec2);
        }

    private:
        std::vector<Cmd> cmds_;
        std::vector<GlyphQuad> glyphs_;
        /// 打开的裁剪，最内的在最后。让不配对的 pop 成为空操作，
        /// 也让 current_clip() 能报告生效区域。
        std::vector<Rect> clip_stack_;
        std::vector<Vec2> path_vertices_;
    };

} // namespace fox