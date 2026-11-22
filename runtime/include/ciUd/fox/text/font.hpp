#pragma once

#include "core/types.hpp"
#include "draw/display_list.hpp"
#include "draw/font_ids.hpp"
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>

namespace fox
{

    inline constexpr int FOX_GLYPH_ATLAS_SIZE = 1024;


    struct GlyphMetrics
    {
        f32 advance = 0.0f;
        f32 bearing_x = 0.0f;
        f32 bearing_y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
    };


    struct FontFace
    {
        FontId id = FONT_INVALID;
        void *ttf = nullptr;
        std::vector<u8> data;
        int face_offset = 0;

        f32 pixel_size = 0.0f;
        f32 scale = 0.0f;
        f32 raster_scale = 0.0f;
        f32 dpi = 1.0f;
        f32 ascent = 0.0f;
        f32 descent = 0.0f;
        f32 line_height = 0.0f;

        bool valid() const { return ttf != nullptr && scale > 0.0f; }
    };


    struct AtlasGlyph
    {
        Rect uv;
        Rect pixel_rect;
        GlyphMetrics m;
        f32 bitmap_left = 0.0f;
        f32 bitmap_top = 0.0f;
        bool empty = true;
    };

    class GlyphAtlas
    {
    public:
        void init(int width, int height);
        void shutdown();

        void bind(FontFace *f)
        {
            face_ = f;
            generation_ = f ? (u32)f->id * 100000u : 0u;
            dirty_ = true;
        }

        u32 generation() const { return generation_; }

        const AtlasGlyph &glyph(u32 codepoint);

        int width() const { return width_; }
        int height() const { return height_; }
        const std::vector<u8> &pixels() const { return pixels_; }
        bool dirty() const { return dirty_; }
        void clear_dirty() { dirty_ = false; }

        size_t glyph_count() const { return cache_.size(); }
        f32 occupancy() const;

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
        bool alloc(int w, int h, int &out_x, int &out_y);

        FontFace *face_ = nullptr;
        AtlasGlyph fallback_;
        int width_ = 0;
        int height_ = 0;
        std::vector<u8> pixels_;
        std::unordered_map<u32, AtlasGlyph> cache_;

        int shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
        bool full_ = false;
        bool dirty_ = false;
        u32 generation_ = 0;
    };


    class FontManager
    {
    public:
        FontId load_file(const char *path, f32 pixel_size, int face_index = 0);

        FontId load_system_ui(f32 pixel_size);

        void set_primary_path(const char *path) { primary_path_ = path ? path : ""; }
        const char *primary_path() const { return primary_path_.c_str(); }

        void set_dpi(f32 d) { dpi_ = d > 0.0f ? d : 1.0f; }
        f32 dpi() const { return dpi_; }

        FontFace *face(FontId id);
        GlyphAtlas *atlas(FontId id);

        std::string source_of(FontId id) const;

        f32 measure(FontId id, const char *utf8);
        f32 line_height(FontId id);
        const AtlasGlyph &glyph_at(FontId id, u32 codepoint);

        u32 atlas_generation() const;


        bool has_glyph(FontId id, u32 codepoint) const;

        FontId resolve(FontId primary, u32 codepoint) const;

        void set_fallback_chain(std::vector<FontId> chain) { fallbacks_ = std::move(chain); }
        const std::vector<FontId> &fallback_chain() const { return fallbacks_; }

        void load_fallback_faces(f32 pixel_size);

        const char *face_source(FontId id) const;

        void shutdown();

    private:
        struct Entry
        {
            FontFace face;
            GlyphAtlas atlas;
            std::string source;
        };

        struct FileCacheEntry
        {
            std::string path;
            int face_index = 0;
            std::vector<u8> data;
        };

        std::deque<Entry> faces_;

        std::vector<FontId> fallbacks_;

        std::string primary_path_;

        std::vector<FileCacheEntry> file_cache_;

        f32 dpi_ = 1.0f;
    };

    FontManager &fonts();


    i32 utf8_next(const char *s, size_t len, size_t &pos);
    size_t utf8_len(const char *s, size_t len);


    u32 shape_text(DisplayList &dl, FontId font, const char *utf8, const Vec2 &pen,
                   Color color, f32 max_width, u32 &out_glyph_start);

    u32 shape_text_n(DisplayList &dl, FontId font, const char *utf8, size_t len, const Vec2 &pen,
                     Color color, f32 max_width, u32 &out_glyph_start);

    f32 text_width(FontId font, const char *utf8);
    f32 text_width_n(FontId font, const char *utf8, size_t len);

    f32 text_offset_x(FontId font, const char *utf8, size_t byte_offset);
    f32 text_offset_x_n(FontId font, const char *utf8, size_t len, size_t byte_offset);

    size_t text_hit_index(FontId font, const char *utf8, f32 x);
    size_t text_hit_index_n(FontId font, const char *utf8, size_t len, f32 x);

    u32 text_span_rects(FontId font, const char *utf8, size_t byte_lo, size_t byte_hi, f32 pen_x,
                        f32 line_h, Rect *out, u32 max);
    int wrap_text(FontId font, const char *utf8, f32 max_width,
                  int (*on_line)(const char *, void *), void *user);

}