#include "fox.h"

#include "core/context.hpp"
#include "image/png.hpp"
#include "text/soft_wrap.hpp"
#include "backend/vk_backend.hpp"
#include "widget/primitives.hpp"
#include "widget/composite.hpp"
#include "window/window.hpp"

#include <exception>
#include <new>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstring>

using namespace fox;

namespace
{
    char g_last_error[256] = {0};

    void set_last_error(const char *msg)
    {
        if (!msg)
        {
            g_last_error[0] = '\0';
            return;
        }
        std::snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
    }
}


namespace
{
    Rect to_rect(fox_rect r) { return Rect{r.x, r.y, r.w, r.h}; }
    fox_rect from_rect(const Rect &r) { return fox_rect{r.x, r.y, r.w, r.h}; }

    Color to_color(fox_color c) { return Color(c.r, c.g, c.b, c.a); }
    fox_color from_color(const Color &c) { return fox_color{c.r, c.g, c.b, c.a}; }

    Rect to_uv(fox_uv s) { return Rect{s.u, s.v, s.du, s.dv}; }

    ButtonStyle to_button_style(int s)
    {
        switch (s)
        {
        case FOX_BUTTON_PLAIN: return ButtonStyle::Plain;
        case FOX_BUTTON_FILLED: return ButtonStyle::Filled;
        case FOX_BUTTON_BEZEL: return ButtonStyle::Bezel;
        case FOX_BUTTON_TINTED:
        default: return ButtonStyle::Tinted;
        }
    }

    SliderStyle to_slider_style(int s)
    {
        return s == FOX_SLIDER_STEPPED ? SliderStyle::Stepped : SliderStyle::Continuous;
    }

    Align to_align(int a)
    {
        switch (a)
        {
        case FOX_ALIGN_CENTER: return Align::Center;
        case FOX_ALIGN_END: return Align::End;
        case FOX_ALIGN_STRETCH: return Align::Stretch;
        case FOX_ALIGN_START:
        default: return Align::Start;
        }
    }

    int from_bool(bool b) { return b ? 1 : 0; }
}


struct fox_ctx
{
    Context ctx;
    VkBackend backend;
    Window window;
    bool backend_ready = false;
    std::string error;

    struct IconTex
    {
        fox_tex tex = TEX_NONE;
        uint32_t version = 0;
    };
    std::unordered_map<int32_t, IconTex> icon_cache;

    int sidebar_scroll = 0;

    void fail(const char *msg)
    {
        error = msg ? msg : "unspecified error";
        std::fprintf(stderr, "[fox] %s\n", error.c_str());
        set_last_error(error.c_str());
    }
};

static_assert(sizeof(fox_rect) == 4 * sizeof(float),
              "fox_rect must stay four floats");
static_assert(sizeof(fox_color) == 4, "fox_color must stay four bytes");


extern "C" fox_ctx *fox_create(const fox_config *cfg)
{
    fox_ctx *c = nullptr;
    try
    {
        c = new fox_ctx();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[fox] create failed: %s\n", e.what());
        set_last_error(e.what());
        return nullptr;
    }
    catch (...)
    {
        set_last_error("out of memory");
        return nullptr;
    }

    int w = cfg && cfg->width > 0 ? cfg->width : 1000;
    int h = cfg && cfg->height > 0 ? cfg->height : 680;
    const char *title = (cfg && cfg->title) ? cfg->title : "Fox";
    c->ctx.init(title, w, h);
    return c;
}

extern "C" void fox_destroy(fox_ctx *ctx)
{
    if (!ctx) return;
    ctx->backend.shutdown();
    ctx->ctx.shutdown();
    delete ctx;
}

extern "C" int fox_init_backend_vulkan(fox_ctx *ctx, void *glfw_window)
{
    if (!ctx) return 0;
    try
    {
        if (!ctx->backend.init((GLFWwindow *)glfw_window, ctx->ctx))
        {
            ctx->fail("fox_init_backend_vulkan failed");
            return 0;
        }
        ctx->backend_ready = true;
        return 1;
    }
    catch (...)
    {
        ctx->fail("fox_init_backend_vulkan threw");
        return 0;
    }
}

extern "C" void fox_shutdown_backend(fox_ctx *ctx)
{
    if (!ctx) return;
    ctx->backend.shutdown();
    ctx->backend_ready = false;
}

extern "C" int fox_backend_ready(fox_ctx *ctx)
{
    if (!ctx) return 0;
    return ctx->backend_ready && ctx->backend.ready() ? 1 : 0;
}

extern "C" int fox_should_close(fox_ctx *ctx)
{
    if (!ctx) return 0;
    return ctx->ctx.should_close() ? 1 : 0;
}

extern "C" int fox_has_window(fox_ctx *ctx)
{
    return (ctx && ctx->backend_ready && ctx->backend.has_window()) ? 1 : 0;
}

extern "C" void fox_set_should_close(fox_ctx *ctx, int value)
{
    if (!ctx) return;
    ctx->ctx.set_close_requested(value != 0);
}


extern "C" void fox_poll_events(fox_ctx *ctx)
{
    if (ctx && ctx->backend_ready) ctx->backend.poll_events(ctx->ctx);
}

extern "C" void fox_begin_frame(fox_ctx *ctx)
{
    if (ctx) ctx->ctx.frame_begin();
}

extern "C" void fox_end_frame(fox_ctx *ctx)
{
    if (!ctx) return;
    ctx->ctx.frame_end();
    if (ctx->backend_ready) ctx->backend.render(ctx->ctx.dl(), ctx->ctx);
}

extern "C" void fox_set_size(fox_ctx *ctx, int32_t w, int32_t h)
{
    if (!ctx || w <= 0 || h <= 0) return;
    ctx->ctx.set_size_logical((int)w, (int)h);
}

extern "C" int32_t fox_width(fox_ctx *ctx) { return ctx ? ctx->ctx.width() : 0; }
extern "C" int32_t fox_height(fox_ctx *ctx) { return ctx ? ctx->ctx.height() : 0; }
extern "C" float fox_time(fox_ctx *ctx) { return ctx ? ctx->ctx.time() : 0.0f; }
extern "C" float fox_dt(fox_ctx *ctx) { return ctx ? ctx->ctx.dt() : 0.0f; }
extern "C" float fox_scale(fox_ctx *ctx) { return ctx ? ctx->ctx.scale() : 1.0f; }


extern "C" void fox_set_theme(fox_ctx *ctx, fox_appearance appearance)
{
    if (!ctx) return;
    ctx->ctx.set_theme(appearance == FOX_APPEARANCE_DARK ? Appearance::Dark
                                                        : Appearance::Light,
                       ctx->ctx.theme().accent);
}

extern "C" void fox_set_accent(fox_ctx *ctx, uint32_t rgb)
{
    if (!ctx) return;
    ctx->ctx.theme().set_accent_rgb(rgb);
}

extern "C" fox_color fox_theme_color(fox_ctx *ctx, fox_color_token token)
{
    if (!ctx) return fox_color{0, 0, 0, 255};
    const Theme &t = ctx->ctx.theme();
    switch (token)
    {
    case FOX_COLOR_WINDOW_BG: return from_color(t.window_bg);
    case FOX_COLOR_SIDEBAR_BG: return from_color(t.sidebar_bg);
    case FOX_COLOR_SIDEBAR_TEXT: return from_color(t.sidebar_text);
    case FOX_COLOR_SIDEBAR_TEXT_DIM: return from_color(t.sidebar_text_dim);
    case FOX_COLOR_TEXT: return from_color(t.text);
    case FOX_COLOR_TEXT_SECONDARY: return from_color(t.text_secondary);
    case FOX_COLOR_TEXT_TERTIARY: return from_color(t.text_tertiary);
    case FOX_COLOR_TEXT_ON_ACCENT: return from_color(t.text_on_accent);
    case FOX_COLOR_CONTROL_BG: return from_color(t.control_bg);
    case FOX_COLOR_CONTROL_BG_HOVER: return from_color(t.control_bg_hover);
    case FOX_COLOR_CONTROL_BG_ACTIVE: return from_color(t.control_bg_active);
    case FOX_COLOR_CONTROL_BG_DISABLED: return from_color(t.control_bg_disabled);
    case FOX_COLOR_CONTROL_BORDER: return from_color(t.control_border);
    case FOX_COLOR_SEPARATOR: return from_color(t.separator);
    case FOX_COLOR_SELECTION: return from_color(t.selection);
    case FOX_COLOR_ACCENT: return from_color(t.accent_color);
    case FOX_COLOR_ACCENT_HOVER: return from_color(t.accent_hover);
    case FOX_COLOR_ACCENT_ACTIVE: return from_color(t.accent_active);
    default: return from_color(t.text);
    }
}

extern "C" float fox_theme_metric(fox_ctx *ctx, int32_t token)
{
    if (!ctx) return 0.0f;
    const Theme &t = ctx->ctx.theme();
    switch (token)
    {
    case FOX_METRIC_CORNER_WINDOW: return t.corner_window;
    case FOX_METRIC_CORNER_PANEL: return t.corner_panel;
    case FOX_METRIC_CORNER_CONTROL: return t.corner_control;
    case FOX_METRIC_CORNER_FIELD: return t.corner_field;
    case FOX_METRIC_CONTROL_HEIGHT: return t.control_height;
    case FOX_METRIC_CONTROL_HEIGHT_SMALL: return t.control_height_small;
    case FOX_METRIC_CONTROL_HEIGHT_LARGE: return t.control_height_large;
    case FOX_METRIC_SIDEBAR_WIDTH: return t.sidebar_width;
    case FOX_METRIC_TITLEBAR_HEIGHT: return t.titlebar_height;
    case FOX_METRIC_ROW_HEIGHT: return t.row_height;
    case FOX_METRIC_SPACING_XS: return t.spacing_xs;
    case FOX_METRIC_SPACING_SM: return t.spacing_sm;
    case FOX_METRIC_SPACING_MD: return t.spacing_md;
    case FOX_METRIC_SPACING_LG: return t.spacing_lg;
    case FOX_METRIC_HAIRLINE: return t.hairline;
    case FOX_METRIC_FONT_CAPTION: return t.font_caption;
    case FOX_METRIC_FONT_SMALL: return t.font_small;
    case FOX_METRIC_FONT_BODY: return t.font_body;
    case FOX_METRIC_FONT_CALLOUT: return t.font_callout;
    case FOX_METRIC_FONT_TITLE3: return t.font_title3;
    case FOX_METRIC_FONT_TITLE2: return t.font_title2;
    case FOX_METRIC_FONT_TITLE1: return t.font_title1;
    default: return 0.0f;
    }
}


extern "C" float fox_mouse_x(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.input().mouse_pos.x : 0.0f;
}
extern "C" float fox_mouse_y(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.input().mouse_pos.y : 0.0f;
}
extern "C" int fox_mouse_down(fox_ctx *ctx, fox_mouse_button b)
{
    if (!ctx || (int)b >= (int)MouseButton::Count) return 0;
    return from_bool(ctx->ctx.input().buttons[(int)b].down);
}
extern "C" int fox_mouse_pressed(fox_ctx *ctx, fox_mouse_button b)
{
    if (!ctx || (int)b >= (int)MouseButton::Count) return 0;
    return from_bool(ctx->ctx.input().buttons[(int)b].pressed);
}
extern "C" int fox_mouse_released(fox_ctx *ctx, fox_mouse_button b)
{
    if (!ctx || (int)b >= (int)MouseButton::Count) return 0;
    return from_bool(ctx->ctx.input().buttons[(int)b].released);
}
extern "C" int fox_mouse_click_count(fox_ctx *ctx, fox_mouse_button button)
{
    if (!ctx || (int)button >= (int)MouseButton::Count) return 0;
    return ctx->ctx.input().buttons[(int)button].click_count;
}
extern "C" float fox_mouse_wheel(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.input().scroll_y : 0.0f;
}

extern "C" int fox_key_down(fox_ctx *ctx, int32_t key)
{
    return ctx ? from_bool(ctx->ctx.input().key_down((Key)key)) : 0;
}
extern "C" int fox_key_pressed(fox_ctx *ctx, int32_t key)
{
    return ctx ? from_bool(ctx->ctx.input().key_press((Key)key)) : 0;
}
extern "C" int fox_key_released(fox_ctx *ctx, int32_t key)
{
    return ctx ? from_bool(ctx->ctx.input().key_released_now((Key)key)) : 0;
}
extern "C" int fox_key_ascii_down(fox_ctx *ctx, int32_t a)
{
    return ctx && a >= 0 && a < KEY_SLOTS ? from_bool(ctx->ctx.input().keys_down[a]) : 0;
}
extern "C" int fox_key_ascii_pressed(fox_ctx *ctx, int32_t a)
{
    return ctx && a >= 0 && a < KEY_SLOTS ? from_bool(ctx->ctx.input().keys_pressed[a]) : 0;
}
extern "C" int fox_shift(fox_ctx *ctx) { return ctx && ctx->ctx.input().shift; }
extern "C" int fox_ctrl(fox_ctx *ctx) { return ctx && ctx->ctx.input().ctrl; }
extern "C" int fox_alt(fox_ctx *ctx) { return ctx && ctx->ctx.input().alt; }
extern "C" int fox_super(fox_ctx *ctx) { return ctx && ctx->ctx.input().super_key; }


extern "C" float fox_label(fox_ctx *ctx, fox_rect r, const char *text, fox_color color,
                           fox_align align)
{
    if (!ctx || !text) return 0.0f;
    return label(ctx->ctx, to_rect(r), text, to_color(color), to_align(align));
}


extern "C" void fox_push_clip(fox_ctx *ctx, fox_rect r)
{
    if (!ctx) return;
    ctx->ctx.dl().push_clip(to_rect(r));
}

extern "C" void fox_clip_pop(fox_ctx *ctx)
{
    if (!ctx) return;
    ctx->ctx.dl().push_clip_pop();
}


extern "C" float fox_text_width(fox_ctx *ctx, int32_t px, const char *text)
{
    if (!ctx || !text) return 0.0f;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    return text_width(f, text);
}

extern "C" float fox_text_height(fox_ctx *ctx, int32_t px)
{
    if (!ctx) return 0.0f;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    return fonts().line_height(f);
}

extern "C" float fox_text_offset_x(fox_ctx *ctx, int32_t px, const char *text,
                                   int32_t byte_index)
{
    if (!ctx || !text) return 0.0f;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    return text_offset_x(f, text, (size_t)(byte_index > 0 ? byte_index : 0));
}

extern "C" int32_t fox_text_lines(fox_ctx *ctx, int32_t width, int32_t px, const char *text)
{
    if (!ctx || !text) return 0;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    const std::string s(text);
    const f32 avail = (f32)(width > 0 ? width : 1);
    size_t rows = 0;
    size_t ls = 0;
    const size_t n = s.size();
    for (size_t i = 0; i <= n; ++i)
    {
        if (i == n || s[i] == '\n')
        {
            std::vector<WrapSpan> spans((i - ls) + 1);
            const int nr = wrap_line(f, s, ls, i, avail, spans.data(), (int)spans.size());
            rows += (size_t)(nr > 0 ? nr : 1);
            ls = i + 1;
        }
    }
    return (int32_t)(rows > 0 ? rows : 1);
}

extern "C" int32_t fox_glyph_count(fox_ctx *ctx, int32_t px)
{
    if (!ctx) return 0;
    const FontId f = ctx->ctx.font((f32)(px > 0 ? px : 1));
    GlyphAtlas *a = fonts().atlas(f);
    return a ? (int32_t)a->glyph_count() : 0;
}


extern "C" int32_t fox_char_pending(fox_ctx *ctx)
{
    if (!ctx) return 0;
    return (int32_t)ctx->ctx.input().chars.size();
}

extern "C" int32_t fox_char(fox_ctx *ctx)
{
    if (!ctx || ctx->ctx.input().chars.empty()) return 0;
    const u32 cp = ctx->ctx.input().chars.front();
    ctx->ctx.input().chars.erase(ctx->ctx.input().chars.begin());
    return (int32_t)cp;
}

extern "C" int fox_button(fox_ctx *ctx, fox_rect r, const char *label_text,
                          fox_button_style style, int enabled)
{
    if (!ctx || !label_text) return 0;
    return from_bool(button(ctx->ctx, to_rect(r), label_text, to_button_style(style),
                            enabled != 0));
}

extern "C" int fox_checkbox(fox_ctx *ctx, fox_rect r, const char *label_text, int *value)
{
    if (!ctx || !label_text || !value) return 0;
    bool v = *value != 0;
    const bool changed = checkbox(ctx->ctx, to_rect(r), label_text, v);
    *value = v ? 1 : 0;
    return from_bool(changed);
}

extern "C" int fox_toggle_switch(fox_ctx *ctx, fox_rect r, int *value)
{
    if (!ctx || !value) return 0;
    bool v = *value != 0;
    const bool changed = toggle_switch(ctx->ctx, to_rect(r), v);
    *value = v ? 1 : 0;
    return from_bool(changed);
}

extern "C" int fox_slider(fox_ctx *ctx, fox_rect r, float *value, float min, float max,
                          fox_slider_style style, float step)
{
    if (!ctx || !value) return 0;
    float v = *value;
    const bool dragging = slider(ctx->ctx, to_rect(r), v, min, max, to_slider_style(style), step);
    *value = v;
    return from_bool(dragging);
}


namespace
{
    struct CsvList
    {
        std::string storage;
        std::vector<const char *> items;
        int count() const { return (int)items.size(); }
    };

    CsvList split_csv(const char *csv)
    {
        CsvList list;
        if (!csv) return list;
        list.storage = csv;
        char *p = list.storage.data();
        list.items.push_back(p);
        for (char &ch : list.storage)
        {
            if (ch == ',')
            {
                ch = '\0';
                list.items.push_back(&ch + 1);
            }
        }
        return list;
    }
}

extern "C" int fox_menu_title(fox_ctx *ctx, fox_rect r, const char *label)
{
    if (!ctx || !label) return 0;
    return from_bool(menu_title(ctx->ctx, to_rect(r), label));
}

extern "C" fox_rect fox_menu_popup(fox_ctx *ctx, fox_rect anchor,
                                   const char *items_csv, float item_height)
{
    if (!ctx) return fox_rect{0, 0, 0, 0};
    const CsvList items = split_csv(items_csv);
    const Rect content = menu_popup(ctx->ctx, to_rect(anchor), items.count(),
                                    item_height > 0.0f ? item_height : 22.0f);
    return from_rect(content);
}

extern "C" int fox_menu_item(fox_ctx *ctx, fox_rect r, const char *label, int enabled)
{
    if (!ctx || !label) return 0;
    return menu_item(ctx->ctx, to_rect(r), label, enabled != 0) ? 1 : 0;
}

extern "C" void fox_menu_close_all(fox_ctx *ctx)
{
    if (ctx) menu_close_all(ctx->ctx);
}

extern "C" int fox_combo(fox_ctx *ctx, fox_rect r, const char *items_csv, int *selected)
{
    if (!ctx || !selected) return -1;
    const CsvList items = split_csv(items_csv);
    int sel = *selected;
    const bool changed = combo(ctx->ctx, to_rect(r), items.items.data(), items.count(), sel);
    *selected = sel;
    return changed ? sel : -1;
}

extern "C" int fox_selectable(fox_ctx *ctx, fox_rect r, const char *label, int *selected)
{
    if (!ctx || !label || !selected) return 0;
    bool v = *selected != 0;
    const bool clicked = selectable(ctx->ctx, to_rect(r), label, v);
    *selected = v ? 1 : 0;
    return from_bool(clicked);
}

extern "C" fox_rect fox_group_begin(fox_ctx *ctx, fox_rect r, const char *title,
                                    fox_id id, float content_height)
{
    if (!ctx) return fox_rect{0, 0, 0, 0};
    return from_rect(group_begin(ctx->ctx, to_rect(r), title, (Id)id, content_height));
}

extern "C" void fox_group_end(fox_ctx *ctx)
{
    if (ctx) group_end(ctx->ctx);
}

extern "C" int fox_property_int(fox_ctx *ctx, fox_rect r, const char *label,
                                int32_t *value, int32_t min, int32_t max, int32_t step)
{
    if (!ctx || !value) return 0;
    int v = *value;
    const bool changed = property_int(ctx->ctx, to_rect(r), label, v, (int)min, (int)max,
                                      (int)(step > 0 ? step : 1));
    *value = v;
    return from_bool(changed);
}

extern "C" int fox_property_float(fox_ctx *ctx, fox_rect r, const char *label,
                                  float *value, float min, float max, float step)
{
    if (!ctx || !value) return 0;
    float v = *value;
    const bool changed = property_float(ctx->ctx, to_rect(r), label, v, min, max, step);
    *value = v;
    return from_bool(changed);
}

extern "C" int fox_text_field(fox_ctx *ctx, fox_rect r, char *buffer, int32_t capacity,
                              const char *placeholder, fox_id id)
{
    if (!ctx || !buffer || capacity <= 1) return 0;
    return from_bool(text_field(ctx->ctx, to_rect(r), buffer, (int)capacity, placeholder,
                                 (Id)id));
}

extern "C" int32_t fox_text_field_caret(fox_ctx *ctx, fox_id id)
{
    if (!ctx || id == 0) return 0;
    const TextBuffer *tb = ctx->ctx.text_edit_peek((Id)id);
    return tb ? (int32_t)tb->caret() : 0;
}

extern "C" void fox_separator(fox_ctx *ctx, fox_rect r, int horizontal)
{
    if (ctx) separator(ctx->ctx, to_rect(r), horizontal != 0);
}

extern "C" void fox_panel(fox_ctx *ctx, fox_rect r, fox_color fill, float radius, int glass)
{
    if (ctx) panel(ctx->ctx, to_rect(r), to_color(fill), radius, glass != 0);
}

extern "C" void fox_glass_bar(fox_ctx *ctx, fox_rect r, float radius)
{
    if (ctx) glass_bar(ctx->ctx, to_rect(r), radius);
}


extern "C" void fox_fill_rect(fox_ctx *ctx, fox_rect r, fox_color c)
{
    if (ctx) ctx->ctx.dl().push(to_rect(r), to_color(c));
}

extern "C" void fox_fill_ellipse(fox_ctx *ctx, fox_rect r, fox_color c)
{
    if (ctx) ctx->ctx.dl().push_ellipse(to_rect(r), to_color(c));
}

extern "C" void fox_fill_triangle(fox_ctx *ctx, float x0, float y0, float x1, float y1,
                                  float x2, float y2, fox_color c)
{
    if (!ctx) return;
    ctx->ctx.dl().push_triangle(Vec2{x0, y0}, Vec2{x1, y1}, Vec2{x2, y2}, to_color(c));
}

extern "C" int fox_path(fox_ctx *ctx, const float *xy, int32_t count, float thickness,
                        fox_color c, int cap, int join)
{
    if (!ctx || !xy || count < 2) return 0;

    Vec2 *pts = nullptr;
    try
    {
        pts = new Vec2[(size_t)count];
    }
    catch (...)
    {
        ctx->fail("fox_path allocation failed");
        return 0;
    }
    for (int32_t i = 0; i < count; ++i)
    {
        pts[i].x = xy[i * 2];
        pts[i].y = xy[i * 2 + 1];
    }
    ctx->ctx.dl().push_path(pts, (u32)count, thickness, to_color(c), (u8)cap, (u8)join);
    delete[] pts;
    return 1;
}


extern "C" fox_tex fox_texture_create(fox_ctx *ctx, int32_t w, int32_t h,
                                      const uint8_t *rgba)
{
    if (!ctx) return TEX_NONE;
    return ctx->ctx.create_texture((u32)w, (u32)h, rgba);
}

extern "C" int fox_texture_update(fox_ctx *ctx, fox_tex tex, int32_t w, int32_t h,
                                  const uint8_t *rgba)
{
    if (!ctx) return 0;
    return from_bool(ctx->ctx.update_texture(tex, (u32)w, (u32)h, rgba));
}

extern "C" void fox_texture_destroy(fox_ctx *ctx, fox_tex tex)
{
    if (ctx) ctx->ctx.destroy_texture(tex);
}

extern "C" fox_tex fox_texture_create_png(fox_ctx *ctx, const void *bytes, size_t len)
{
    if (!ctx || !bytes) return TEX_NONE;
    return ctx->ctx.create_texture_png((const u8 *)bytes, len, "fox_texture_create_png");
}

extern "C" int fox_image(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_color tint)
{
    if (!ctx) return 0;
    return from_bool(image(ctx->ctx, to_rect(r), tex, to_color(tint)));
}

extern "C" int fox_image_region(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_uv src,
                                fox_color tint)
{
    if (!ctx) return 0;
    return from_bool(image_region(ctx->ctx, to_rect(r), tex, to_uv(src), to_color(tint)));
}


namespace
{
    struct IconDef
    {
        std::string name;
        bool png = false;
        int w = 0;
        int h = 0;
        std::vector<uint8_t> bytes;
        uint32_t version = 1;
    };

    std::vector<IconDef> &icon_defs()
    {
        static std::vector<IconDef> defs;
        return defs;
    }

    int32_t register_def(const char *name, bool png, int w, int h,
                         const void *data, size_t len)
    {
        if (!name || !*name || !data || len == 0) return -1;
        if (!png && (w <= 0 || h <= 0)) return -1;
        if (png && (w < 0 || h < 0)) return -1;

        auto &defs = icon_defs();
        for (size_t i = 0; i < defs.size(); ++i)
        {
            if (defs[i].name == name)
            {
                defs[i].png = png;
                defs[i].w = w;
                defs[i].h = h;
                defs[i].bytes.assign((const uint8_t *)data, (const uint8_t *)data + len);
                ++defs[i].version;
                return (int32_t)i;
            }
        }

        IconDef d;
        d.name = name;
        d.png = png;
        d.w = w;
        d.h = h;
        d.bytes.assign((const uint8_t *)data, (const uint8_t *)data + len);
        defs.push_back(std::move(d));
        return (int32_t)(defs.size() - 1);
    }
}

extern "C" int32_t fox_icon_register(const char *name, int32_t w, int32_t h,
                                     const uint8_t *rgba)
{
    if (w <= 0 || h <= 0) return -1;
    if (w > 4096 || h > 4096) return -1;
    if (!rgba) return -1;
    return register_def(name, false, (int)w, (int)h, rgba, (size_t)w * (size_t)h * 4u);
}

extern "C" int32_t fox_icon_register_png(const char *name, const void *bytes, size_t len)
{
    return register_def(name, true, 0, 0, bytes, len);
}

extern "C" int32_t fox_icon_register_png_mask(const char *name, const void *bytes, size_t len)
{
    if (!bytes || len == 0) return -1;
    img::Image im;
    std::string err;
    if (!img::decode_png((const u8 *)bytes, len, im, &err) || im.w == 0 || im.h == 0)
    {
        set_last_error(err.empty() ? "PNG decode failed" : err.c_str());
        return -1;
    }
    std::vector<uint8_t> rgba((size_t)im.w * im.h * 4u, 255);
    for (size_t i = 0; i < (size_t)im.w * im.h; ++i)
        rgba[i * 4u + 3u] = im.rgba[i * 4u + 3u];
    return register_def(name, false, im.w, im.h, rgba.data(), rgba.size());
}

extern "C" int32_t fox_icon_find(const char *name)
{
    if (!name) return -1;
    const auto &defs = icon_defs();
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].name == name) return (int32_t)i;
    return -1;
}

extern "C" int32_t fox_icon_count(void)
{
    return (int32_t)icon_defs().size();
}

extern "C" const char *fox_icon_name(int32_t id)
{
    const auto &defs = icon_defs();
    if (id < 0 || (size_t)id >= defs.size()) return nullptr;
    return defs[(size_t)id].name.c_str();
}

extern "C" int fox_icon_draw(fox_ctx *ctx, fox_rect r, int32_t id, fox_color tint)
{
    if (!ctx) return 0;
    const auto &defs = icon_defs();
    if (id < 0 || (size_t)id >= defs.size()) return 0;
    const IconDef &d = defs[(size_t)id];

    fox_ctx::IconTex &slot = ctx->icon_cache[id];
    if (slot.tex != TEX_NONE && slot.version != d.version)
    {
        ctx->ctx.destroy_texture(slot.tex);
        slot.tex = TEX_NONE;
    }

    if (slot.tex == TEX_NONE)
    {
        if (d.png)
        {
            slot.tex = ctx->ctx.create_texture_png(d.bytes.data(), d.bytes.size(),
                                                   "fox_icon_draw");
        }
        else
        {
            slot.tex = ctx->ctx.create_texture((u32)d.w, (u32)d.h, d.bytes.data());
        }
        slot.version = d.version;
        if (slot.tex == TEX_NONE)
        {
            ctx->fail(d.png ? "fox_icon_draw: PNG decode failed"
                            : "fox_icon_draw: texture upload failed");
            return 0;
        }
    }

    return from_bool(image(ctx->ctx, to_rect(r), slot.tex, to_color(tint)));
}


extern "C" int fox_window_begin(fox_ctx *ctx, fox_rect bounds, const char *title,
                                int show_sidebar)
{
    if (!ctx) return 0;
    ctx->window.begin(ctx->ctx, to_rect(bounds), title ? title : "Fox",
                      show_sidebar != 0);
    return 1;
}

extern "C" int fox_window_close_requested(fox_ctx *ctx)
{
    return (ctx && ctx->window.close_requested()) ? 1 : 0;
}

extern "C" fox_rect fox_window_content(fox_ctx *ctx)
{
    return ctx ? from_rect(ctx->window.content()) : fox_rect{0, 0, 0, 0};
}
extern "C" fox_rect fox_window_sidebar(fox_ctx *ctx)
{
    return ctx ? from_rect(ctx->window.sidebar()) : fox_rect{0, 0, 0, 0};
}
extern "C" fox_rect fox_window_titlebar(fox_ctx *ctx)
{
    return ctx ? from_rect(ctx->window.titlebar()) : fox_rect{0, 0, 0, 0};
}

extern "C" int32_t fox_sidebar(fox_ctx *ctx, fox_rect r, const fox_sidebar_item *items,
                               int32_t count, int32_t selected)
{
    if (!ctx || !items || count <= 0) return -1;

    SidebarItem *tmp = nullptr;
    try
    {
        tmp = new SidebarItem[(size_t)count];
    }
    catch (...)
    {
        ctx->fail("fox_sidebar allocation failed");
        return -1;
    }
    for (int32_t i = 0; i < count; ++i)
    {
        tmp[i].label = items[i].label;
        tmp[i].icon_tex = (TexId)items[i].icon_tex;
        tmp[i].icon_src = to_uv(items[i].icon_src);
        if (tmp[i].icon_src.w <= 0.0f && tmp[i].icon_src.h <= 0.0f)
            tmp[i].icon_src = Rect{0.0f, 0.0f, 1.0f, 1.0f};
        tmp[i].icon = items[i].icon;
    }
    const int32_t clicked =
        sidebar(ctx->ctx, to_rect(r), tmp, (int)count, (int)selected, ctx->sidebar_scroll);
    delete[] tmp;
    return clicked;
}


extern "C" fox_id fox_make_id(uint32_t salt, const char *label_text)
{
    return ctx().make_id((Id)salt, label_text);
}
extern "C" fox_id fox_make_id_i(uint32_t salt, int32_t n)
{
    return ctx().make_id((Id)salt, (Id)n);
}
extern "C" fox_id fox_make_id_rect(uint32_t salt, fox_rect r)
{
    return ctx().make_id((Id)salt, id_hash_rect(to_rect(r)));
}


extern "C" const char *fox_error(fox_ctx *ctx)
{
    return ctx ? ctx->error.c_str() : "null context";
}

extern "C" const char *fox_last_error(void)
{
    return g_last_error;
}

extern "C" const char *fox_window_title(fox_ctx *ctx)
{
    return ctx ? ctx->ctx.title() : "";
}

extern "C" uint32_t fox_last_instance_count(fox_ctx *ctx)
{
    return ctx ? ctx->backend.last_instance_count() : 0;
}

extern "C" uint32_t fox_frame_faults(fox_ctx *ctx)
{
    if (!ctx) return 0;
    if (!ctx->backend_ready) return (uint32_t)ctx->ctx.frame_index();
    return ctx->backend.frame_faults();
}

extern "C" const char *fox_last_fault(fox_ctx *ctx)
{
    return ctx ? ctx->backend.last_fault() : "null context";
}

extern "C" uint32_t fox_last_frame_dropped(fox_ctx *ctx)
{
    return ctx ? ctx->backend.last_dropped() : 0;
}

extern "C" const char *fox_gpu_name(fox_ctx *ctx)
{
    return ctx ? ctx->backend.gpu_name() : "";
}

extern "C" uint32_t fox_gpu_vendor(fox_ctx *ctx)
{
    return ctx ? ctx->backend.gpu_vendor() : 0;
}

extern "C" void fox_set_quality(fox_ctx *ctx, int blur, int shadows, int animations)
{
    if (ctx) ctx->ctx.set_quality(blur, shadows, animations != 0);
}

extern "C" void fox_get_quality(fox_ctx *ctx, int *blur, int *shadows, int *animations)
{
    const Context::Quality q = ctx ? ctx->ctx.quality() : Context::Quality{};
    if (blur) *blur = q.blur;
    if (shadows) *shadows = q.shadows;
    if (animations) *animations = q.animations ? 1 : 0;
}

extern "C" size_t fox_clipboard_text(char *out, size_t cap)
{
    Platform *p = platform();
    if (!p) { if (out && cap) out[0] = 0; return 0; }
    p->clipboard_text_into(out, cap);
    if (!out || cap == 0) return 0;
    return strlen(out);
}

extern "C" int fox_clipboard_set_text(const char *utf8)
{
    Platform *p = platform();
    if (!p || !utf8) return 0;
    p->set_clipboard_text(std::string_view(utf8));
    return 1;
}

extern "C" void fox_ime_begin(fox_ctx *ctx, const char *utf8, size_t caret_bytes)
{
    if (ctx) ctx->ctx.ime_begin(utf8, caret_bytes);
}
extern "C" void fox_ime_update(fox_ctx *ctx, const char *utf8, size_t caret_bytes)
{
    if (ctx) ctx->ctx.ime_update(utf8, caret_bytes);
}
extern "C" void fox_ime_commit(fox_ctx *ctx)
{
    if (ctx) ctx->ctx.ime_commit();
}
extern "C" void fox_ime_cancel(fox_ctx *ctx)
{
    if (ctx) ctx->ctx.ime_cancel();
}
extern "C" int fox_ime_is_composing(fox_ctx *ctx)
{
    return ctx && ctx->ctx.ime_composing() ? 1 : 0;
}

extern "C" void fox_focus(fox_ctx *ctx, fox_id id)
{
    if (!ctx) return;
    if (id == 0) ctx->ctx.ime_cancel();
    ctx->ctx.focus().set_focus((Id)id);
}

extern "C" fox_id fox_focused_id(fox_ctx *ctx)
{
    return ctx ? (fox_id)ctx->ctx.focus().focused() : 0;
}

extern "C" int fox_has_focus(fox_ctx *ctx, fox_id id)
{
    return ctx && ctx->ctx.focus().has_focus((Id)id) ? 1 : 0;
}

extern "C" int fox_used_blur_path(fox_ctx *ctx)
{
    return ctx ? from_bool(ctx->backend.used_blur_path()) : 0;
}

extern "C" int fox_dump_ppm(fox_ctx *ctx, const char *path)
{
    if (!ctx || !path) return 0;
    return from_bool(ctx->backend.dump_ppm(path));
}