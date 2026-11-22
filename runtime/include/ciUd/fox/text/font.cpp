#include "font.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "../../../gui/src/font_search_paths.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "detail/stbtt_impl.hpp"
#undef STB_TRUETYPE_IMPLEMENTATION
#include "detail/stbtt_impl.hpp"

namespace fox
{


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
            pos++;
            return 0xFFFD;
        }

        if (pos + (size_t)extra >= len)
        {
            pos = len;
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

        if (shelf_x_ + w > width_)
        {
            shelf_x_ = 0;
            shelf_y_ += shelf_h_ + 1;
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
        fallback_.empty = true;
        fallback_.m.advance = face_ ? face_->pixel_size * 0.5f : 8.0f;

        auto it = cache_.find(codepoint);
        if (it != cache_.end()) return it->second;

        ++generation_;

        if (!face_ || !face_->valid())
        {
            cache_.emplace(codepoint, fallback_);
            return fallback_;
        }

        stbtt_fontinfo *info = (stbtt_fontinfo *)face_->ttf;

        if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
            codepoint == '\r' || codepoint == 0x00A0 ||
            codepoint == 0x3000 )
        {
            int ws_adv = 0, ws_lsb = 0;
            stbtt_GetCodepointHMetrics(info, (int)codepoint, &ws_adv, &ws_lsb);
            AtlasGlyph w;
            w.empty = true;
            w.m.advance = (f32)ws_adv * face_->scale;
            w.m.width = w.m.height = 0.0f;
            auto [ins, _ok] = cache_.emplace(codepoint, w);
            return ins->second;
        }

        int adv_u = 0, lsb_u = 0;
        stbtt_GetCodepointHMetrics(info, (int)codepoint, &adv_u, &lsb_u);

        AtlasGlyph g;
        g.m.advance = (f32)adv_u * face_->scale;

        int gw = 0, gh = 0, gx = 0, gy = 0;
        unsigned char *bmp = stbtt_GetCodepointBitmap(info, face_->raster_scale,
                                                      face_->raster_scale, (int)codepoint,
                                                      &gw, &gh, &gx, &gy);
        if (!bmp)
        {
            g.empty = false;
            g.bitmap_left = 0.0f;
            g.bitmap_top = face_->pixel_size * 0.7f;
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
            g.empty = true;
        }

        const f32 inv_dpi = face_->dpi > 0.0f ? (1.0f / face_->dpi) : 1.0f;
        const f32 lgw = (f32)gw * inv_dpi;
        const f32 lgh = (f32)gh * inv_dpi;
        const f32 lgx = (f32)gx * inv_dpi;
        const f32 lgy = (f32)gy * inv_dpi;

        g.pixel_rect = Rect{0, 0, lgw, lgh};

        g.m.width = lgw;
        g.m.height = lgh;
        g.m.bearing_x = lgx;
        g.bitmap_left = lgx;
        g.bitmap_top = -lgy;
        g.m.bearing_y = g.bitmap_top;
        g.empty = (gw <= 0 || gh <= 0);

        stbtt_FreeBitmap(bmp, nullptr);

        auto [ins, _ok] = cache_.emplace(codepoint, g);
        return ins->second;
    }


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

        Entry e;
        e.face.data = *bytes;
        e.face.pixel_size = pixel_size;
        e.source = path;

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

        e.face.scale = stbtt_ScaleForPixelHeight(info, pixel_size);
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
        faces_.back().atlas.bind(&faces_.back().face);
        faces_.back().face.id = (FontId)faces_.size();
        return faces_.back().face.id;
    }

    FontId FontManager::load_system_ui(f32 pixel_size)
    {
        if (!primary_path_.empty())
        {
            const FontId id = load_file(primary_path_.c_str(), pixel_size);
            if (id != FONT_INVALID) return id;
        }

        static const char *candidates[] = {
            qgui::fontpath::NOTO_CJK_PKG_TTC,
            qgui::fontpath::NOTO_CJK_SC_OTF,
            qgui::fontpath::NOTO_CJK_TTC,
            qgui::fontpath::NOTO_CJK_FLAT_TTC,
            qgui::fontpath::WQY_MICROHEI,
            qgui::fontpath::DROID_FALLBACK_FULL,
            qgui::fontpath::ANDROID_NOTO_CJK,
            qgui::fontpath::ANDROID_ROBOTO,
            qgui::fontpath::ANDROID_DROID_FALLBACK,
            qgui::fontpath::WIN_MSYH,
            qgui::fontpath::WIN_SIMHEI,
            qgui::fontpath::WIN_SIMSUN,
            qgui::fontpath::WIN_SEGOEUI,
            qgui::fontpath::WIN_ARIAL,
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

        if (id == FONT_INVALID || (size_t)id > faces_.size()) return false;
        const FontFace &f = faces_[(size_t)id - 1].face;
        if (!f.valid()) return false;

        stbtt_fontinfo *info = (stbtt_fontinfo *)f.ttf;

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
        return primary;
    }

    void FontManager::load_fallback_faces(f32 pixel_size)
    {
        static const char *candidates[] = {
            qgui::fontpath::WIN_MSYH,
            qgui::fontpath::WIN_SIMSUN,
            qgui::fontpath::WIN_SEGUIEE,
            qgui::fontpath::WIN_SEGUISYM,
            qgui::fontpath::NOTO_CJK_PKG_TTC,
            qgui::fontpath::NOTO_CJK_SC_OTF,
            qgui::fontpath::NOTO_SANS_REGULAR,
            qgui::fontpath::LIBERATION_SANS,
            qgui::fontpath::DROID_FALLBACK_FULL,
            qgui::fontpath::WQY_MICROHEI,
            qgui::fontpath::ANDROID_NOTO_CJK,
            qgui::fontpath::ANDROID_DROID_FALLBACK,
            qgui::fontpath::MAC_PINGFANG,
            qgui::fontpath::MAC_APPLE_SYMBOLS,
        };

        for (const char *p : candidates)
        {
            const FontId id = load_file(p, pixel_size);
            if (id == FONT_INVALID) continue;

            bool dup = false;
            for (FontId f : fallbacks_)
                if (f == id) { dup = true; break; }
            if (dup) continue;

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
        u32 g = 0;
        for (const Entry &e : faces_) g += e.atlas.generation();
        return g;
    }

    f32 FontManager::measure(FontId id, const char *utf8)
    {
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
            if (cp == '\n') break;

            const f32 adv = a->glyph((u32)cp).m.advance;
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
            if (cp == '\n') break;

            const f32 adv = a->glyph((u32)cp).m.advance;
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

            const FontId gf = mgr.resolve(font, (u32)cp);
            GlyphAtlas *ga = gf == FONT_INVALID ? nullptr : mgr.atlas(gf);
            if (!ga) break;
            const AtlasGlyph &g = ga->glyph((u32)cp);

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
                if (last_space != (size_t)-1 && last_space > tok_start)
                {
                    on_line(utf8 + last_space + 1, user);
                    ++lines;
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

}