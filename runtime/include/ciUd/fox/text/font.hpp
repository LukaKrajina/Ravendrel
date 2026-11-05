#pragma once
// Fox text/font.hpp - 字体加载、字形栅格化、图集管理
//
// Fox 自己持有字体栈。不复用 Nuklear 的字体图集，也不链接 Nuklear
// （TrueType 代码为何私自带入，见 detail/stbtt_impl.hpp）。字形按需栅格化
// 并装进可增长的图集，这样字符集可以在运行时发现——对 CJK 很关键，
// 需要的 2 万字形没法预先知道。
//
// ── 本模块要挡住的 stb_truetype API 陷阱 ─────────────────────────
// 以下都用 msyh.ttc / simsun.ttc / arial.ttf 实测过，每一条都是静默出错
// （数字看着合理但是错的），而不是报错：
//
//  1. stbtt_InitFont(info, data, OFFSET) 收的是字节偏移，而
//     stbtt_PackFontRanges(.., INDEX, ..) 收的是 face 下标。.ttc 里两者
//     不同（offset=32 vs index=0）；互相喂错会索引超出 face 数，崩在
//     CFF 解析器里。
//
//  2. 没有任何东西隐式应用 info->scale。stbtt_GetFontVMetrics 和
//     stbtt_GetCodepointHMetrics 返回的都是原始字体单位；
//     stbtt_GetCodepointBitmap 要显式传缩放。以为度量返回的是像素，会让
//     所有基线偏出屏幕约 100 倍。本模块每次都显式缩放。
//
//  3. GetCodepointHMetrics 的参数顺序是 (info, cp, &advance, &lsb)。
//     写反的话 advance 和 bearing 会静默互换——两个都像合理的整数。
//
//  4. 用 ScaleForMappingEmToPixels 烘焙的缩放不影响 GetCodepointBitmap；
//     给烘焙过的 fontinfo 传 1,1 会按字体单位栅格化（20px 的 'A' 画成
//     约 1394px）。度量路径和位图路径要分开处理。

#include "core/types.hpp"
#include "draw/display_list.hpp"
#include "draw/font_ids.hpp"
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>

namespace fox
{

    /// 每个 face 字形图集的边长，像素。
    ///
    /// 字形按需栅格化并装进 shelf 分配器，所以它必须装下某个 face 某个字号
    /// 下应用可能显示的全部字形。1024 在 16px 下约 250 个 CJK 字形，或约
    /// 4000 个拉丁字形。装不下会报出来，而不是悄悄丢文字
    /// （见 GlyphAtlas::alloc）。
    inline constexpr int FOX_GLYPH_ATLAS_SIZE = 1024;

    // ── 字形度量（全部为像素）──────────────────────────────────

    struct GlyphMetrics
    {
        f32 advance = 0.0f;
        f32 bearing_x = 0.0f;
        f32 bearing_y = 0.0f; ///< 从基线到字形顶部的距离
        f32 width = 0.0f;
        f32 height = 0.0f;
    };

    // ── 字体 face ──────────────────────────────────────────────────────

    struct FontFace
    {
        FontId id = FONT_INVALID;
        void *ttf = nullptr;        ///< stbtt_fontinfo*（本模块持有）
        std::vector<u8> data;       ///< 字体文件字节的持有副本
        int face_offset = 0;        ///< face 在 .ttc 中的字节偏移

        f32 pixel_size = 0.0f;      ///< 请求的像素高度，逻辑单位
        f32 scale = 0.0f;           ///< 每字体单位多少逻辑像素（见陷阱 2）
        /// 每字体单位多少物理像素，用于栅格化图集。
        ///
        /// ⚠ 它是 `scale * dpi`，两者不能混。度量（advance、ascent、行高）
        ///   以逻辑单位报告，让布局与 DPI 无关；位图按物理分辨率栅格化，
        ///   这样 2x 屏上也清晰。按逻辑尺寸栅格化，HiDPI 屏上文字发虚；
        ///   按物理尺寸栅格化，布局处处都要做除法。
        f32 raster_scale = 0.0f;
        /// 栅格化密度（物理 / 逻辑）。位图尺寸从 stbtt 返回的是物理像素，
        /// 除以它得到 Fox 其余部分使用的逻辑度量。
        f32 dpi = 1.0f;
        f32 ascent = 0.0f;          ///< px，从字体单位换算（逻辑）
        f32 descent = 0.0f;         ///< px（负值）
        f32 line_height = 0.0f;     ///< px，ascent - descent + 行距

        bool valid() const { return ttf != nullptr && scale > 0.0f; }
    };

    // ── 字形图集 ────────────────────────────────────────────────────

    /// 已装箱字形在图集里的位置 + 度量。
    struct AtlasGlyph
    {
        Rect uv;        ///< 图集纹理内的区域（归一化 0..1）
        /// 该字形的目标尺寸，逻辑单位。
        ///
        /// ⚠ 是逻辑单位，不是纹素数。图集按物理分辨率存位图（任何 DPI 下
        ///   都清晰），uv 寻址的是那些纹素，但 shape_text() 按这个决定目标
        ///   尺寸。用物理单位的话，2x 下每个字形四边形是 advance 的两倍宽，
        ///   文字会叠成看不清的一团。
        Rect pixel_rect;
        GlyphMetrics m; ///< advance/bearing，像素
        f32 bitmap_left = 0.0f;  ///< 笔位到位图左边的偏移（px）
        f32 bitmap_top = 0.0f;   ///< 基线到位图顶部的偏移（px）
        bool empty = true;       ///< 空白：无位图
    };

    /// 单个 FontFace 的 shelf 装箱分配器 + 字形缓存。
    class GlyphAtlas
    {
    public:
        void init(int width, int height);
        void shutdown();

        /// 把图集绑定到持有 stbtt_fontinfo + 字节的 face。必须在 init() 之后、
        /// 任何 glyph() 请求之前调用。
        ///
        /// 同时用 face id 播种代际计数器，两个不同 face 的图集永远不会报出
        /// 相同的代际号（见下）。
        void bind(FontFace *f)
        {
            face_ = f;
            generation_ = f ? (u32)f->id * 100000u : 0u;
            dirty_ = true;
        }

        /// 单调计数器，图集像素一变（某个字形首次栅格化）就递增。缓存图集
        /// 派生数据的后端——尤其是 damage 跟踪——必须把它折进帧键：字形是
        /// 惰性栅格化的，一个字形会话中途首次出现时像素变了而显示列表
        /// 完全没变。
        u32 generation() const { return generation_; }

        /// 取一个码点的字形（首次使用时栅格化 + 装箱）。
        const AtlasGlyph &glyph(u32 codepoint);

        int width() const { return width_; }
        int height() const { return height_; }
        const std::vector<u8> &pixels() const { return pixels_; }
        bool dirty() const { return dirty_; }
        void clear_dirty() { dirty_ = false; }

        size_t glyph_count() const { return cache_.size(); }
        /// 占用率 0..1——让应用在图集装满前发出警告。
        f32 occupancy() const;

        /// 在归一化 (u,v) 采样图集覆盖率。越界返回 0。
        ///
        /// 边缘钳位而不回绕，与采样器的 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
        /// 一致。这让 CPU 光栅化器不用模拟采样器行为就能产出与 GPU 相同的
        /// 字形像素。
        f32 sample(f32 u, f32 v) const
        {
            if (width_ <= 0 || height_ <= 0 || pixels_.empty()) return 0.0f;
            int x = (int)(u * (f32)width_);
            int y = (int)(v * (f32)height_);
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= width_) x = width_ - 1;
            if (y >= height_) y = height_ - 1;
            return (f32)pixels_[(size_t)y * width_ + x] / 255.0f;
        }

    private:
        /// 尝试分配 w x h；返回图集像素原点，失败返回 {-1,-1}。
        bool alloc(int w, int h, int &out_x, int &out_y);

        FontFace *face_ = nullptr;
        AtlasGlyph fallback_;      ///< 无法产出字形时返回它
        int width_ = 0;
        int height_ = 0;
        std::vector<u8> pixels_;
        std::unordered_map<u32, AtlasGlyph> cache_;

        // Shelf 装箱器状态
        int shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
        bool full_ = false;
        bool dirty_ = false;
        /// 见 generation()。init() 不重置它：对图集实例而言计数器必须单调，
        /// 否则重新 init 在 damage 跟踪器看来像"什么都没变"。
        u32 generation_ = 0;
    };

    // ── 字体管理器 ───────────────────────────────────────────────────

    /// 持有已加载的 face 和它们的图集。每进程一个。
    class FontManager
    {
    public:
        /// 加载字体文件。支持 .ttf、.otf、.ttc（face_index 选集合中的哪个
        /// face）。失败返回 FONT_INVALID。
        ///
        /// `pixel_size` 是逻辑单位。图集按 pixel_size * dpi() 栅格化，
        /// HiDPI 屏上字形才清晰。
        FontId load_file(const char *path, f32 pixel_size, int face_index = 0);

        /// 查找并加载覆盖 CJK 的系统 UI 字体，找不到时得体降级。
        /// 找不到任何合适的返回 FONT_INVALID。
        FontId load_system_ui(f32 pixel_size);

        /// 覆盖主 UI face 的来源文件，例如随应用发布专用字体。传 nullptr
        /// 回到平台默认搜索。
        ///
        /// 它主要为了让字体回退可观察。默认搜索刻意优先选覆盖 CJK 的 face
        /// （雅黑、Noto CJK、苹方），所以多数机器上主字体恰好覆盖 CJK，
        /// 回退链从不触发——"回退链可用"就成了没测过的说法。把主字体指向
        /// 只有拉丁的 face，回退链才会真正干活。
        void set_primary_path(const char *path) { primary_path_ = path ? path : ""; }
        const char *primary_path() const { return primary_path_.c_str(); }

        /// 栅格化密度：每逻辑像素的物理像素数。
        ///
        /// 启动时设一次（窗口移到不同缩放的显示器时再设）。只影响调用之后
        /// 加载的 face，所以 Context 也按有效字号给字体缓存做键——改 DPI
        /// 不能返回一个按旧密度栅格化的 face，否则字号对了字却是糊的。
        void set_dpi(f32 d) { dpi_ = d > 0.0f ? d : 1.0f; }
        f32 dpi() const { return dpi_; }

        FontFace *face(FontId id);
        GlyphAtlas *atlas(FontId id);

        /// 这个 face 从哪加载的（路径，或平台搜索一无所获用了兜底 face 时的
        /// "<fallback:N>"）。用于诊断，不是渲染：不知道真正打开的是哪个文件，
        /// "CJK 文字是豆腐块"就没法回答，而候选列表里有十几个路径。
        std::string source_of(FontId id) const;

        /// 便捷：量一段 UTF-8 字符串的像素宽度。
        f32 measure(FontId id, const char *utf8);
        /// 字体的行高。
        f32 line_height(FontId id);
        /// 某码点的字形度量（布局用）。
        const AtlasGlyph &glyph_at(FontId id, u32 codepoint);

        /// 所有已加载图集 generation() 之和。任何一个 face 的任何一个字形
        /// 被栅格化，这个数就变。代价低（face 就那么几个），也是帧缓存
        /// 发现惰性栅格化文字的唯一正确途径。
        u32 atlas_generation() const;

        // ── 字形覆盖与字体回退 ─────────────────────────────
        //
        // 覆盖率是字体回退的全部依据，弄错的话拉丁文情形看不出来，CJK
        // 情形是灾难：没有 CJK 覆盖的 face 回答"没字形"的方式，与"从没被
        // 问过"的 face 一模一样。

        /// 这个 face 真的有 `codepoint` 的字形吗？
        ///
        /// 问字体本身。不查图集也不栅格化，原因有二：为整个字符串判定覆盖
        /// 不能以分配图集单元为副作用，而且 stb_truetype 的回答需要解读
        /// 而不是照单全信（见实现）。
        bool has_glyph(FontId id, u32 codepoint) const;

        /// 该由哪个 face 画这个码点：`primary` 有就用它，否则用回退链里
        /// 第一个有的，再否则回到 `primary`。
        ///
        /// 回退到 `primary` 而不是"什么都不画"是刻意的：缺字形就该看起来
        /// 像缺字形。丢掉字符会把字体问题变成数据问题。
        FontId resolve(FontId primary, u32 codepoint) const;

        /// `primary` 缺码点时按顺序尝试的 face。
        void set_fallback_chain(std::vector<FontId> chain) { fallbacks_ = std::move(chain); }
        const std::vector<FontId> &fallback_chain() const { return fallbacks_; }

        /// 按 `pixel_size` 加载平台常用的回退 face（CJK、符号）。尽力而为：
        /// 平台没有不算错，链留空、所有字形都走主字体即可。
        ///
        /// 在 load_system_ui() 之后调用，并用相同的像素字号加载，这样回退
        /// 字体与主字体共享基线和行高，而不是在一行里上下跳。
        void load_fallback_faces(f32 pixel_size);

        /// face 的加载来源文件，id 未知则返回 ""。诊断用：不知道十几个
        /// 候选路径里真正打开的是哪个，"CJK 文字是豆腐块"没法回答。
        const char *face_source(FontId id) const;

        void shutdown();

    private:
        struct Entry
        {
            FontFace face;
            GlyphAtlas atlas;
            /// `face` 的加载来源。留给诊断：不知道真正打开的是哪个文件，
            /// "CJK 文字是豆腐块"没法回答，而 load_system_ui 的候选列表里
            /// 有十几个路径。
            std::string source;
        };

        /// 字体文件的原始字节，缓存起来，让同一字体的多个字号不必各自重读
        /// 几 MB 的文件（也避免持有 N 份副本）。
        struct FileCacheEntry
        {
            std::string path;
            int face_index = 0;
            std::vector<u8> data;
        };

        // ⚠ 必须是 deque，不是 vector。
        //   GlyphAtlas::bind() 存的是指向所属 Entry（&e.face）的裸指针。
        //   std::vector 增长时会搬元素，加载第二个字号就重分配，之前绑定的
        //   指针全部悬空——之后 glyph() 解引用已释放内存，所有字形静默退化成
        //   豆腐块。std::deque 在 push_back 时从不失效已有元素的引用。
        std::deque<Entry> faces_;

        /// 回退 face，按优先级排序。见 resolve()。
        std::vector<FontId> fallbacks_;

        /// 显式指定的主字体路径，"" 表示走平台搜索。
        std::string primary_path_;

        std::vector<FileCacheEntry> file_cache_;

        /// 今后加载的 face 的栅格化密度。见 set_dpi()。
        f32 dpi_ = 1.0f;
    };

    FontManager &fonts();

    // ── UTF-8 辅助 ──────────────────────────────────────────────────

    /// 从 UTF-8 字符串解出下一个码点，推进 pos。
    /// 字符串末尾返回 -1。非法字节给出 U+FFFD。
    i32 utf8_next(const char *s, size_t len, size_t &pos);
    /// UTF-8 字符串的码点数（不是字节数）。
    size_t utf8_len(const char *s, size_t len);

    // ── 文字整形 / 排版 ──────────────────────────────────────────

    /// 把 UTF-8 字符串沿笔位整形为字形四边形。
    ///
    /// `out` 每个可见字形收一个 GlyphQuad。返回发出的四边形数，并把总推进
    /// 宽度加到 *pen_x。
    ///
    /// 刻意保持简单：不做复杂整形（无 kerning 表、无 BiDi）。拉丁 + CJK 用
    /// 线性推进就能排好；阿拉伯/印度系文字的重排不在本里程碑范围内。
    u32 shape_text(DisplayList &dl, FontId font, const char *utf8, const Vec2 &pen,
                   Color color, f32 max_width, u32 &out_glyph_start);

    // -- 带长度的形式 ------------------------------------------------
    //
    // 上面的 (const char*) 形式用 strlen 量宽度，无法处理缓冲的**切片**——
    // 比如多行文档里的一行。把指针指向字符串中间不是"一个更短的字符串"：
    // strlen 会一直跑到下一个 NUL，于是每"一行"都把文档的整个剩余部分重画
    // 一遍，和自己叠在一起。
    //
    // 这些形式显式收长度。它们不是优化，而是整形子区间的唯一正确方式——
    // 也不能把调用方往"把一行拷进固定缓冲"赶，因为不存在安全的行长上限
    // （压缩过的 JS、一整行超长日志、粘贴进来没有换行的段落）。
    //
    // NUL 结尾的形式是这些的薄封装，排版数学只有一份实现。
    u32 shape_text_n(DisplayList &dl, FontId font, const char *utf8, size_t len, const Vec2 &pen,
                     Color color, f32 max_width, u32 &out_glyph_start);

    /// 只测量，不发绘制命令。
    f32 text_width(FontId font, const char *utf8);
    f32 text_width_n(FontId font, const char *utf8, size_t len);

    /// `utf8` 内字节 `byte_offset` 处的水平偏移，从笔位原点量起。用于放置光标。
    ///
    /// 逐码点走并累加推进宽度，所以一个字符贡献一次推进，而不是每字节一次。
    /// 落在码点内部的字节偏移按该码点的起点处理——光标永远不会画在字符中间，
    /// 问它"会在"哪里没有意义。
    f32 text_offset_x(FontId font, const char *utf8, size_t byte_offset);
    f32 text_offset_x_n(FontId font, const char *utf8, size_t len, size_t byte_offset);

    /// 离 `x` 最近的码点的字节偏移，用于点击定位。
    ///
    /// 返回的总在码点边界上，绝不会是码点内部。
    ///
    /// ⚠ 对字段内容是 O(n)，因为它重新测量字符串。单行字段就几十个字形，
    ///   而且只在点击时跑。不要每帧调用。
    size_t text_hit_index(FontId font, const char *utf8, f32 x);
    size_t text_hit_index_n(FontId font, const char *utf8, size_t len, f32 x);

    /// 把 [byte_lo, byte_hi) 内每个码点的水平跨度填进 `out`。返回写了多少
    /// （不会超过 `max`）。
    ///
    /// 存在的理由是让选区画在字形**下面**。选区必须在 shape_text() 之前推进，
    /// 而 shape_text 整形完才发命令——选区需要绘制的那一刻没有东西可读回。
    /// 这里一遍 O(n)，而不是对每个码点调 text_offset_x()（那样是 O(n^2)，
    /// 恰好在选区最常用的场景——选中一长行——变慢）。
    ///
    /// 跨度以逻辑单位返回，覆盖整行高，调用方不必知道基线。
    u32 text_span_rects(FontId font, const char *utf8, size_t byte_lo, size_t byte_hi, f32 pen_x,
                        f32 line_h, Rect *out, u32 max);
    /// 把 `utf8` 按 max_width 自动换行，逐行回调。返回行数。
    int wrap_text(FontId font, const char *utf8, f32 max_width,
                  int (*on_line)(const char *, void *), void *user);

} // namespace fox