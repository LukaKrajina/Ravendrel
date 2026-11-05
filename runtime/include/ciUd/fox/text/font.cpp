// Fox text/font.cpp - 字体加载、按需字形栅格化、整形
//
// font.hpp 的实现。stb_truetype API 会静默给出"看着合理但错误"的数字的
// 每一处都做了标注；完整清单见 font.hpp。

#include "font.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// 字体绝对路径的单一知识来源（与 qgui 的 font_loader.cpp 共用；顺序各自独立）。
// 相对本文件（ciUd/fox/text/）上行三层到 include/，再进 gui/src/。
#include "../../../gui/src/font_search_paths.hpp"

// 两遍包含：先声明后实现。detail 头文件没有 include guard，正是为了让这样
// 可行（见 detail/stbtt_impl.hpp）。
#define STB_TRUETYPE_IMPLEMENTATION
#include "detail/stbtt_impl.hpp"
#undef STB_TRUETYPE_IMPLEMENTATION
#include "detail/stbtt_impl.hpp"

namespace fox
{

    // ── UTF-8 ──────────────────────────────────────────────────────────

    i32 utf8_next(const char *s, size_t len, size_t &pos)
    {
        if (!s || pos >= len) return -1;
        unsigned char c = (unsigned char)s[pos];
        i32 cp;
        int extra;
        if (c < 0x80)
        {
            cp = c;
            extra = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F;
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F;
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07;
            extra = 3;
        }
        else
        {
            pos++; // 游离的续字节
            return 0xFFFD;
        }

        if (pos + (size_t)extra >= len)
        {
            pos = len; // 字符串末尾的截断序列
            return 0xFFFD;
        }
        for (int i = 1; i <= extra; ++i)
        {
            unsigned char cc = (unsigned char)s[pos + i];
            if ((cc & 0xC0) != 0x80)
            {
                pos++;
                return 0xFFFD;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        pos += extra + 1;
        return cp;
    }

    size_t utf8_len(const char *s, size_t len)
    {
        size_t pos = 0, n = 0;
        while (utf8_next(s, len, pos) >= 0) ++n;
        return n;
    }

    // ── 字形图集 ────────────────────────────────────────────────────

    void GlyphAtlas::init(int width, int height)
    {
        width_ = width;
        height_ = height;
        pixels_.assign((size_t)width * height, 0);
        cache_.clear();
        shelf_x_ = shelf_y_ = shelf_h_ = 0;
        full_ = false;
        dirty_ = false;
    }

    void GlyphAtlas::shutdown()
    {
        pixels_.clear();
        pixels_.shrink_to_fit();
        cache_.clear();
        width_ = height_ = 0;
    }

    f32 GlyphAtlas::occupancy() const
    {
        if (width_ <= 0 || height_ <= 0) return 0.0f;
        return (f32)shelf_y_ / (f32)height_;
    }

    bool GlyphAtlas::alloc(int w, int h, int &out_x, int &out_y)
    {
        if (full_ || w <= 0 || h <= 0) return false;
        if (w > width_ || h > height_) { full_ = true; return false; }

        // Shelf 装箱器：一行填到装不下为止，再在下面开新行。文字是随时间
        // 生成的，行内访问大体顺序，对这种负载接近最优。
        if (shelf_x_ + w > width_)
        {
            shelf_x_ = 0;
            shelf_y_ += shelf_h_ + 1; // 留 1px 缝隙防双线性渗色
            shelf_h_ = 0;
        }
        if (shelf_y_ + h > height_) { full_ = true; return false; }

        out_x = shelf_x_;
        out_y = shelf_y_;
        shelf_x_ += w + 1;
        if (h > shelf_h_) shelf_h_ = h;
        return true;
    }

    const AtlasGlyph &GlyphAtlas::glyph(u32 codepoint)
    {
        // ⚠ fallback 必须是成员，不能是函数内 static。
        //   它按引用返回，用 static 的话所有调用者拿到同一个对象：字体里缺
        //   失的两个不同码点会混用一个条目，通过一个引用写入会破坏另一个。
        //   共享 static 被逐次修改，正是那类表现为"文字在不相关的标签里
        //   随机变化"的 bug。
        fallback_.empty = true;
        fallback_.m.advance = face_ ? face_->pixel_size * 0.5f : 8.0f;

        auto it = cache_.find(codepoint);
        if (it != cache_.end()) return it->second;

        // 过了这里必然插入一个条目（真字形、空白度量或豆腐块兜底），
        // 所以只有这一处需要递增代际号。
        ++generation_;

        if (!face_ || !face_->valid())
        {
            cache_.emplace(codepoint, fallback_);
            return fallback_;
        }

        stbtt_fontinfo *info = (stbtt_fontinfo *)face_->ttf;

        // 空白字符没有轮廓，stbtt_GetCodepointBitmap 对它返回 null——和
        // "这个码点根本不在字体里"的返回值一样。下面的豆腐块路径分不清
        // 两者，空格会被栅格化成基线上方一个看得见的空心框，UI 里每个
        // 空格都露出来。
        //
        // 先消除歧义：空白只推进笔位、什么都不画；只有真正缺失的码点才
        // 配得到豆腐块。
        if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
            codepoint == '\r' || codepoint == 0x00A0 /* NBSP */ ||
            codepoint == 0x3000 /* ideographic space */)
        {
            int ws_adv = 0, ws_lsb = 0;
            stbtt_GetCodepointHMetrics(info, (int)codepoint, &ws_adv, &ws_lsb);
            AtlasGlyph w;
            w.empty = true; // 没有可画的东西
            w.m.advance = (f32)ws_adv * face_->scale;
            w.m.width = w.m.height = 0.0f;
            auto [ins, _ok] = cache_.emplace(codepoint, w);
            return ins->second;
        }

        // 陷阱 3：(info, codepoint, &advance, &leftSideBearing)——顺序就是
        // 这样。
        int adv_u = 0, lsb_u = 0;
        stbtt_GetCodepointHMetrics(info, (int)codepoint, &adv_u, &lsb_u);

        AtlasGlyph g;
        // 陷阱 2：hmetrics 是字体单位。乘 scale 才是像素。
        g.m.advance = (f32)adv_u * face_->scale;

        int gw = 0, gh = 0, gx = 0, gy = 0;
        // 陷阱 4：显式传缩放。face 里烘焙的缩放这个调用不认——传 1 会按
        // 字体单位栅格化。
        //
        // 用 `raster_scale` 不是 `scale`：图集存物理像素，HiDPI 屏上字形才
        // 清晰。用 `scale` 会在每块 2x 屏上给出尺寸正确但被放大（即发虚）
        // 的字形。
        unsigned char *bmp = stbtt_GetCodepointBitmap(info, face_->raster_scale,
                                                      face_->raster_scale, (int)codepoint,
                                                      &gw, &gh, &gx, &gy);
        if (!bmp)
        {
            // 字体里没有这个码点（例如拉丁 face 里的 emoji）。显示看得见的
            // 豆腐块，而不是悄悄什么都不画。
            // ⚠ 空白字符在这里也返回 null——它在上面已处理，否则 UI 里每个
            //   空格都会渲染成空心框。
            g.empty = false;
            g.bitmap_left = 0.0f;
            // 与栅格化路径同一"向上为正"约定：框顶在基线上方约 cap 高度处，
            // 这样一排豆腐块与真实文字对齐，而不是浮在上面。
            g.bitmap_top = face_->pixel_size * 0.7f;
            // 落到下面打包一个描边框。图集单元是物理像素（它是真实纹理
            // 数据），而报出的度量保持逻辑单位，与下面的栅格化路径一致。
            const f32 dpi = face_->dpi > 0.0f ? face_->dpi : 1.0f;
            const f32 lw = face_->pixel_size * 0.6f;
            const f32 lh = face_->pixel_size * 0.7f;
            int tw = (int)(lw * dpi);
            int th = (int)(lh * dpi);
            int tx = 0, ty = 0;
            g.m.width = lw;
            g.m.height = face_->pixel_size;
            if (alloc(tw, th, tx, ty))
            {
                for (int yy = 0; yy < th; ++yy)
                    for (int xx = 0; xx < tw; ++xx)
                    {
                        bool edge = (xx == 0 || yy == 0 || xx == tw - 1 || yy == th - 1);
                        pixels_[(size_t)(ty + yy) * width_ + (tx + xx)] = edge ? 0xC0 : 0x30;
                    }
                g.uv = Rect{(f32)tx / width_, (f32)ty / height_, (f32)tw / width_,
                            (f32)th / height_};
                g.pixel_rect = Rect{0, 0, lw, lh};
                dirty_ = true;
            }
            auto [ins, _ok] = cache_.emplace(codepoint, g);
            return ins->second;
        }

        // 把位图拷进图集（stbtt 给的是归我们所有的临时缓冲）。
        int ax = 0, ay = 0;
        if (gw > 0 && gh > 0 && alloc(gw, gh, ax, ay))
        {
            for (int y = 0; y < gh; ++y)
            {
                memcpy(&pixels_[(size_t)(ay + y) * width_ + ax], bmp + (size_t)y * gw, (size_t)gw);
            }
            g.uv = Rect{(f32)ax / width_, (f32)ay / height_, (f32)gw / width_, (f32)gh / height_};
            dirty_ = true;
        }
        else
        {
            // 图集满了：保留 advance（布局仍然正确）但什么都不画。
            // 缺字形好过画坏的字形。
            g.empty = true;
        }

        // 把物理位图尺寸换回逻辑单位，因为 Fox 其余部分在逻辑空间布局。
        //
        // 图集单元本身保持物理（上面 alloc/pack 用的是原始 gw/gh），所以
        // 字形纹理按设备分辨率存储，build_instances 把逻辑目标矩形按同一
        // 倍数放大——纹理到屏幕 1:1，没有重采样。
        //
        // 这里弄错是静默的：字形形状对、颜色对，只是大小不对，看起来像
        // 字体 bug。
        const f32 inv_dpi = face_->dpi > 0.0f ? (1.0f / face_->dpi) : 1.0f;
        const f32 lgw = (f32)gw * inv_dpi;
        const f32 lgh = (f32)gh * inv_dpi;
        const f32 lgx = (f32)gx * inv_dpi;
        const f32 lgy = (f32)gy * inv_dpi;

        // ⚠ pixel_rect 是目标尺寸，shape_text() 原样使用。把它留成物理像素
        //   而 advance 保持逻辑单位，会让每个字形四边形是笔位给它那格的
        //   两倍宽，HiDPI 屏上字母叠成看不清的一团。上面的 uv 仍是物理
        //   单位——它寻址纹素。
        g.pixel_rect = Rect{0, 0, lgw, lgh};

        g.m.width = lgw;
        g.m.height = lgh;
        g.m.bearing_x = lgx;
        // stb 的 `yoff` 是从字形原点（基线）到位图顶部的偏移，在屏幕空间
        // 量——即基线以上为负。Fox 的 `bitmap_top` 在 font.hpp 里定义为
        // "从基线向上到顶部的距离"，所以转换就是取一次符号。
        //
        // 这里曾经写的是 `ascent + lgy`，两处都错：ascent 不该出现在相对
        // 基线的量里，而且符号是反的。结果是文字落在自己基线下方 4-7 px，
        // 每个字形按彼此的高度差错位（'l' 在基线下 6.6 px，'o' 下 2.6 px）
        // ——看起来像"字体坏了"，而不是"一个度量算错了"。它活这么久是因为
        // 每个字形都错，局部看不出来任何不一致：只有基线是错的。
        g.bitmap_left = lgx;
        g.bitmap_top = -lgy;
        g.m.bearing_y = g.bitmap_top;
        g.empty = (gw <= 0 || gh <= 0);

        stbtt_FreeBitmap(bmp, nullptr);

        auto [ins, _ok] = cache_.emplace(codepoint, g);
        return ins->second;
    }

    // ── 字体管理器 ───────────────────────────────────────────────────

    FontManager &fonts()
    {
        static FontManager mgr;
        return mgr;
    }

    FontFace *FontManager::face(FontId id)
    {
        if (id == FONT_INVALID || id > faces_.size()) return nullptr;
        return &faces_[id - 1].face;
    }

    std::string FontManager::source_of(FontId id) const
    {
        if (id == FONT_INVALID || id > faces_.size()) return std::string("<invalid>");
        return faces_[id - 1].source;
    }

    GlyphAtlas *FontManager::atlas(FontId id)
    {
        if (id == FONT_INVALID || id > faces_.size()) return nullptr;
        return &faces_[id - 1].atlas;
    }

    FontId FontManager::load_file(const char *path, f32 pixel_size, int face_index)
    {
        if (!path) return FONT_INVALID;

        // 缓存文件字节。没有它，每种字号都要重读整个文件——本项目常用的
        // CJK 字体每字号约 19 MB。一个窗口用五种字号就要从磁盘拉约 95 MB，
        // 内存里还存五份相同副本。
        //
        // 按 (path, face 下标) 做键，因为 .ttc 装着多个 face，各自需要
        // 自己的字节区间副本。
        const std::vector<u8> *bytes = nullptr;
        for (const auto &c : file_cache_)
        {
            if (c.path == path && c.face_index == face_index)
            {
                bytes = &c.data;
                break;
            }
        }

        if (!bytes)
        {
            FILE *f = fopen(path, "rb");
            if (!f) return FONT_INVALID;
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len <= 0)
            {
                fclose(f);
                return FONT_INVALID;
            }
            FileCacheEntry fc;
            fc.path = path;
            fc.face_index = face_index;
            fc.data.resize((size_t)len);
            size_t got = fread(fc.data.data(), 1, (size_t)len, f);
            fclose(f);
            if (got != (size_t)len) return FONT_INVALID;
            file_cache_.push_back(std::move(fc));
            bytes = &file_cache_.back().data;
        }

        // 先在一边把 Entry 建好。完全构造好才移进 faces_，中途失败不会留下
        // 半初始化的 Entry（也不会泄漏 stbtt_fontinfo）。
        Entry e;
        e.face.data = *bytes;
        e.face.pixel_size = pixel_size;
        e.source = path;

        // 陷阱 1：InitFont 要字节偏移，由 face 下标推出。
        // （stbtt_PackFontRanges 要的是下标本身——这里没用到。）
        e.face.face_offset = stbtt_GetFontOffsetForIndex(e.face.data.data(), face_index);
        if (e.face.face_offset < 0) return FONT_INVALID;

        auto *info = (stbtt_fontinfo *)malloc(sizeof(stbtt_fontinfo));
        if (!info) return FONT_INVALID;
        if (stbtt_InitFont(info, e.face.data.data(), e.face.face_offset) == 0)
        {
            free(info);
            return FONT_INVALID;
        }
        e.face.ttf = info;

        // 陷阱 2：下面的度量在乘 scale 之前都是字体单位。`scale` 是每字体
        // 单位多少逻辑像素，所以由它推出的度量都是逻辑单位，布局与 DPI
        // 无关。
        e.face.scale = stbtt_ScaleForPixelHeight(info, pixel_size);
        // `raster_scale` 是每字体单位多少物理像素：图集按设备分辨率存储，
        // HiDPI 屏上字形才清晰。
        e.face.dpi = dpi_;
        e.face.raster_scale = e.face.scale * dpi_;
        if (e.face.scale <= 0.0f)
        {
            free(info);
            e.face.ttf = nullptr;
            return FONT_INVALID;
        }

        int asc_u = 0, desc_u = 0, gap_u = 0;
        stbtt_GetFontVMetrics(info, &asc_u, &desc_u, &gap_u);
        e.face.ascent = (f32)asc_u * e.face.scale;
        e.face.descent = (f32)desc_u * e.face.scale;
        e.face.line_height = (f32)(asc_u - desc_u + gap_u) * e.face.scale;
        if (e.face.line_height <= 0.0f) e.face.line_height = pixel_size * 1.2f;

        e.atlas.init(FOX_GLYPH_ATLAS_SIZE, FOX_GLYPH_ATLAS_SIZE);

        faces_.push_back(std::move(e));
        // 插入之后再绑定：bind() 存的是 &faces_.back().face。deque 的地址
        // 稳定，但在移动前绑定会捕获局部变量的地址，而不是最终归属。
        faces_.back().atlas.bind(&faces_.back().face);
        faces_.back().face.id = (FontId)faces_.size();
        return faces_.back().face.id;
    }

    FontId FontManager::load_system_ui(f32 pixel_size)
    {
        // 显式指定的主字体直接胜出。加载失败时不回退到平台搜索：点了名要
        // 某字体的调用方想听到"没加载成"，而不是悄悄拿到另一个。
        if (!primary_path_.empty())
        {
            const FontId id = load_file(primary_path_.c_str(), pixel_size);
            if (id != FONT_INVALID) return id;
        }

        // 优先选覆盖 CJK 的 face：一个顶拉丁 + CJK 两类。
        //
        // ⚠ 正是这个偏好让回退链默认从没被测过——多数机器上主字体覆盖一切，
        //   resolve() 永远走不到回退，"它能用"是没验证过的说法。
        //   set_primary_path() 就是为了打破这个僵局。
        // 路径字符串的单一来源是 gui/src/font_search_paths.hpp；此处的顺序是
        // fox 引擎自己的优先级（与 qgui 的 font_loader.cpp 那份不同，勿合并）。
        static const char *candidates[] = {
            // Linux
            qgui::fontpath::NOTO_CJK_PKG_TTC,
            qgui::fontpath::NOTO_CJK_SC_OTF,
            qgui::fontpath::NOTO_CJK_TTC,
            qgui::fontpath::NOTO_CJK_FLAT_TTC,
            qgui::fontpath::WQY_MICROHEI,
            qgui::fontpath::DROID_FALLBACK_FULL,
            // Android
            qgui::fontpath::ANDROID_NOTO_CJK,
            qgui::fontpath::ANDROID_ROBOTO,
            qgui::fontpath::ANDROID_DROID_FALLBACK,
            // Windows
            qgui::fontpath::WIN_MSYH,
            qgui::fontpath::WIN_SIMHEI,
            qgui::fontpath::WIN_SIMSUN,
            qgui::fontpath::WIN_SEGOEUI,
            qgui::fontpath::WIN_ARIAL,
            // macOS
            qgui::fontpath::MAC_PINGFANG,
            qgui::fontpath::MAC_HELVETICA,
            qgui::fontpath::MAC_ARIAL_UNICODE,
        };
        for (const char *p : candidates)
        {
            FontId id = load_file(p, pixel_size);
            if (id != FONT_INVALID) return id;
        }
        return FONT_INVALID;
    }

    bool FontManager::has_glyph(FontId id, u32 codepoint) const
    {
        if (codepoint > 0x10FFFFu) return false;

        // 按下标查找而不通过 faces_，这样本函数保持 const。
        // FontId -> deque 下标是恒等映射（id 从头分配、从不复用），
        // atlas()/face() 已经依赖这一点。
        if (id == FONT_INVALID || (size_t)id > faces_.size()) return false;
        const FontFace &f = faces_[(size_t)id - 1].face;
        if (!f.valid()) return false;

        stbtt_fontinfo *info = (stbtt_fontinfo *)f.ttf;

        // 字形下标 0 按定义就是 .notdef——字体必须为它没有的任何码点画的
        // 那个字形。所以 `index != 0` 正是存在性判定，不是近似。
        //
        // （本注释早前的草稿说反了，还想找 stbtt_FindGlyphIndex 之外的
        // stbtt_FindGlyphInfo 当"更可靠"的信号。带进来的 stb_truetype 里
        // 没有那个函数，推理也是错的：真在下标 0 放了字形的字体同样无法
        // 把它与 .notdef 区分，所以没有字体会把真实内容放在那里。）
        return stbtt_FindGlyphIndex(info, (int)codepoint) != 0;
    }

    FontId FontManager::resolve(FontId primary, u32 codepoint) const
    {
        if (primary == FONT_INVALID) return FONT_INVALID;
        if (has_glyph(primary, codepoint)) return primary;

        for (FontId f : fallbacks_)
        {
            if (f == FONT_INVALID || f == primary) continue;
            if (has_glyph(f, codepoint)) return f;
        }
        // 谁都没有。仍返回主字体，让豆腐块路径兜底，缺口看得见。
        return primary;
    }

    void FontManager::load_fallback_faces(f32 pixel_size)
    {
        // 只放回退 face。主 UI face 刻意不在此列：放进来会让 resolve() 对
        // 调用方已确认它没有的码点也返回它，应用里每个 CJK 字符都白查一次。
        //
        // 按覆盖面宽度排序，不按喜好。其中好几个很大；加载一个约 19 MB
        // 文件字节（各字号共享，见 load_file 的缓存）外加一个图集。
        // 路径字符串的单一来源是 gui/src/font_search_paths.hpp（见该文件头说明）。
        static const char *candidates[] = {
            // Windows
            qgui::fontpath::WIN_MSYH,   // 微软雅黑：CJK + 拉丁
            qgui::fontpath::WIN_SIMSUN, // 宋体
            qgui::fontpath::WIN_SEGUIEE,
            qgui::fontpath::WIN_SEGUISYM, // 符号
            // Linux
            qgui::fontpath::NOTO_CJK_PKG_TTC,
            qgui::fontpath::NOTO_CJK_SC_OTF,
            qgui::fontpath::NOTO_SANS_REGULAR,
            qgui::fontpath::LIBERATION_SANS,
            qgui::fontpath::DROID_FALLBACK_FULL,
            qgui::fontpath::WQY_MICROHEI,
            // Android
            qgui::fontpath::ANDROID_NOTO_CJK,
            qgui::fontpath::ANDROID_DROID_FALLBACK,
            // macOS
            qgui::fontpath::MAC_PINGFANG,
            qgui::fontpath::MAC_APPLE_SYMBOLS,
        };

        for (const char *p : candidates)
        {
            const FontId id = load_file(p, pixel_size);
            if (id == FONT_INVALID) continue;

            // 跳过链里已有的 face。主字体是 msyh.ttc 的机器上，第一个候选
            // 就是同一文件——再加载一次等于多一个带自己图集的 Entry，为
            // resolve() 反正会跳过的 face 重复占内存。
            bool dup = false;
            for (FontId f : fallbacks_)
                if (f == id) { dup = true; break; }
            if (dup) continue;

            // 追加而不是前插：先加载的 face 不能被现在才加载的挤到后面。
            fallbacks_.push_back(id);
        }
    }

    const char *FontManager::face_source(FontId id) const
    {
        if (id == FONT_INVALID || (size_t)id > faces_.size()) return "";
        return faces_[(size_t)id - 1].source.c_str();
    }

    void FontManager::shutdown()
    {
        for (auto &e : faces_)
        {
            // 按这个顺序释放：图集持有像素缓冲，`ttf` 是 malloc 出来的
            // stbtt_fontinfo。不清这些就清 deque，会让每个 face 泄漏一个
            // fontinfo 和一个图集直到进程结束——本函数就是来防这个的。
            if (e.face.ttf) free(e.face.ttf);
            e.atlas.shutdown();
        }
        faces_.clear();
        fallbacks_.clear();
        file_cache_.clear();
    }

    f32 FontManager::line_height(FontId id)
    {
        FontFace *f = face(id);
        return f ? f->line_height : 0.0f;
    }

    u32 FontManager::atlas_generation() const
    {
        // 求和而不是异或合并：两个 face 的计数器相互独立，求和在任一个
        // 变化时都会变。碰撞在这里无所谓——这个值唯一的职责是在任何图集
        // 变过的帧之间取不同的值。
        u32 g = 0;
        for (const Entry &e : faces_) g += e.atlas.generation();
        return g;
    }

    f32 FontManager::measure(FontId id, const char *utf8)
    {
        // 它从一开始就声明在头文件里，直到字体回退需要它才有了定义——
        // 这正是它没被发现的原因：未定义的成员函数在有人引用之前不算错，
        // 声明就一直待在那里看着挺好，什么也没量过。
        //
        // 它走与 shape_text() 相同的解析链。用主 face 量、用回退 face 画，
        // 会让标签报出错误的宽度，而症状——文字装不进自己的矩形——会指向
        // 布局而不是测量。
        FontManager &mgr = fonts();
        GlyphAtlas *a = atlas(id);
        if (!a || !utf8) return 0.0f;

        const size_t len = strlen(utf8);
        size_t pos = 0;
        f32 w = 0.0f;
        while (pos < len)
        {
            const i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;
            const FontId gf = mgr.resolve(id, (u32)cp);
            GlyphAtlas *ga = gf == FONT_INVALID ? nullptr : atlas(gf);
            if (!ga) break;
            w += ga->glyph((u32)cp).m.advance;
        }
        return w;
    }

    const AtlasGlyph &FontManager::glyph_at(FontId id, u32 codepoint)
    {
        GlyphAtlas *a = atlas(id);
        if (!a)
        {
            static AtlasGlyph empty;
            return empty;
        }
        return a->glyph(codepoint);
    }

    // ── 整形 / 测量 ──────────────────────────────────────────────────

    f32 text_width_n(FontId font, const char *utf8, size_t len)
    {
        FontManager &mgr = fonts();
        GlyphAtlas *a = mgr.atlas(font);
        if (!a || !utf8) return 0.0f;

        size_t pos = 0;
        f32 w = 0.0f;
        while (pos < len)
        {
            i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;
            const AtlasGlyph &g = a->glyph((u32)cp);
            w += g.m.advance;
        }
        return w;
    }

    f32 text_offset_x_n(FontId font, const char *utf8, size_t len, size_t byte_offset)
    {
        FontManager &mgr = fonts();
        GlyphAtlas *a = mgr.atlas(font);
        if (!a || !utf8) return 0.0f;

        if (byte_offset > len) byte_offset = len;

        size_t pos = 0;
        f32 x = 0.0f;
        while (pos < byte_offset)
        {
            const i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;
            // 每个码点累加一次推进，按解码出的码点查字形。两个细节都要紧：
            // 按字节累加看起来像"字体偏窄"而不是 bug，用原始首字节索引
            // 图集会顺带把垃圾字形缓存进去。
            x += a->glyph((u32)cp).m.advance;
        }
        return x;
    }

    size_t text_hit_index_n(FontId font, const char *utf8, size_t len, f32 x)
    {
        FontManager &mgr = fonts();
        GlyphAtlas *a = mgr.atlas(font);
        if (!a || !utf8) return 0;

        size_t pos = 0;
        f32 pen = 0.0f;
        size_t last_boundary = 0;

        while (pos < len)
        {
            const size_t here = pos;
            const i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;
            if (cp == '\n') break; // 单行：到换行为止

            const f32 adv = a->glyph((u32)cp).m.advance;
            // x 落在这个字形的左半边就算命中，否则贴到它的起点。按中点
            // 切分，点在字符边界附近时才会感觉选中了更近的那个。
            if (x < pen + adv * 0.5f) return here;
            pen += adv;
            last_boundary = pos;
        }
        return last_boundary;
    }

    u32 text_span_rects_n(FontId font, const char *utf8, size_t len, size_t byte_lo,
                            size_t byte_hi, f32 pen_x, f32 line_h, Rect *out, u32 max)
    {
        FontManager &mgr = fonts();
        GlyphAtlas *a = mgr.atlas(font);
        if (!a || !utf8 || byte_hi <= byte_lo || max == 0) return 0;

        if (byte_lo > len) byte_lo = len;
        if (byte_hi > len) byte_hi = len;

        size_t pos = 0;
        f32 x = pen_x;
        u32 n = 0;

        while (pos < len)
        {
            const size_t here = pos;
            const i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;
            if (cp == '\n') break; // 只算单行跨度

            const f32 adv = a->glyph((u32)cp).m.advance;
            // 空白也算在内：选中 "a b" 必须是一条连续色带，跳过空格会在
            // 高亮中间留出可见的缺口。
            if (pos > byte_lo && here < byte_hi && n < max)
                out[n++] = Rect{x, 0.0f, adv, line_h};
            x += adv;
            if (pos >= byte_hi) break;
        }
        return n;
    }

    u32 shape_text_n(DisplayList &dl, FontId font, const char *utf8, size_t len, const Vec2 &pen,
                     Color color, f32 max_width, u32 &out_glyph_start)
    {
        FontManager &mgr = fonts();
        GlyphAtlas *a = mgr.atlas(font);
        if (!a || !utf8)
        {
            out_glyph_start = dl.glyph_count();
            return 0;
        }

        out_glyph_start = dl.glyph_count();


        size_t pos = 0;
        f32 x = pen.x;
        u32 n = 0;

        // 字体回退需要不止一个 Text 命令：一段混排拉丁与 CJK 的文字会解析到
        // 多个 face，每个 face 有自己的字形图集和自己的度量。
        //
        // 好消息是下游不需要新机制。Cmd 本来就带 (font, glyph_start,
        // glyph_count)，build_instances 按 (font, tex) 对连续段分批——所以
        // 每段 face 发一个 Text 命令，现有批处理器就会在恰好的位置切帧，
        // 每段绑上正确的图集。
        //
        // 这也是为什么分批按"段"而不是"先所有图形再所有文字"：否则混排
        // 文字根本没法工作。
        FontId run_font = FONT_INVALID;
        u32 run_glyph_start = out_glyph_start;
        u32 run_glyph_count = 0;
        f32 run_x0 = pen.x;

        auto flush_run = [&]() {
            if (run_glyph_count == 0 || run_font == FONT_INVALID) return;
            f32 line_h = mgr.line_height(run_font);
            f32 asc = mgr.face(run_font) ? mgr.face(run_font)->ascent : 0.0f;
            dl.push_text(Rect{run_x0, pen.y - asc, x - run_x0, line_h}, color, run_font,
                         run_glyph_start, run_glyph_count, 0.0f);
            run_glyph_count = 0;
            run_font = FONT_INVALID;
        };

        while (pos < len)
        {
            const size_t glyph_start_here = dl.glyph_count();
            i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;
            if (cp == '\n') continue;

            // 先解析出该由哪个 face 画这个码点，再到那个 face 的图集里查
            // 字形。先在主字体里查、事后再问回退是反的：那时豆腐块已经
            // 栅格化出来了。
            const FontId gf = mgr.resolve(font, (u32)cp);
            GlyphAtlas *ga = gf == FONT_INVALID ? nullptr : mgr.atlas(gf);
            // 解析出的 face 没有图集意味着字体子系统坏了（face 加载了却没
            // 图集，或 init 失败）。停下来而不是猜 advance：猜的宽度会让
            // 光标、选区带和字形互相失步，三者全错，看起来还像布局 bug。
            if (!ga) break;
            const AtlasGlyph &g = ga->glyph((u32)cp);

            // max_width <= 0 表示"不限宽"。
            if (max_width > 0.0f && x - pen.x + g.m.advance > max_width) break;

            if (gf != run_font)
            {
                flush_run();
                run_font = gf;
                run_glyph_start = glyph_start_here;
                run_x0 = x;
            }

            if (!g.empty && g.pixel_rect.w > 0.0f && g.pixel_rect.h > 0.0f)
            {
                GlyphQuad q;
                q.dst = Rect{x + g.bitmap_left, pen.y - g.bitmap_top, g.pixel_rect.w,
                             g.pixel_rect.h};
                q.uv0 = Vec2{g.uv.x, g.uv.y};
                q.uv1 = Vec2{g.uv.x + g.uv.w, g.uv.y + g.uv.h};
                q.color = color;
                dl.add_glyph(q);
                ++n;
                ++run_glyph_count;
            }
            x += g.m.advance;
        }

        flush_run();
        return n;
    }

    // ── NUL 结尾的封装 ───────────────────────────────────────────────
    //
    // 薄封装，而且刻意如此：带长度的形式是排版数学的唯一实现。再抄一遍
    // 遍历，就多一处两者对"字形放哪"说法不一的地方。
    f32 text_width(FontId font, const char *utf8)
    {
        return utf8 ? text_width_n(font, utf8, strlen(utf8)) : 0.0f;
    }

    f32 text_offset_x(FontId font, const char *utf8, size_t byte_offset)
    {
        return utf8 ? text_offset_x_n(font, utf8, strlen(utf8), byte_offset) : 0.0f;
    }

    size_t text_hit_index(FontId font, const char *utf8, f32 x)
    {
        return utf8 ? text_hit_index_n(font, utf8, strlen(utf8), x) : 0;
    }

    u32 text_span_rects(FontId font, const char *utf8, size_t byte_lo, size_t byte_hi, f32 pen_x,
                        f32 line_h, Rect *out, u32 max)
    {
        return utf8 ? text_span_rects_n(font, utf8, strlen(utf8), byte_lo, byte_hi, pen_x, line_h,
                                        out, max)
                    : 0;
    }

    u32 shape_text(DisplayList &dl, FontId font, const char *utf8, const Vec2 &pen, Color color,
                   f32 max_width, u32 &out_glyph_start)
    {
        return utf8 ? shape_text_n(dl, font, utf8, strlen(utf8), pen, color, max_width,
                                   out_glyph_start)
                    : 0;
    }

    int wrap_text(FontId font, const char *utf8, f32 max_width, int (*on_line)(const char *, void *),
                  void *user)
    {
        if (!utf8 || !on_line) return 0;
        FontManager &mgr = fonts();
        GlyphAtlas *a = mgr.atlas(font);
        if (!a) return 0;

        int lines = 0;

        const size_t len = strlen(utf8);
        size_t pos = 0;
        f32 line_w = 0.0f;
        size_t last_space = (size_t)-1;

        while (pos < len)
        {
            size_t tok_start = pos;
            i32 cp = utf8_next(utf8, len, pos);
            if (cp < 0) break;

            if (cp == '\n')
            {
                on_line(utf8 + tok_start, user);
                ++lines;
                line_w = 0.0f;
                last_space = (size_t)-1;
                continue;
            }

            const AtlasGlyph &g = a->glyph((u32)cp);
            f32 adv = g.m.advance;

            if (max_width > 0.0f && line_w + adv > max_width)
            {
                // 优先在最后一个空格处断行（按词换行），如果有的话。
                if (last_space != (size_t)-1 && last_space > tok_start)
                {
                    on_line(utf8 + last_space + 1, user);
                    ++lines;
                    // 重新测量推回到断点之后的那段。
                    f32 w2 = 0.0f;
                    for (size_t k = last_space + 1; k < pos; ++k)
                        w2 += a->glyph((u8)utf8[k]).m.advance;
                    line_w = w2;
                }
                else
                {
                    on_line(utf8 + tok_start, user);
                    ++lines;
                    line_w = 0.0f;
                }
                last_space = (size_t)-1;
            }
            line_w += adv;
            if (cp == ' ') last_space = pos - 1;
        }

        if (pos > 0)
        {
            on_line(utf8, user);
            ++lines;
        }
        return lines;
    }

} // namespace fox
