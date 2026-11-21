#ifndef DYN_UI_H
#define DYN_UI_H

#include <stddef.h>
#include <stdint.h>

typedef struct dyn_ui dyn_ui_t;
typedef uint32_t dyn_ui_id;

enum { DYN_UI_FIT,
    DYN_UI_GROW,
    DYN_UI_FIXED,
    DYN_UI_PERCENT };
enum { DYN_UI_ROW,
    DYN_UI_COLUMN };
enum { DYN_UI_START,
    DYN_UI_CENTER,
    DYN_UI_END };
enum { DYN_UI_FLOAT_NONE,
    DYN_UI_FLOAT_PARENT,
    DYN_UI_FLOAT_POPUP,
    DYN_UI_FLOAT_ROOT,
    DYN_UI_FLOAT_SAFE };
enum { DYN_UI_ANCHOR_TOP_LEFT,
    DYN_UI_ANCHOR_BOTTOM_LEFT,
    DYN_UI_ANCHOR_TOP_RIGHT };
enum {
    DYN_UI_CMD_RECT = 1,
    DYN_UI_CMD_BORDER,
    DYN_UI_CMD_GLYPHS,
    DYN_UI_CMD_IMAGE,
    DYN_UI_CMD_TRIANGLES,
    DYN_UI_CMD_CLIP,
    DYN_UI_CMD_UNCLIP
};
enum {
    DYN_UI_HOT = 1u << 0,
    DYN_UI_ACTIVE = 1u << 1,
    DYN_UI_PRESSED = 1u << 2,
    DYN_UI_RELEASED = 1u << 3,
    DYN_UI_CLICKED = 1u << 4,
    DYN_UI_FOCUSED = 1u << 5,
    DYN_UI_DOUBLE = 1u << 6,
    DYN_UI_CONTEXT = 1u << 7
};
enum { DYN_UI_FOCUSABLE = 1,
    DYN_UI_DRAG_X = 2,
    DYN_UI_DRAG_Y = 4 };
enum { DYN_UI_POINTER_MOUSE,
    DYN_UI_POINTER_TOUCH,
    DYN_UI_POINTER_PEN };
enum { DYN_UI_TEXT_LINE,
    DYN_UI_TEXT_WRAP,
    DYN_UI_TEXT_ELLIPSIS };
#define DYN_UI_TEX_NONE 0u
#define DYN_UI_TEX_ATLAS 0xFFFFFFFFu
enum {
    DYN_UI_KEY_LEFT = 1,
    DYN_UI_KEY_RIGHT,
    DYN_UI_KEY_UP,
    DYN_UI_KEY_DOWN,
    DYN_UI_KEY_HOME,
    DYN_UI_KEY_END,
    DYN_UI_KEY_BACKSPACE,
    DYN_UI_KEY_DELETE,
    DYN_UI_KEY_RETURN,
    DYN_UI_KEY_TAB,
    DYN_UI_KEY_ESCAPE,
    DYN_UI_KEY_PAGEUP,
    DYN_UI_KEY_PAGEDOWN,
    DYN_UI_KEY_A,
    DYN_UI_KEY_C,
    DYN_UI_KEY_V,
    DYN_UI_KEY_X,
    DYN_UI_KEY_Y,
    DYN_UI_KEY_Z,
    DYN_UI_KEY_SPACE,
    DYN_UI_KEY_COUNT
};
enum { DYN_UI_MOD_SHIFT = 1,
    DYN_UI_MOD_CTRL = 2,
    DYN_UI_MOD_ALT = 4,
    DYN_UI_MOD_SUPER = 8 };
enum { DYN_UI_OK = 0,
    DYN_UI_ERR_NOMEM = -1,
    DYN_UI_ERR_LIMIT = -2,
    DYN_UI_ERR_NESTING = -3,
    DYN_UI_ERR_FONT = -4 };

typedef struct {
    float x, y, w, h;
} dyn_ui_rect;

typedef struct {
    uint8_t kind;
    float value, min, max;
} dyn_ui_size;

typedef struct {
    dyn_ui_size w, h;
    float pad_l, pad_t, pad_r, pad_b;
    float gap;
    uint8_t dir, align_x, align_y;
    uint8_t clip_x, clip_y;
    uint8_t floating;
    uint8_t anchor;
    uint8_t interactive;
    uint8_t wrap;
    int32_t z;
    float float_x, float_y;
} dyn_ui_layout;

typedef struct {
    uint32_t fill, border;
    float border_w, radius;
} dyn_ui_style;

typedef struct {
    float x0, y0, x1, y1, u0, v0, u1, v1;
} dyn_ui_quad;

typedef struct {
    float x, y;
} dyn_ui_vertex;

typedef struct {
    uint8_t type;
    uint32_t color;
    float width, radius;
    dyn_ui_rect rect;
    uint32_t first, count;
    uint32_t image;
    dyn_ui_rect src;
} dyn_ui_cmd;

typedef struct {
    float x, y;
    uint32_t buttons;
    float wheel_x, wheel_y;
    double time;
    uint8_t kind;
} dyn_ui_pointer;

typedef struct {
    int width, height;
    const uint8_t* pixels;
    uint32_t version;
} dyn_ui_atlas_view;

typedef struct {
    float x, y, content_w, content_h, view_w, view_h;
} dyn_ui_scroll_view;

typedef struct {
    int font;
    float font_px, pad_x, pad_y, spacing, window_pad, indent, scrollbar_w, radius, border_w, label_w, min_target, drag_slop, long_press,
        text_scale;
    uint32_t text, text_dim, on_accent, bg, panel, frame, frame_hot, frame_active, button, button_hot, button_active, accent,
        accent_hot, border, title, title_active, selection, popup, shade, tooltip, link, scrollbar, scrollbar_hot, error;
} dyn_ui_theme;

typedef struct {
    float x, y, u, v;
    uint32_t color;
} dyn_ui_gvertex;

typedef struct {
    uint32_t texture;
    dyn_ui_rect clip;
    uint32_t first, count;
} dyn_ui_batch;

typedef const char* (*dyn_ui_clipboard_get)(void* ud, size_t* n);
typedef void (*dyn_ui_clipboard_set)(void* ud, const char* s, size_t n);

dyn_ui_t* dyn_ui_new(void);
void dyn_ui_free(dyn_ui_t* ui);
int dyn_ui_error(const dyn_ui_t* ui);
const char* dyn_ui_error_text(const dyn_ui_t* ui);

int dyn_ui_font_add(dyn_ui_t* ui, const uint8_t* ttf, size_t len, int copy);
int dyn_ui_font_count(const dyn_ui_t* ui);
float dyn_ui_text_width(dyn_ui_t* ui, int font, float px, const char* s, size_t n);
float dyn_ui_line_height(dyn_ui_t* ui, int font, float px);
float dyn_ui_ascent(dyn_ui_t* ui, int font, float px);
dyn_ui_atlas_view dyn_ui_atlas(const dyn_ui_t* ui);

int dyn_ui_image_add(dyn_ui_t* ui, int width, int height, const uint8_t* rgba);
int dyn_ui_image_update(dyn_ui_t* ui, uint32_t image, const uint8_t* rgba);
void dyn_ui_image_remove(dyn_ui_t* ui, uint32_t image);
int dyn_ui_image_size(const dyn_ui_t* ui, uint32_t image, int* width, int* height);

void dyn_ui_input_pointer(dyn_ui_t* ui, const dyn_ui_pointer* p);
void dyn_ui_input_key(dyn_ui_t* ui, int key, int down, int mods);
void dyn_ui_input_text(dyn_ui_t* ui, const char* utf8, size_t n);

int dyn_ui_begin(dyn_ui_t* ui, float width, float height);
int dyn_ui_end(dyn_ui_t* ui, int* changed);
uint64_t dyn_ui_frame(const dyn_ui_t* ui);

dyn_ui_id dyn_ui_hash(dyn_ui_id seed, const void* data, size_t n);
dyn_ui_id dyn_ui_id_of(dyn_ui_t* ui, const char* label, size_t n);
void dyn_ui_push_id(dyn_ui_t* ui, dyn_ui_id id);
void dyn_ui_pop_id(dyn_ui_t* ui);

int dyn_ui_open(dyn_ui_t* ui, dyn_ui_id id, const dyn_ui_layout* layout, const dyn_ui_style* style);
int dyn_ui_close(dyn_ui_t* ui);
int dyn_ui_text(dyn_ui_t* ui, dyn_ui_id id, const char* s, size_t n, int font, float px, uint32_t color, int wrap, uint8_t align);
int dyn_ui_image(dyn_ui_t* ui, dyn_ui_id id, uint32_t image, const dyn_ui_layout* layout, uint32_t tint);
int dyn_ui_triangles(dyn_ui_t* ui, const dyn_ui_vertex* v, uint32_t count, uint32_t color);
int dyn_ui_depth(const dyn_ui_t* ui);

uint32_t dyn_ui_interact(dyn_ui_t* ui, dyn_ui_id id, int focusable);
int dyn_ui_box(const dyn_ui_t* ui, dyn_ui_id id, dyn_ui_rect* out);
int dyn_ui_last_box(const dyn_ui_t* ui, dyn_ui_rect* out);
int dyn_ui_last_item_box(const dyn_ui_t* ui, int item, int column, dyn_ui_rect* out);
void dyn_ui_set_last(dyn_ui_t* ui, dyn_ui_id id);
int dyn_ui_has_last(const dyn_ui_t* ui);
int dyn_ui_drag_source(dyn_ui_t* ui);
int dyn_ui_drop_target(dyn_ui_t* ui);
dyn_ui_id dyn_ui_dragging(const dyn_ui_t* ui);
dyn_ui_id dyn_ui_hot(const dyn_ui_t* ui);
dyn_ui_id dyn_ui_active(const dyn_ui_t* ui);
dyn_ui_id dyn_ui_focus(const dyn_ui_t* ui);
void dyn_ui_set_focus(dyn_ui_t* ui, dyn_ui_id id);
const dyn_ui_pointer* dyn_ui_pointer_state(const dyn_ui_t* ui);
void dyn_ui_pointer_edges(const dyn_ui_t* ui, uint32_t* pressed, uint32_t* released, float* wheel_x, float* wheel_y);
int dyn_ui_key_pressed(const dyn_ui_t* ui, int key);
int dyn_ui_key_down(const dyn_ui_t* ui, int key);
int dyn_ui_mods(const dyn_ui_t* ui);
const char* dyn_ui_typed(const dyn_ui_t* ui, size_t* n);
void* dyn_ui_state(dyn_ui_t* ui, dyn_ui_id id, size_t size, int* fresh);
void dyn_ui_scroll(dyn_ui_t* ui, dyn_ui_id id, float* x, float* y);
int dyn_ui_scroll_info(dyn_ui_t* ui, dyn_ui_id id, dyn_ui_scroll_view* out);

const dyn_ui_cmd* dyn_ui_commands(const dyn_ui_t* ui, size_t* n);
const dyn_ui_quad* dyn_ui_quads(const dyn_ui_t* ui, size_t* n);
const dyn_ui_vertex* dyn_ui_vertices(const dyn_ui_t* ui, size_t* n);
const uint8_t* dyn_ui_image_pixels(const dyn_ui_t* ui, uint32_t image, int* width, int* height);
uint32_t dyn_ui_image_version(const dyn_ui_t* ui, uint32_t image);

void dyn_ui_theme_default(dyn_ui_theme* t);
dyn_ui_theme* dyn_ui_theme_of(dyn_ui_t* ui);
void dyn_ui_set_time(dyn_ui_t* ui, double seconds);
double dyn_ui_time(const dyn_ui_t* ui);
void dyn_ui_wake_at(dyn_ui_t* ui, double seconds);
double dyn_ui_next_wake(const dyn_ui_t* ui);
void dyn_ui_set_clipboard(dyn_ui_t* ui, dyn_ui_clipboard_get get, dyn_ui_clipboard_set set, void* ud);
dyn_ui_id dyn_ui_hot_root(const dyn_ui_t* ui);
void dyn_ui_open_popup(dyn_ui_t* ui, dyn_ui_id id, int level);
void dyn_ui_close_popups(dyn_ui_t* ui, int level);
int dyn_ui_popup_level(const dyn_ui_t* ui, dyn_ui_id id);
int dyn_ui_popup_was_open(const dyn_ui_t* ui, dyn_ui_id id);
int dyn_ui_polyline(dyn_ui_t* ui, const dyn_ui_vertex* norm, uint32_t count, uint32_t color, float thickness);

typedef struct {
    dyn_ui_id id;
    int flags, visible, depth;
} dyn_ui_window_frame;

typedef struct {
    dyn_ui_id id;
    int vertical;
} dyn_ui_split_frame;

typedef struct {
    int depth;
} dyn_ui_menu_frame;

typedef const char* (*dyn_ui_item_fn)(void* ud, int index, size_t* n);

typedef const char* (*dyn_ui_cell_fn)(void* ud, int row, int col, size_t* n);

enum { DYN_UI_EDIT_PASSWORD = 1,
    DYN_UI_EDIT_MULTILINE = 2,
    DYN_UI_EDIT_READONLY = 4 };

enum { DYN_UI_EDIT_CHANGED = 1,
    DYN_UI_EDIT_SUBMITTED = 2,
    DYN_UI_EDIT_FOCUSED = 4 };

enum { DYN_UI_WIN_CLOSABLE = 1,
    DYN_UI_WIN_COLLAPSIBLE = 2,
    DYN_UI_WIN_RESIZABLE = 4,
    DYN_UI_WIN_FIXED = 8 };

void dyn_ui_w_label(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_text(dyn_ui_t* ui, const char* s, size_t n, uint32_t color, int wrap);
void dyn_ui_w_heading(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_bullet(dyn_ui_t* ui, const char* s, size_t n);
int dyn_ui_w_link(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_badge(dyn_ui_t* ui, const char* s, size_t n, uint32_t color);
void dyn_ui_w_image(dyn_ui_t* ui, uint32_t image, float w, float h, uint32_t tint);
void dyn_ui_w_progress(dyn_ui_t* ui, double fraction, float w);
void dyn_ui_w_spinner(dyn_ui_t* ui, float size);
int dyn_ui_w_button(dyn_ui_t* ui, const char* s, size_t n);
int dyn_ui_w_icon_button(dyn_ui_t* ui, const char* s, size_t n, uint32_t image, float size);
int dyn_ui_w_toggle(dyn_ui_t* ui, const char* s, size_t n, int on);
int dyn_ui_w_checkbox(dyn_ui_t* ui, const char* s, size_t n, int checked);
int dyn_ui_w_radio(dyn_ui_t* ui, const char* id_label, size_t idn, const char* const* items, const size_t* lens, int count, int selected, int horizontal);
int dyn_ui_w_switch(dyn_ui_t* ui, const char* s, size_t n, int on);
int dyn_ui_w_segmented(dyn_ui_t* ui, const char* id_label, size_t idn, const char* const* items, const size_t* lens, int count, int selected);
int dyn_ui_w_selectable(dyn_ui_t* ui, const char* s, size_t n, int selected);
int dyn_ui_w_tabs(dyn_ui_t* ui, const char* id_label, size_t idn, const char* const* items, const size_t* lens, int count, int selected);
void dyn_ui_w_separator(dyn_ui_t* ui, const char* s, size_t n);
double dyn_ui_w_slider(dyn_ui_t* ui, const char* s, size_t n, double v, double lo, double hi, double step, int dec, int vertical, float len);
void dyn_ui_w_range(dyn_ui_t* ui, const char* s, size_t n, double* plo, double* phi, double lo, double hi, double step, int dec);
double dyn_ui_w_drag(dyn_ui_t* ui, const char* s, size_t n, double v, double speed, double lo, double hi, int dec);
double dyn_ui_w_spin(dyn_ui_t* ui, const char* s, size_t n, double v, double lo, double hi, double step, int dec);
const char* dyn_ui_w_edit(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, int flags, int max_chars, const char* hint, size_t hn, float rows, float width, size_t* out_n, int* events);
int dyn_ui_w_combo(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count, int selected);
int dyn_ui_w_list(dyn_ui_t* ui, const char* s, size_t n, int count, dyn_ui_item_fn item, void* ud, int selected, int visible, int selectable, int follow, int* activated);
uint32_t dyn_ui_w_color(dyn_ui_t* ui, const char* s, size_t n, uint32_t rgba);
int dyn_ui_w_table(dyn_ui_t* ui, const char* s, size_t n, const char* const* heads, const size_t* hlens, const float* widths, int ncols, int rows, dyn_ui_cell_fn cell, void* ud, int visible, int selected, int* sort_col, int* sort_desc);
void dyn_ui_w_plot(dyn_ui_t* ui, const char* s, size_t n, const double* v, int count, double lo, double hi, float h, int bars);
void dyn_ui_w_property_begin(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_property_end(dyn_ui_t* ui);
int dyn_ui_w_begin(dyn_ui_t* ui, float width, float height);
int dyn_ui_w_end(dyn_ui_t* ui, int* changed);
void dyn_ui_w_spacer(dyn_ui_t* ui, float w, float h);
int dyn_ui_w_window_begin(dyn_ui_t* ui, const char* s, size_t n, float x, float y, float width, float height, int flags, int* closed, dyn_ui_window_frame* fr);
void dyn_ui_w_window_end(dyn_ui_t* ui, const dyn_ui_window_frame* fr);
void dyn_ui_w_panel_begin(dyn_ui_t* ui, const char* s, size_t n, float w, float h, int border);
void dyn_ui_w_panel_end(dyn_ui_t* ui);
void dyn_ui_w_group_begin(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_group_end(dyn_ui_t* ui);
int dyn_ui_w_collapsing(dyn_ui_t* ui, const char* s, size_t n, int default_open);
int dyn_ui_w_tree_begin(dyn_ui_t* ui, const char* s, size_t n, int default_open, int leaf);
void dyn_ui_w_tree_end(dyn_ui_t* ui);
void dyn_ui_w_open_popup(dyn_ui_t* ui, const char* s, size_t n);
int dyn_ui_w_popup_begin(dyn_ui_t* ui, const char* s, size_t n, int* depth);
void dyn_ui_w_popup_end(dyn_ui_t* ui, int depth);
int dyn_ui_w_context_begin(dyn_ui_t* ui, const char* s, size_t n, int* depth);
void dyn_ui_w_close_popup(dyn_ui_t* ui);
void dyn_ui_w_open_modal(dyn_ui_t* ui, const char* s, size_t n);
int dyn_ui_w_modal_begin(dyn_ui_t* ui, const char* s, size_t n, float width, int* depth);
void dyn_ui_w_modal_end(dyn_ui_t* ui, int depth);
void dyn_ui_w_tooltip(dyn_ui_t* ui, const char* s, size_t n, double delay);
float dyn_ui_w_split_begin(dyn_ui_t* ui, const char* s, size_t n, float ratio, int vertical, dyn_ui_split_frame* fr);
void dyn_ui_w_split_next(dyn_ui_t* ui, const dyn_ui_split_frame* fr);
void dyn_ui_w_split_end(dyn_ui_t* ui);
void dyn_ui_w_menubar_begin(dyn_ui_t* ui);
void dyn_ui_w_menubar_end(dyn_ui_t* ui);
int dyn_ui_w_menu_begin(dyn_ui_t* ui, const char* s, size_t n, dyn_ui_menu_frame* fr);
void dyn_ui_w_menu_end(dyn_ui_t* ui, const dyn_ui_menu_frame* fr);
int dyn_ui_w_menu_item(dyn_ui_t* ui, const char* s, size_t n, const char* key, size_t kn, int checked, int enabled);
void dyn_ui_w_toolbar_begin(dyn_ui_t* ui);
void dyn_ui_w_statusbar_begin(dyn_ui_t* ui);
void dyn_ui_w_bar_end(dyn_ui_t* ui);

const char* dyn_ui_w_search(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, const char* hint, size_t hn, float width, size_t* out_n, int* events);

void dyn_ui_set_scale(dyn_ui_t* ui, float scale);
float dyn_ui_scale(const dyn_ui_t* ui);
void dyn_ui_set_insets(dyn_ui_t* ui, float left, float top, float right, float bottom);
const dyn_ui_theme* dyn_ui_resolved(const dyn_ui_t* ui);
void dyn_ui_input_composition(dyn_ui_t* ui, const char* utf8, size_t n, int cursor);
int dyn_ui_text_input(const dyn_ui_t* ui, dyn_ui_rect* caret);
int dyn_ui_tessellate(dyn_ui_t* ui, const dyn_ui_gvertex** v, size_t* nv, const uint32_t** idx, size_t* ni, const dyn_ui_batch** b, size_t* nb);

void dyn_ui_w_app_bar_begin(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_avatar(dyn_ui_t* ui, const char* s, size_t n, uint32_t image, float size);
int dyn_ui_w_breadcrumbs(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count);
void dyn_ui_w_card_begin(dyn_ui_t* ui, const char* s, size_t n, float width);
void dyn_ui_w_card_end(dyn_ui_t* ui);
int dyn_ui_w_chip(dyn_ui_t* ui, const char* s, size_t n, int selected);
int dyn_ui_w_date(dyn_ui_t* ui, const char* s, size_t n, int* py, int* pm, int* pd, int first_day);
int dyn_ui_w_drawer_begin(dyn_ui_t* ui, const char* s, size_t n, int right, float width, int* depth);
int dyn_ui_w_fab(dyn_ui_t* ui, const char* s, size_t n, uint32_t image);
int dyn_ui_w_nav(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, const uint32_t* icons, int count, int selected, int rail);
void dyn_ui_w_open_overlay(dyn_ui_t* ui, const char* s, size_t n);
void dyn_ui_w_overlay_end(dyn_ui_t* ui, int depth);
int dyn_ui_w_pagination(dyn_ui_t* ui, const char* s, size_t n, int page, int pages);
int dyn_ui_w_rating(dyn_ui_t* ui, const char* s, size_t n, int value, int max);
int dyn_ui_w_sheet_begin(dyn_ui_t* ui, const char* s, size_t n, int* depth);
void dyn_ui_w_skeleton(dyn_ui_t* ui, float w, float h);
int dyn_ui_w_stepper(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count, int current);
int dyn_ui_w_toast(dyn_ui_t* ui, const char* s, size_t n, double seconds);
void dyn_ui_w_wrap_begin(dyn_ui_t* ui);

typedef struct {
    int selected, column, sort_col, sort_desc, cells, activated;
    const char* edit;
    size_t edit_n;
    const char* edit_out;
    size_t edit_out_n;
    int committed, cancelled;
} dyn_ui_table_state;

enum { DYN_UI_TIME_H12 = 1 };

int dyn_ui_w_table_ex(dyn_ui_t* ui, const char* s, size_t n, const char* const* heads, const size_t* hlens, const float* widths, int ncols, int rows,
    dyn_ui_cell_fn cell, void* ud, int visible, dyn_ui_table_state* st);
int dyn_ui_w_combo_ex(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count, int selected, int search);
const char* dyn_ui_w_autocomplete(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, const char* const* items, const size_t* lens, int count,
    const char* hint, size_t hn, float width, size_t* out_n, int* picked);
int dyn_ui_w_time(dyn_ui_t* ui, const char* s, size_t n, int* hour, int* minute, int step, int flags);
int dyn_ui_w_tags(dyn_ui_t* ui, const char* s, size_t n, const char* const* tags, const size_t* lens, int count, const char* hint, size_t hn, const char** add,
    size_t* add_n);
void dyn_ui_w_accordion_begin(dyn_ui_t* ui, const char* s, size_t n);
int dyn_ui_w_accordion_item(dyn_ui_t* ui, int index, const char* s, size_t n, int open);
void dyn_ui_w_accordion_section_begin(dyn_ui_t* ui);
void dyn_ui_w_accordion_section_end(dyn_ui_t* ui);
void dyn_ui_w_accordion_end(dyn_ui_t* ui);
int dyn_ui_w_scaffold_begin(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, const uint32_t* icons, int count, int selected);
int dyn_ui_w_scaffold_end(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, const uint32_t* icons, int count, int selected);
void dyn_ui_w_field_error(dyn_ui_t* ui, int on);
void dyn_ui_w_field_note(dyn_ui_t* ui, const char* s, size_t n, int error);
int dyn_ui_w_drag_source(dyn_ui_t* ui, const char* s, size_t n);
int dyn_ui_w_drop_target(dyn_ui_t* ui);

typedef struct {
    uint32_t elements, text_bytes, commands, quads, vertices, hits, runs, run_records, glyphs, atlas_height;
    uint64_t frames, reused;
} dyn_ui_stats_t;

void dyn_ui_stats(const dyn_ui_t* ui, dyn_ui_stats_t* s);

int dyn_ui_raster(const dyn_ui_t* ui, uint8_t* rgba, int width, int height, size_t stride, uint32_t clear);

#endif
