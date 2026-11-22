#pragma once



























#include <stddef.h>

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif


typedef struct fox_ctx fox_ctx;



typedef uint32_t fox_tex;


typedef uint32_t fox_id;


typedef struct fox_rect {
    float x, y, w, h;
} fox_rect;

typedef struct fox_color {
    uint8_t r, g, b, a;
} fox_color;

typedef struct fox_uv {
    float u, v, du, dv;
} fox_uv;


typedef enum {
    FOX_ALIGN_START = 0,
    FOX_ALIGN_CENTER = 1,
    FOX_ALIGN_END = 2,
    FOX_ALIGN_STRETCH = 3,
} fox_align;

typedef enum {
    FOX_BUTTON_PLAIN = 0,
    FOX_BUTTON_FILLED = 1,
    FOX_BUTTON_TINTED = 2,
    FOX_BUTTON_BEZEL = 3,
} fox_button_style;

typedef enum {
    FOX_SLIDER_CONTINUOUS = 0,
    FOX_SLIDER_STEPPED = 1,
} fox_slider_style;

typedef enum {
    FOX_MOUSE_LEFT = 0,
    FOX_MOUSE_RIGHT = 1,
    FOX_MOUSE_MIDDLE = 2,
} fox_mouse_button;

typedef enum { FOX_APPEARANCE_LIGHT = 0, FOX_APPEARANCE_DARK = 1 } fox_appearance;

typedef enum {
    FOX_COLOR_WINDOW_BG = 0,
    FOX_COLOR_SIDEBAR_BG,
    FOX_COLOR_SIDEBAR_TEXT,
    FOX_COLOR_SIDEBAR_TEXT_DIM,
    FOX_COLOR_TEXT,
    FOX_COLOR_TEXT_SECONDARY,
    FOX_COLOR_TEXT_TERTIARY,
    FOX_COLOR_TEXT_ON_ACCENT,
    FOX_COLOR_CONTROL_BG,
    FOX_COLOR_CONTROL_BG_HOVER,
    FOX_COLOR_CONTROL_BG_ACTIVE,
    FOX_COLOR_CONTROL_BG_DISABLED,
    FOX_COLOR_CONTROL_BORDER,
    FOX_COLOR_SEPARATOR,
    FOX_COLOR_SELECTION,
    FOX_COLOR_ACCENT,
    FOX_COLOR_ACCENT_HOVER,
    FOX_COLOR_ACCENT_ACTIVE,
    FOX_COLOR_COUNT
} fox_color_token;


typedef enum {
    FOX_METRIC_CORNER_WINDOW = 0,
    FOX_METRIC_CORNER_PANEL,
    FOX_METRIC_CORNER_CONTROL,
    FOX_METRIC_CORNER_FIELD,
    FOX_METRIC_CONTROL_HEIGHT,
    FOX_METRIC_CONTROL_HEIGHT_SMALL,
    FOX_METRIC_CONTROL_HEIGHT_LARGE,
    FOX_METRIC_SIDEBAR_WIDTH,
    FOX_METRIC_TITLEBAR_HEIGHT,
    FOX_METRIC_ROW_HEIGHT,
    FOX_METRIC_SPACING_XS,
    FOX_METRIC_SPACING_SM,
    FOX_METRIC_SPACING_MD,
    FOX_METRIC_SPACING_LG,
    FOX_METRIC_HAIRLINE,
    FOX_METRIC_FONT_CAPTION,
    FOX_METRIC_FONT_SMALL,
    FOX_METRIC_FONT_BODY,
    FOX_METRIC_FONT_CALLOUT,
    FOX_METRIC_FONT_TITLE3,
    FOX_METRIC_FONT_TITLE2,
    FOX_METRIC_FONT_TITLE1,
    FOX_METRIC_COUNT
} fox_metric_token;


typedef struct fox_config {
    const char *title;
    int32_t width;
    int32_t height;
} fox_config;








fox_ctx *fox_create(const fox_config *cfg);
void fox_destroy(fox_ctx *ctx);











int fox_init_backend_vulkan(fox_ctx *ctx, void *glfw_window);
void fox_shutdown_backend(fox_ctx *ctx);
int fox_backend_ready(fox_ctx *ctx);





int fox_should_close(fox_ctx *ctx);


int fox_has_window(fox_ctx *ctx);


void fox_set_should_close(fox_ctx *ctx, int value);


void fox_begin_frame(fox_ctx *ctx);





void fox_end_frame(fox_ctx *ctx);

void fox_poll_events(fox_ctx *ctx);

void fox_set_size(fox_ctx *ctx, int32_t w, int32_t h);
int32_t fox_width(fox_ctx *ctx);
int32_t fox_height(fox_ctx *ctx);
float fox_time(fox_ctx *ctx);
float fox_dt(fox_ctx *ctx);


float fox_scale(fox_ctx *ctx);





void fox_set_theme(fox_ctx *ctx, fox_appearance appearance);


void fox_set_accent(fox_ctx *ctx, uint32_t rgb);
fox_color fox_theme_color(fox_ctx *ctx, fox_color_token token);
float fox_theme_metric(fox_ctx *ctx, int32_t token);


float fox_mouse_x(fox_ctx *ctx);
float fox_mouse_y(fox_ctx *ctx);
int fox_mouse_down(fox_ctx *ctx, fox_mouse_button button);
int fox_mouse_pressed(fox_ctx *ctx, fox_mouse_button button);
int fox_mouse_released(fox_ctx *ctx, fox_mouse_button button);

int fox_mouse_click_count(fox_ctx *ctx, fox_mouse_button button);
float fox_mouse_wheel(fox_ctx *ctx);

int fox_key_down(fox_ctx *ctx, int32_t key);
int fox_key_pressed(fox_ctx *ctx, int32_t key);
int fox_key_released(fox_ctx *ctx, int32_t key);
int fox_shift(fox_ctx *ctx);
int fox_ctrl(fox_ctx *ctx);
int fox_alt(fox_ctx *ctx);
int fox_super(fox_ctx *ctx);

int fox_key_ascii_down(fox_ctx *ctx, int32_t ascii);
int fox_key_ascii_pressed(fox_ctx *ctx, int32_t ascii);


typedef enum {
    FOX_KEY_NONE = 0,
    FOX_KEY_BACKSPACE = 0x100,
    FOX_KEY_TAB,
    FOX_KEY_ENTER,
    FOX_KEY_ESCAPE,
    FOX_KEY_SPACE = 0x20,
    FOX_KEY_DELETE,
    FOX_KEY_UP,
    FOX_KEY_DOWN,
    FOX_KEY_LEFT,
    FOX_KEY_RIGHT,
    FOX_KEY_HOME,
    FOX_KEY_END,
    FOX_KEY_PAGE_UP,
    FOX_KEY_PAGE_DOWN,
    FOX_KEY_SHIFT,
    FOX_KEY_CONTROL,
    FOX_KEY_ALT,
    FOX_KEY_SUPER,
    FOX_KEY_F1, FOX_KEY_F2,  FOX_KEY_F3,  FOX_KEY_F4,  FOX_KEY_F5,  FOX_KEY_F6,
    FOX_KEY_F7, FOX_KEY_F8,  FOX_KEY_F9,  FOX_KEY_F10, FOX_KEY_F11, FOX_KEY_F12,
} fox_key;




float fox_label(fox_ctx *ctx, fox_rect r, const char *text, fox_color color,
                fox_align align);

int fox_button(fox_ctx *ctx, fox_rect r, const char *label,
               fox_button_style style, int enabled);

int fox_checkbox(fox_ctx *ctx, fox_rect r, const char *label, int *value);
int fox_toggle_switch(fox_ctx *ctx, fox_rect r, int *value);

int fox_slider(fox_ctx *ctx, fox_rect r, float *value, float min, float max,
               fox_slider_style style, float step);


int fox_text_field(fox_ctx *ctx, fox_rect r, char *buffer, int32_t capacity,
                   const char *placeholder, fox_id id);


int32_t fox_text_field_caret(fox_ctx *ctx, fox_id id);

void fox_separator(fox_ctx *ctx, fox_rect r, int horizontal);
void fox_panel(fox_ctx *ctx, fox_rect r, fox_color fill, float radius, int glass);
void fox_glass_bar(fox_ctx *ctx, fox_rect r, float radius);



int fox_menu_title(fox_ctx *ctx, fox_rect r, const char *label);



fox_rect fox_menu_popup(fox_ctx *ctx, fox_rect anchor, const char *items_csv,
                        float item_height);


int fox_menu_item(fox_ctx *ctx, fox_rect r, const char *label, int enabled);

void fox_menu_close_all(fox_ctx *ctx);


int fox_combo(fox_ctx *ctx, fox_rect r, const char *items_csv, int *selected);


int fox_selectable(fox_ctx *ctx, fox_rect r, const char *label, int *selected);



fox_rect fox_group_begin(fox_ctx *ctx, fox_rect r, const char *title,
                         fox_id id, float content_height);
void fox_group_end(fox_ctx *ctx);


int fox_property_int(fox_ctx *ctx, fox_rect r, const char *label, int32_t *value,
                     int32_t min, int32_t max, int32_t step);
int fox_property_float(fox_ctx *ctx, fox_rect r, const char *label, float *value,
                       float min, float max, float step);









void fox_fill_rect(fox_ctx *ctx, fox_rect r, fox_color c);








void fox_push_clip(fox_ctx *ctx, fox_rect r);
void fox_clip_pop(fox_ctx *ctx);




float fox_text_width(fox_ctx *ctx, int32_t px, const char *text);
float fox_text_height(fox_ctx *ctx, int32_t px);
float fox_text_offset_x(fox_ctx *ctx, int32_t px, const char *text,
                        int32_t byte_index);



int32_t fox_text_lines(fox_ctx *ctx, int32_t width, int32_t px, const char *text);




int32_t fox_glyph_count(fox_ctx *ctx, int32_t px);




int32_t fox_char_pending(fox_ctx *ctx);
int32_t fox_char(fox_ctx *ctx);



void fox_fill_ellipse(fox_ctx *ctx, fox_rect r, fox_color c);

void fox_fill_triangle(fox_ctx *ctx, float x0, float y0, float x1, float y1,
                       float x2, float y2, fox_color c);



int fox_path(fox_ctx *ctx, const float *xy, int32_t count, float thickness,
             fox_color c, int cap, int join);



fox_tex fox_texture_create(fox_ctx *ctx, int32_t w, int32_t h, const uint8_t *rgba);
int fox_texture_update(fox_ctx *ctx, fox_tex tex, int32_t w, int32_t h,
                       const uint8_t *rgba);
void fox_texture_destroy(fox_ctx *ctx, fox_tex tex);







fox_tex fox_texture_create_png(fox_ctx *ctx, const void *bytes, size_t len);

int fox_image(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_color tint);
int fox_image_region(fox_ctx *ctx, fox_rect r, fox_tex tex, fox_uv src,
                     fox_color tint);















int32_t fox_icon_register(const char *name, int32_t w, int32_t h,
                          const uint8_t *rgba);


int32_t fox_icon_register_png(const char *name, const void *bytes, size_t len);




int32_t fox_icon_register_png_mask(const char *name, const void *bytes, size_t len);

int32_t fox_icon_find(const char *name);

int32_t fox_icon_count(void);

const char *fox_icon_name(int32_t id);

int fox_icon_draw(fox_ctx *ctx, fox_rect r, int32_t id, fox_color tint);




int fox_window_begin(fox_ctx *ctx, fox_rect bounds, const char *title,
                     int show_sidebar);




int fox_window_close_requested(fox_ctx *ctx);
fox_rect fox_window_content(fox_ctx *ctx);
fox_rect fox_window_sidebar(fox_ctx *ctx);
fox_rect fox_window_titlebar(fox_ctx *ctx);

typedef struct fox_sidebar_item {
    const char *label;
    fox_tex icon_tex;
    fox_uv icon_src;
    const char *icon;
} fox_sidebar_item;

int32_t fox_sidebar(fox_ctx *ctx, fox_rect r, const fox_sidebar_item *items,
                    int32_t count, int32_t selected);



fox_id fox_make_id(uint32_t salt, const char *label);
fox_id fox_make_id_i(uint32_t salt, int32_t n);





fox_id fox_make_id_rect(uint32_t salt, fox_rect r);


const char *fox_error(fox_ctx *ctx);


const char *fox_last_error(void);

const char *fox_window_title(fox_ctx *ctx);












size_t fox_clipboard_text(char *out, size_t cap);
int fox_clipboard_set_text(const char *utf8);



























void fox_ime_begin(fox_ctx *ctx, const char *utf8, size_t caret_bytes);
void fox_ime_update(fox_ctx *ctx, const char *utf8, size_t caret_bytes);
void fox_ime_commit(fox_ctx *ctx);
void fox_ime_cancel(fox_ctx *ctx);
int fox_ime_is_composing(fox_ctx *ctx);












void fox_focus(fox_ctx *ctx, fox_id id);
fox_id fox_focused_id(fox_ctx *ctx);
int fox_has_focus(fox_ctx *ctx, fox_id id);

uint32_t fox_last_instance_count(fox_ctx *ctx);
int fox_used_blur_path(fox_ctx *ctx);




uint32_t fox_frame_faults(fox_ctx *ctx);


const char *fox_last_fault(fox_ctx *ctx);



uint32_t fox_last_frame_dropped(fox_ctx *ctx);




const char *fox_gpu_name(fox_ctx *ctx);
uint32_t fox_gpu_vendor(fox_ctx *ctx);










void fox_set_quality(fox_ctx *ctx, int blur, int shadows, int animations);
void fox_get_quality(fox_ctx *ctx, int *blur, int *shadows, int *animations);



int fox_dump_ppm(fox_ctx *ctx, const char *path);

#if defined(__cplusplus)
}
#endif