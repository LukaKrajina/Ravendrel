#include "context.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>

namespace fox
{


    void FocusStack::begin_frame()
    {
        ++frame_;
        order_.clear();
        if (!focused_ && focus_index_ >= 0) focus_index_ = -1;
    }

    void FocusStack::register_widget(Id id, const Rect &r, bool focusable)
    {
        if (!focusable) return;
        order_.push_back(Widget{id, r});
    }

    void FocusStack::set_focus(Id id)
    {
        focused_ = id;
        for (size_t i = 0; i < order_.size(); ++i)
            if (order_[i].id == id)
            {
                focus_index_ = (int)i;
                break;
            }
    }

    bool FocusStack::focus_next(const Rect &)
    {
        if (order_.empty()) return false;
        int n = (int)order_.size();
        int ni = (focus_index_ < 0) ? 0 : (focus_index_ + 1) % n;
        set_focus(order_[(size_t)ni].id);
        return true;
    }

    bool FocusStack::focus_prev(const Rect &)
    {
        if (order_.empty()) return false;
        int n = (int)order_.size();
        int ni = (focus_index_ < 0) ? n - 1 : (focus_index_ - 1 + n) % n;
        set_focus(order_[(size_t)ni].id);
        return true;
    }


    bool Context::init(const char *title, int width, int height)
    {
        title_ = title ? title : "Fox";
        fb_w_ = (int)((f32)width * scale_ + 0.5f);
        fb_h_ = (int)((f32)height * scale_ + 0.5f);
        set_framebuffer_size(fb_w_, fb_h_);

        fonts().set_dpi(scale_);

        if (!platform()) set_platform(make_default_platform());

        return true;
    }

    void Context::shutdown()
    {
        state_cache_.clear();
        text_edits_.clear();
        hover_stack_.clear();
        for (Texture &t : textures_) t.alive = false;
        textures_.clear();
        ++tex_generation_;
    }


    TexId Context::create_texture(u32 w, u32 h, const u8 *rgba)
    {
        const u32 kMaxDim = 16384;
        if (!w || !h || w > kMaxDim || h > kMaxDim)
        {
            std::fprintf(stderr, "[Fox] create_texture rejected %ux%u\n", w, h);
            return TEX_NONE;
        }
        if (!rgba) return TEX_NONE;

        textures_.push_back(Texture{});
        Texture &t = textures_.back();
        t.w = w;
        t.h = h;
        t.rgba.assign(rgba, rgba + (size_t)w * h * 4);
        t.dirty = true;
        t.uploaded = false;
        t.alive = true;
        t.generation = 1;
        ++tex_generation_;
        return (TexId)textures_.size();
    }

    bool Context::update_texture(TexId id, u32 w, u32 h, const u8 *rgba)
    {
        Texture *t = texture_mut(id);
        if (!t || !rgba || !w || !h) return false;

        if (w != t->w || h != t->h || t->rgba.size() != (size_t)w * h * 4)
        {
            t->w = w;
            t->h = h;
            t->rgba.assign(rgba, rgba + (size_t)w * h * 4);
        }
        else
        {
            memcpy(t->rgba.data(), rgba, t->rgba.size());
        }
        t->dirty = true;
        ++t->generation;
        ++tex_generation_;
        t->mips = img::MipChain{};
        return true;
    }

    TexId Context::create_texture_mipped(u32 w, u32 h, const u8 *rgba)
    {
        const TexId id = create_texture(w, h, rgba);
        if (id == TEX_NONE) return TEX_NONE;
        texture_mut(id)->mips = img::build_mips(w, h, rgba);
        return id;
    }

    TexId Context::create_texture_png(const u8 *png, size_t n, const char *name)
    {
        img::Image decoded;
        std::string err;
        if (!png || !img::decode_png(png, n, decoded, &err))
        {
            std::fprintf(stderr, "[Fox] create_texture_png%s%s failed: %s\n",
                         name ? " " : "", name ? name : "", err.empty() ? "bad input" : err.c_str());
            return TEX_NONE;
        }
        return create_texture_mipped(decoded.w, decoded.h, decoded.rgba.data());
    }

    void Context::destroy_texture(TexId id)
    {
        Texture *t = texture_mut(id);
        if (!t) return;
        t->alive = false;
        t->rgba.clear();
        t->rgba.shrink_to_fit();
        t->mips = img::MipChain{};
        t->w = t->h = 0;
        ++tex_generation_;
    }

    void Context::frame_begin()
    {
        dl_.reset();
        layout_.begin(Rect{0, 0, (f32)win_w_, (f32)win_h_}, Direction::Column, 0.0f);
        focus_.begin_frame();

        if (fixed_dt_ > 0.0f)
        {
            dt_ = fixed_dt_;
            time_ += dt_;
        }
        else
        {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
            if (last_time_ == 0.0f) last_time_ = (f32)secs;
            dt_ = (f32)secs - last_time_;
            last_time_ = (f32)secs;
            time_ += dt_;
        }

        ++frame_;
        wants_redraw_ = false;

        prune_state_cache();
        hover_stack_.clear();
    }

    void Context::frame_end()
    {
        layout_.end();
        input_.end_frame();
        topmost_hit_ = focus_.topmost_registered();
    }

    Id Context::make_id(Id salt, const char *label) const
    {
        u32 h = 2166136261u;
        if (label)
            for (const char *p = label; *p; ++p)
            {
                h ^= (u8)*p;
                h *= 16777619u;
            }
        return id_hash(salt, h);
    }

    WidgetState &Context::state(Id id)
    {
        auto it = state_cache_.find(id);
        if (it != state_cache_.end()) return it->second;
        return state_cache_.emplace(id, WidgetState{}).first->second;
    }

    WidgetState &Context::state_touch(Id id)
    {
        WidgetState &s = state(id);
        s.last_frame = frame_;
        s.last_time = time_;
        return s;
    }

    TextBuffer *Context::focused_text_buffer()
    {
        const Id fid = focus_.focused();
        if (!has_text_edit(fid)) return nullptr;
        return &text_edit(fid);
    }

    void Context::ime_begin(const char *utf8, size_t caret_bytes)
    {
        if (TextBuffer *tb = focused_text_buffer())
            tb->composition_begin(std::string_view(utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0),
                                  caret_bytes);
    }

    void Context::ime_update(const char *utf8, size_t caret_bytes)
    {
        if (TextBuffer *tb = focused_text_buffer())
            tb->composition_update(std::string_view(utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0),
                                   caret_bytes);
    }

    void Context::ime_commit()
    {
        if (TextBuffer *tb = focused_text_buffer()) tb->composition_commit();
    }

    void Context::ime_cancel()
    {
        if (TextBuffer *tb = focused_text_buffer()) tb->composition_cancel();
    }

    bool Context::ime_composing() const
    {
        const Id fid = focus_.focused();
        if (!has_text_edit(fid)) return false;
        return text_edits_.find(fid)->second.has_composition();
    }

    void Context::prune_state_cache()
    {
        constexpr f32 kStateStaleSeconds = 10.0f;
        const f32 cutoff = time_ - kStateStaleSeconds;

        for (auto it = state_cache_.begin(); it != state_cache_.end();)
        {
            if (it->second.last_time < cutoff)
                it = state_cache_.erase(it);
            else
                ++it;
        }
        for (auto it = text_edits_.begin(); it != text_edits_.end();)
        {
            if (it->second.last_time() < cutoff)
                it = text_edits_.erase(it);
            else
                ++it;
        }
    }

    TextBuffer &Context::text_edit(Id id)
    {
        auto it = text_edits_.find(id);
        if (it != text_edits_.end()) return it->second;

        TextBuffer &tb = text_edits_.emplace(id, TextBuffer{}).first->second;
        tb.advance_time(time_);
        return tb;
    }

    void Context::sync_text_from_host(TextBuffer &tb, const char *host, size_t host_len)
    {
        tb.advance_time(time_);

        const std::string_view hv(host ? host : "", host ? host_len : 0);
        if (hv == tb.text()) return;

        tb.set_text(hv);
    }

    bool Context::hover(const Rect &r, Id id)
    {
        const bool over = r.contains(input_.mouse_pos);
        WidgetState &s = state_touch(id);
        if (over)
        {
            for (const auto &e : hover_stack_)
            {
                WidgetState &under = state(e.first);
                under.hovered = false;
                under.pressed = false;
            }
            hover_stack_.push_back({id, r});
            s.hovered = true;
            focus_.register_widget(id, r, true);
            return true;
        }
        s.hovered = false;
        return false;
    }

    bool Context::claim_hover(const Rect &r, Id id)
    {
        return hover(r, id);
    }

    bool Context::claim_click(const Rect &r, Id id)
    {
        const bool over = r.contains(input_.mouse_pos);
        WidgetState &s = state_touch(id);
        s.hovered = over;
        if (!over) return false;

        if (topmost_hit_ != 0 && id != topmost_hit_) return false;

        if (input_.buttons[(int)MouseButton::Left].pressed)
        {
            s.pressed = true;
            focus_.set_focus(id);
            return true;
        }
        if (input_.buttons[(int)MouseButton::Left].released)
        {
            s.pressed = false;
            return s.pressed;
        }
        return false;
    }

    Context &ctx()
    {
        static Context c;
        return c;
    }


    FontId Context::font(f32 pixel_size)
    {
        const f32 eff = pixel_size * scale_;
        int key = (int)(eff * 10.0f + 0.5f);
        auto it = fonts_.find(key);
        if (it != fonts_.end()) return it->second;

        FontId id = fonts().load_system_ui(pixel_size);

        if (!fallbacks_tried_)
        {
            fallbacks_tried_ = true;
            fonts().load_fallback_faces(pixel_size);
        }

        fonts_.emplace(key, id);
        return id;
    }
    GlyphAtlas *Context::font_atlas(FontId id)
    {
        return fonts().atlas(id);
    }

    void Context::set_theme(Appearance app, Accent ac)
    {
        theme().init(app, ac);
    }


    void Context::set_framebuffer_size(int w, int h)
    {
        fb_w_ = w;
        fb_h_ = h;
        win_w_ = (int)((f32)w / scale_ + 0.5f);
        win_h_ = (int)((f32)h / scale_ + 0.5f);
    }

    bool Context::set_scale(f32 s)
    {
        if (s <= 0.0f) s = 1.0f;
        if (s == scale_) return false;
        scale_ = s;

        fonts().set_dpi(s);
        fonts_.clear();

        set_framebuffer_size(fb_w_, fb_h_);
        request_redraw();
        return true;
    }

}