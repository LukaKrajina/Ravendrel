#pragma once

#include "core/types.hpp"
#include "draw/display_list.hpp"
#include "image/png.hpp"
#include "input/input.hpp"
#include "layout/layout.hpp"
#include "theme/theme.hpp"
#include "text/font.hpp"
#include "text/text_buffer.hpp"
#include "core/platform.hpp"
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fox
{


    struct WidgetState
    {
        f32 hover = 0.0f;
        f32 active = 0.0f;
        f32 anim = 0.0f;
        bool hovered = false;
        bool pressed = false;
        bool focused = false;
        f32 scroll = 0.0f;
        f32 scroll_x = 0.0f;
        bool visible = true;
        int32_t last_frame = -1;
        f32 last_time = -1e30f;
        i32 caret = 0;
        f32 drag_start_x = 0.0f;
        f32 drag_start_value = 0.0f;
    };


    class FocusStack
    {
    public:
        void begin_frame();
        void register_widget(Id id, const Rect &r, bool focusable);
        void set_focus(Id id);
        Id focused() const { return focused_; }
        bool has_focus(Id id) const { return id != 0 && id == focused_; }
        bool focus_next(const Rect &bounds);
        bool focus_prev(const Rect &bounds);
        void clear_focus() { focused_ = 0; }

        Id topmost_registered() const
        {
            return order_.empty() ? 0 : order_.back().id;
        }

    private:
        struct Widget
        {
            Id id;
            Rect rect;
        };
        std::vector<Widget> order_;
        Id focused_ = 0;
        int focus_index_ = -1;
        int frame_ = 0;
    };


    class Context
    {
    public:
        bool init(const char *title, int width, int height);
        void shutdown();

        void frame_begin();
        void frame_end();
        bool close_requested() const { return close_requested_; }
        const char *title() const { return title_; }
        void set_close_requested(bool on) { close_requested_ = on; }

        bool should_close() const { return close_requested_ || backend_close_requested_; }

        void set_backend_close_requested(bool on) { backend_close_requested_ = on; }

        DisplayList &dl() { return dl_; }
        LayoutSystem &layout() { return layout_; }
        InputState &input() { return input_; }
        FocusStack &focus() { return focus_; }
        Theme &theme() { return fox::theme(); }
        f32 time() const { return time_; }
        f32 dt() const { return dt_; }
        int frame_index() const { return frame_; }

        void set_fixed_dt(f32 seconds) { fixed_dt_ = seconds; }
        f32 fixed_dt() const { return fixed_dt_; }

        void set_framebuffer_size(int w, int h);

        void set_size_logical(int w, int h)
        {
            const f32 s = scale_ > 0.0f ? scale_ : 1.0f;
            set_framebuffer_size((int)((f32)w * s + 0.5f), (int)((f32)h * s + 0.5f));
        }

        int width() const { return win_w_; }
        int height() const { return win_h_; }

        int framebuffer_width() const { return fb_w_; }
        int framebuffer_height() const { return fb_h_; }

        f32 scale() const { return scale_; }

        bool set_scale(f32 s);

        Id make_id(Id salt, const char *label) const;
        Id make_id(Id salt, Id n) const { return id_hash(salt, n); }

        WidgetState &state(Id id);
        WidgetState &state_touch(Id id);

        bool claim_hover(const Rect &r, Id id);
        bool claim_click(const Rect &r, Id id);

        void prune_state_cache();


        TextBuffer &text_edit(Id id);

        bool has_text_edit(Id id) const
        {
            return id != 0 && text_edits_.find(id) != text_edits_.end();
        }

        const TextBuffer *text_edit_peek(Id id) const
        {
            auto it = text_edits_.find(id);
            return it == text_edits_.end() ? nullptr : &it->second;
        }

        void ime_begin(const char *utf8, size_t caret_bytes);
        void ime_update(const char *utf8, size_t caret_bytes);
        void ime_commit();
        void ime_cancel();
        bool ime_composing() const;

        TextBuffer *focused_text_buffer();

        void sync_text_from_host(TextBuffer &tb, const char *host, size_t host_len);

        void request_redraw() { wants_redraw_ = true; }

        bool hover(const Rect &r, Id id);

        f32 time_seconds() const { return time_; }

        void (*on_resize)(int w, int h) = nullptr;

        struct Texture
        {
            u32 w = 0, h = 0;
            std::vector<u8> rgba;
            bool dirty = true;
            bool uploaded = false;
            u32 generation = 0;
            bool alive = false;

            img::MipChain mips;

            u32 mip_levels() const { return mips.levels.empty() ? 1u : (u32)mips.levels.size(); }
        };

        TexId create_texture(u32 w, u32 h, const u8 *rgba);

        TexId create_texture_png(const u8 *png, size_t n, const char *name = nullptr);

        TexId create_texture_mipped(u32 w, u32 h, const u8 *rgba);

        bool update_texture(TexId id, u32 w, u32 h, const u8 *rgba);

        void destroy_texture(TexId id);

        const Texture *texture(TexId id) const
        {
            if (id == TEX_NONE || id > textures_.size()) return nullptr;
            return textures_[id - 1].alive ? &textures_[id - 1] : nullptr;
        }

        Texture *texture_mut(TexId id)
        {
            if (id == TEX_NONE || id > textures_.size()) return nullptr;
            return textures_[id - 1].alive ? &textures_[id - 1] : nullptr;
        }

        const u8 *sample_rgba(TexId id, f32 u, f32 v) const
        {
            const Texture *t = texture(id);
            if (!t || t->w == 0 || t->h == 0 || t->rgba.empty()) return nullptr;
            int x = (int)(u * (f32)t->w);
            int y = (int)(v * (f32)t->h);
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= (int)t->w) x = (int)t->w - 1;
            if (y >= (int)t->h) y = (int)t->h - 1;
            return &t->rgba[((size_t)y * t->w + x) * 4];
        }

        void sample_rgba_lod(TexId id, f32 u, f32 v, f32 lod, u8 out[4]) const
        {
            const Texture *t = texture(id);
            if (!t || t->w == 0 || t->h == 0 || t->rgba.empty())
            {
                out[0] = out[1] = out[2] = out[3] = 0;
                return;
            }
            if (t->mips.levels.size() > 1)
            {
                img::sample_trilinear(t->mips, u, v, lod, out);
                return;
            }
            const u8 *s = sample_rgba(id, u, v);
            out[0] = s[0];
            out[1] = s[1];
            out[2] = s[2];
            out[3] = s[3];
        }

        u32 texture_generation() const { return tex_generation_; }

        size_t texture_count() const { return textures_.size(); }


        FontId font(f32 pixel_size);

        bool cull_hidden() const { return cull_hidden_; }
        void set_cull_hidden(bool on) { cull_hidden_ = on; }

        struct Quality
        {
            int blur = 0;
            int shadows = 0;
            bool animations = true;
        };

        const Quality &quality() const { return quality_; }
        void set_quality(int blur, int shadows, bool animations)
        {
            quality_.blur = blur < 0 ? 0 : (blur > 2 ? 2 : blur);
            quality_.shadows = shadows < 0 ? 0 : (shadows > 2 ? 2 : shadows);
            quality_.animations = animations;
        }

        f32 anim_dt() const { return quality_.animations ? dt_ : 1.0e9f; }

        u32 hidden_culled() const { return hidden_culled_; }
        void note_hidden_culled(u32 n) { hidden_culled_ = n; }

        void set_font_file(const char *path) { fonts().set_primary_path(path); }
        FontId default_font() { return font(theme().font_body); }
        GlyphAtlas *font_atlas(FontId id);

        void set_theme(Appearance app, Accent ac);


        Id open_popup() const { return open_popup_; }
        void set_open_popup(Id anchor) { open_popup_ = anchor; }
        bool popup_is_open(Id anchor) const { return anchor != 0 && anchor == open_popup_; }

    private:
        DisplayList dl_;
        LayoutSystem layout_;
        InputState input_;
        FocusStack focus_;

        std::unordered_map<Id, WidgetState> state_cache_;

        std::unordered_map<Id, TextBuffer> text_edits_;

        Id open_popup_ = 0;

        std::unordered_map<int, FontId> fonts_;

        bool fallbacks_tried_ = false;

        std::deque<Texture> textures_;
        u32 tex_generation_ = 0;

        int win_w_ = 800;
        int win_h_ = 600;
        int fb_w_ = 800;
        int fb_h_ = 600;
        f32 scale_ = 1.0f;
        bool close_requested_ = false;
        Id topmost_hit_ = 0;
        const char *title_ = "Fox";
        bool backend_close_requested_ = false;
        bool wants_redraw_ = false;
        bool cull_hidden_ = true;
        u32 hidden_culled_ = 0;
        Quality quality_{};

        int frame_ = 0;
        f32 time_ = 0.0f;
        f32 dt_ = 0.0f;
        f32 last_time_ = 0.0f;
        f32 fixed_dt_ = 0.0f;

        std::vector<std::pair<Id, Rect>> hover_stack_;
    };

    Context &ctx();

}