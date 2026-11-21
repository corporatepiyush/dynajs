typedef struct {
    int* s;
    int n, cap, max;
    dyn_ui_t* ui;
} uw_str;

static int uw_str_insert(uw_str* b, int at, const int* c, int n);
static float uw_cp_width(dyn_ui_t* ui, int cp);

#define STB_TEXTEDIT_STRING uw_str
#define STB_TEXTEDIT_CHARTYPE int
#define STB_TEXTEDIT_POSITIONTYPE int
#define STB_TEXTEDIT_UNDOSTATECOUNT 99
#define STB_TEXTEDIT_UNDOCHARCOUNT 4096
#define STB_TEXTEDIT_STRINGLEN(o) ((o)->n)
#define STB_TEXTEDIT_LAYOUTROW(r, o, i) uw_layout_row(r, o, i)
#define STB_TEXTEDIT_GETWIDTH(o, n, i) uw_cp_width((o)->ui, (o)->s[(n) + (i)])
#define STB_TEXTEDIT_KEYTOTEXT(k) ((k) == '\n' || ((k) >= 0x20 && (k) < 0x110000) ? (k) : -1)
#define STB_TEXTEDIT_GETCHAR(o, i) ((o)->s[i])
#define STB_TEXTEDIT_NEWLINE '\n'
#define STB_TEXTEDIT_IS_SPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n')
#define STB_TEXTEDIT_DELETECHARS(o, i, n) uw_str_delete(o, i, n)
#define STB_TEXTEDIT_INSERTCHARS(o, i, c, n) uw_str_insert(o, i, c, n)
#define STB_TEXTEDIT_K_SHIFT 0x400000
#define STB_TEXTEDIT_K_LEFT 0x200001
#define STB_TEXTEDIT_K_RIGHT 0x200002
#define STB_TEXTEDIT_K_UP 0x200003
#define STB_TEXTEDIT_K_DOWN 0x200004
#define STB_TEXTEDIT_K_PGUP 0x200005
#define STB_TEXTEDIT_K_PGDOWN 0x200006
#define STB_TEXTEDIT_K_LINESTART 0x200007
#define STB_TEXTEDIT_K_LINEEND 0x200008
#define STB_TEXTEDIT_K_TEXTSTART 0x200009
#define STB_TEXTEDIT_K_TEXTEND 0x20000A
#define STB_TEXTEDIT_K_DELETE 0x20000B
#define STB_TEXTEDIT_K_BACKSPACE 0x20000C
#define STB_TEXTEDIT_K_UNDO 0x20000D
#define STB_TEXTEDIT_K_REDO 0x20000E
#define STB_TEXTEDIT_K_WORDLEFT 0x20000F
#define STB_TEXTEDIT_K_WORDRIGHT 0x200010

#include "stb_textedit.h"

static void uw_layout_row(StbTexteditRow* r, uw_str* o, int i);
static void uw_str_delete(uw_str* b, int at, int n);

#define STB_TEXTEDIT_IMPLEMENTATION
#include "stb_textedit.h"

struct uw_state {
    dyn_ui_id edit_id;
    uw_str buf;
    STB_TexteditState st;
    int single;
    char* out;
    size_t out_n, out_cap;
    char* last;
    size_t last_n, last_cap;
    char scratch[64];
    int popup_depth;
    uint32_t window_stamp;
    int32_t screen;
    float dock_t, dock_b, dock_b_last;
    struct {
        char* text;
        size_t n;
        double until;
    } toasts[8];
    int ntoasts;
    dyn_ui_id blocks[64];
    int nblocks;
    int field_error;
    float edit_h;
    int scaffold_rail, scaffold_sel;
    dyn_ui_id scaffold_id;
    dyn_ui_id accs[16];
    int naccs;
};

enum { UW_EDIT_KEYLIST = 0x100 };

enum { UW_Z_WINDOW = 0x10000000,
    UW_Z_POPUP = 0x40000000,
    UW_Z_MODAL = 0x60000000,
    UW_Z_TOOLTIP = 0x7F000000 };

static void uw_free(dyn_ui_t* ui)
{
    if (!ui->w)
        return;
    {
        int k;
        for (k = 0; k < ui->w->ntoasts; k++)
            free(ui->w->toasts[k].text);
    }
    free(ui->w->buf.s);
    free(ui->w->out);
    free(ui->w->last);
    free(ui->w);
    ui->w = NULL;
}

static struct uw_state* uw_get(dyn_ui_t* ui)
{
    if (!ui->w) {
        ui->w = calloc(1, sizeof(*ui->w));
        if (!ui->w)
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for widget state");
        else
            ui->w->buf.ui = ui;
    }
    return ui->w;
}

static int uw_str_insert(uw_str* b, int at, const int* c, int n)
{
    if (n < 0 || at < 0 || at > b->n || (b->max > 0 && b->n + n > b->max) || b->n > (1 << 24) - n)
        return 0;
    if (b->n + n > b->cap) {
        int cap = b->cap ? b->cap : 64;
        int* s;
        while (cap < b->n + n)
            cap *= 2;
        s = realloc(b->s, (size_t)cap * sizeof(int));
        if (!s)
            return 0;
        b->s = s;
        b->cap = cap;
    }
    memmove(b->s + at + n, b->s + at, (size_t)(b->n - at) * sizeof(int));
    memcpy(b->s + at, c, (size_t)n * sizeof(int));
    b->n += n;
    return 1;
}

static void uw_str_delete(uw_str* b, int at, int n)
{
    if (at < 0 || n <= 0 || at >= b->n)
        return;
    if (n > b->n - at)
        n = b->n - at;
    memmove(b->s + at, b->s + at + n, (size_t)(b->n - at - n) * sizeof(int));
    b->n -= n;
}

static size_t uw_utf8_put(char* o, uint32_t c)
{
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18));
    o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static float uw_cp_width(dyn_ui_t* ui, int cp)
{
    const ui_glyph* g;
    if (cp == '\n' || ui->rt.font < 0 || ui->rt.font >= ui->nfonts)
        return 0;
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    g = ui_glyph_get(ui, ui->rt.font, ui->rt.font_px, (uint32_t)cp);
    return g ? g->advance : 0;
}

static float uw_lh(dyn_ui_t* ui)
{
    return dyn_ui_line_height(ui, ui->rt.font, ui->rt.font_px);
}

static float uw_fh(dyn_ui_t* ui)
{
    float h = uw_lh(ui) + 2 * ui->rt.pad_y;
    return h > ui->rt.min_target ? h : ui->rt.min_target;
}

static float uw_px(const dyn_ui_t* ui, float pt)
{
    return roundf(pt * ui->scale);
}

static float uw_hair(const dyn_ui_t* ui)
{
    return fmaxf(1, roundf(ui->scale));
}

static float uw_lhx(dyn_ui_t* ui, float f)
{
    return uw_px(ui, roundf(uw_lh(ui) / ui->scale * f));
}

#define UW_OVERLAY_SEED 0x0E7A1A00u

static dyn_ui_id uw_overlay_id(const char* s, size_t n)
{
    return dyn_ui_hash(UW_OVERLAY_SEED, s, n);
}

static int uw_commit(const dyn_ui_t* ui, uint32_t bits)
{
    return (bits & (ui->ptr.kind == DYN_UI_POINTER_MOUSE ? DYN_UI_PRESSED : DYN_UI_CLICKED)) != 0;
}

static float uw_bar_h(dyn_ui_t* ui)
{
    return uw_lh(ui) + 2 * ui->rt.pad_y;
}

static void uw_layout_row(StbTexteditRow* r, uw_str* o, int i)
{
    int k = i;
    float x = 0;
    while (k < o->n && o->s[k] != '\n')
        x += uw_cp_width(o->ui, o->s[k++]);
    if (k < o->n && !(o->ui->w && o->ui->w->single))
        k++;
    r->x0 = 0;
    r->x1 = x;
    r->baseline_y_delta = uw_lh(o->ui);
    r->ymin = 0;
    r->ymax = uw_lh(o->ui);
    r->num_chars = k - i;
}

static size_t uw_shown(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++)
        if (s[i] == '#' && s[i + 1] == '#')
            return i;
    return n;
}

static dyn_ui_id uw_id(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_id id = dyn_ui_id_of(ui, s, n);
    ui->last_id = id;
    ui->last_items = 0;
    return id;
}

static dyn_ui_id uw_sub(dyn_ui_id id, uint32_t k)
{
    return dyn_ui_hash(id, &k, sizeof(k));
}

static void uw_items(dyn_ui_t* ui, dyn_ui_id last, dyn_ui_id base, uint32_t off)
{
    ui->last_id = last;
    ui->last_items = base;
    ui->last_items_off = off;
    ui->last_cols_n = 0;
}

static dyn_ui_id uw_tag(dyn_ui_t* ui, uint32_t kind, const void* s, size_t n)
{
    const unsigned char* p = s;
    size_t m = n < 32 ? n : 32, i;
    uint64_t h = ((uint64_t)(ui->nids ? ui->ids[ui->nids - 1] : 0) << 32 | (0x7A60000u + kind)) ^ ((uint64_t)n * 0xC2B2AE3D27D4EB4Full);
    dyn_ui_id id;
    for (i = 0; i + 8 <= m; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    if (i < m) {
        uint64_t w = 0;
        size_t j;
        for (j = 0; i + j < m; j++)
            w |= (uint64_t)p[i + j] << (8 * j);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    id = (dyn_ui_id)(h ^ (h >> 32));
    id = id ? id : 1;
    ui->last_id = id;
    ui->last_items = 0;
    return id;
}

static void uw_block_push(dyn_ui_t* ui, dyn_ui_id id)
{
    struct uw_state* w = ui->w;
    if (w && w->nblocks < (int)(sizeof(w->blocks) / sizeof(w->blocks[0])))
        w->blocks[w->nblocks] = id;
    if (w)
        w->nblocks++;
}

static void uw_block_pop(dyn_ui_t* ui)
{
    struct uw_state* w = ui->w;
    if (!w || w->nblocks <= 0)
        return;
    w->nblocks--;
    if (w->nblocks < (int)(sizeof(w->blocks) / sizeof(w->blocks[0])) && w->blocks[w->nblocks])
        dyn_ui_set_last(ui, w->blocks[w->nblocks]);
}

static dyn_ui_size uw_sz(int kind, float v)
{
    dyn_ui_size s;
    memset(&s, 0, sizeof(s));
    s.kind = (uint8_t)kind;
    s.value = v;
    return s;
}

#define UW_FIT uw_sz(DYN_UI_FIT, 0)
#define UW_GROW(v) uw_sz(DYN_UI_GROW, v)
#define UW_FIXED(v) uw_sz(DYN_UI_FIXED, v)
#define UW_PCT(v) uw_sz(DYN_UI_PERCENT, v)

static dyn_ui_layout uw_l(dyn_ui_size w, dyn_ui_size h)
{
    dyn_ui_layout l;
    memset(&l, 0, sizeof(l));
    l.w = w;
    l.h = h;
    return l;
}

static void uw_pad(dyn_ui_layout* l, float x, float y)
{
    l->pad_l = l->pad_r = x;
    l->pad_t = l->pad_b = y;
}

static dyn_ui_style uw_s(uint32_t fill, uint32_t border, float bw, float radius)
{
    dyn_ui_style s;
    s.fill = fill;
    s.border = border;
    s.border_w = bw;
    s.radius = radius;
    return s;
}

static int uw_font(dyn_ui_t* ui, float px)
{
    if (ui->theme.font >= 0 && ui->theme.font < ui->nfonts)
        return ui->theme.font;
    return fabsf(px - 13) < 0.01f || ui->nfonts < 2 ? 0 : 1;
}

static void uw_text(dyn_ui_t* ui, const char* s, size_t n, uint32_t color)
{
    dyn_ui_text(ui, 0, s, n, ui->rt.font, ui->rt.font_px, color, 0, DYN_UI_START);
}

static void uw_label_fit(dyn_ui_t* ui, const char* s, size_t n, uint32_t color)
{
    dyn_ui_text(ui, 0, s, n, ui->rt.font, ui->rt.font_px, color, DYN_UI_TEXT_ELLIPSIS, DYN_UI_START);
}

static void uw_rect(dyn_ui_t* ui, dyn_ui_layout l, uint32_t fill)
{
    dyn_ui_style s = uw_s(fill, 0, 0, 0);
    dyn_ui_open(ui, 0, &l, &s);
    dyn_ui_close(ui);
}

static void uw_float_rect(dyn_ui_t* ui, float x, float y, float w, float h, uint32_t fill, float radius)
{
    dyn_ui_layout l = uw_l(UW_FIXED(w), UW_FIXED(h));
    dyn_ui_style s = uw_s(fill, 0, 0, radius);
    l.floating = DYN_UI_FLOAT_PARENT;
    l.float_x = x;
    l.float_y = y;
    dyn_ui_open(ui, 0, &l, &s);
    dyn_ui_close(ui);
}

static void uw_cross(dyn_ui_t* ui, uint32_t color)
{
    dyn_ui_vertex a[2] = { { 0.25f, 0.25f }, { 0.75f, 0.75f } }, b[2] = { { 0.75f, 0.25f }, { 0.25f, 0.75f } };
    dyn_ui_polyline(ui, a, 2, color, 1.5f * ui->scale);
    dyn_ui_polyline(ui, b, 2, color, 1.5f * ui->scale);
}

static uint32_t uw_fill(uint32_t bits, uint32_t base, uint32_t hot, uint32_t active)
{
    return (bits & DYN_UI_ACTIVE) ? active : (bits & DYN_UI_HOT) ? hot
                                                                 : base;
}

static uint32_t uw_border(dyn_ui_t* ui, dyn_ui_id id)
{
    if (ui->w && ui->w->field_error)
        return ui->rt.error;
    return ui->focus == id && ui->focus_visible ? ui->rt.accent : ui->rt.border;
}

static int uw_activated(dyn_ui_t* ui, dyn_ui_id id, uint32_t bits)
{
    if (bits & DYN_UI_CLICKED)
        return 1;
    return ui->focus == id && (ui->fin.key_hit[DYN_UI_KEY_RETURN] > 0 || ui->fin.key_hit[DYN_UI_KEY_SPACE] > 0) && ui->focus_visible;
}

static size_t uw_fmt(char* o, size_t cap, double v, int dec)
{
    char tmp[48];
    size_t n = 0, k = 0;
    uint64_t ip, fp, p10 = 1;
    int i, neg;
    if (dec < 0)
        dec = 0;
    if (dec > 9)
        dec = 9;
    if (!isfinite(v))
        return (size_t)snprintf(o, cap, "%s", v != v ? "NaN" : v > 0 ? "inf"
                                                                     : "-inf");
    for (i = 0; i < dec; i++)
        p10 *= 10;
    neg = v < 0;
    if (neg)
        v = -v;
    if (v * (double)p10 >= 9e15)
        return (size_t)snprintf(o, cap, "%s%.3e", neg ? "-" : "", v);
    fp = (uint64_t)llround(v * (double)p10);
    ip = fp / p10;
    fp %= p10;
    if (neg && (ip || fp))
        tmp[n++] = '-';
    do {
        tmp[n + k++] = (char)('0' + ip % 10);
        ip /= 10;
    } while (ip);
    for (i = 0; i < (int)k / 2; i++) {
        char t = tmp[n + i];
        tmp[n + i] = tmp[n + k - 1 - i];
        tmp[n + k - 1 - i] = t;
    }
    n += k;
    if (dec) {
        tmp[n++] = '.';
        for (i = dec - 1; i >= 0; i--) {
            tmp[n + i] = (char)('0' + fp % 10);
            fp /= 10;
        }
        n += (size_t)dec;
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(o, tmp, n);
    o[n] = 0;
    return n;
}

void dyn_ui_w_label(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_text(ui, uw_tag(ui, 1, s, n), s, n, ui->rt.font, ui->rt.font_px, ui->rt.text, 0, DYN_UI_START);
}

void dyn_ui_w_text(dyn_ui_t* ui, const char* s, size_t n, uint32_t color, int wrap)
{
    dyn_ui_text(ui, uw_tag(ui, 2, s, n), s, n, ui->rt.font, ui->rt.font_px, color, wrap, DYN_UI_START);
}

void dyn_ui_w_heading(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_text(ui, uw_tag(ui, 3, s, n), s, n, uw_font(ui, ui->rt.font_px * 2), ui->rt.font_px * 2, ui->rt.text, 0, DYN_UI_START);
}

void dyn_ui_w_bullet(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_layout row = uw_l(UW_FIT, UW_FIT);
    float d = uw_lhx(ui, 1.0f / 3);
    row.dir = DYN_UI_ROW;
    row.gap = ui->rt.pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, uw_tag(ui, 4, s, n), &row, NULL);
    uw_rect(ui, uw_l(UW_FIXED(d), UW_FIXED(d)), ui->rt.text_dim);
    dyn_ui_text(ui, 0, s, n, ui->rt.font, ui->rt.font_px, ui->rt.text, 1, DYN_UI_START);
    dyn_ui_close(ui);
}

int dyn_ui_w_link(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
    dyn_ui_style st = uw_s(0, 0, 0, 0);
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_text(ui, s, uw_shown(s, n), ui->rt.link);
    if (bits & (DYN_UI_HOT | DYN_UI_ACTIVE) || (ui->focus == id && ui->focus_visible))
        uw_float_rect(ui, 0, uw_lh(ui) - uw_hair(ui), dyn_ui_text_width(ui, ui->rt.font, ui->rt.font_px, s, uw_shown(s, n)), uw_hair(ui), ui->rt.link, 0);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits);
}

void dyn_ui_w_badge(dyn_ui_t* ui, const char* s, size_t n, uint32_t color)
{
    dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
    dyn_ui_style st = uw_s(color ? color : ui->rt.accent, 0, 0, ui->rt.radius > 0 ? (uw_lh(ui) + 2 * uw_px(ui, 1)) / 2 : 0);
    uw_pad(&l, floorf(ui->rt.pad_x / 2), uw_px(ui, 1));
    dyn_ui_open(ui, uw_tag(ui, 5, s, n), &l, &st);
    uw_text(ui, s, n, ui->rt.on_accent);
    dyn_ui_close(ui);
}

void dyn_ui_w_image(dyn_ui_t* ui, uint32_t image, float w, float h, uint32_t tint)
{
    dyn_ui_layout l = uw_l(w > 0 ? UW_FIXED(w) : UW_FIT, h > 0 ? UW_FIXED(h) : UW_FIT);
    dyn_ui_image(ui, uw_tag(ui, 6, &image, sizeof(image)), image, &l, tint ? tint : 0xFFFFFFFFu);
}

void dyn_ui_w_progress(dyn_ui_t* ui, double fraction, float w)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout l = uw_l(w > 0 ? UW_FIXED(w) : UW_GROW(1), UW_FIXED(uw_bar_h(ui)));
    dyn_ui_style st = uw_s(t->frame, t->border, t->border_w, t->radius);
    float f = fraction > 0 ? fraction < 1 ? (float)fraction : 1.0f : 0.0f;
    l.dir = DYN_UI_ROW;
    uw_pad(&l, t->border_w, t->border_w);
    dyn_ui_open(ui, uw_tag(ui, 7, "", 0), &l, &st);
    if (f > 0)
        uw_rect(ui, uw_l(UW_PCT(f), UW_GROW(1)), t->accent);
    dyn_ui_close(ui);
}

void dyn_ui_w_spinner(dyn_ui_t* ui, float size)
{
    const dyn_ui_theme* t = &ui->rt;
    float s = size > 0 ? size : uw_lh(ui), c = s / 2, r = s / 2 - s / 8, d = s / 8;
    dyn_ui_layout l = uw_l(UW_FIXED(s), UW_FIXED(s));
    int k, lit = (int)fmod(floor(ui->now * 8), 8);
    dyn_ui_open(ui, 0, &l, NULL);
    for (k = 0; k < 8; k++) {
        float a = (float)k * 0.785398163f, x = c + r * cosf(a), y = c + r * sinf(a);
        dyn_ui_vertex v[6] = { { x - d, y - d }, { x + d, y - d }, { x + d, y + d }, { x - d, y - d }, { x + d, y + d }, { x - d, y + d } };
        dyn_ui_triangles(ui, v, 6, k == lit ? t->text : t->text_dim);
    }
    dyn_ui_close(ui);
    dyn_ui_wake_at(ui, (floor(ui->now * 8) + 1) / 8);
}

static uint32_t uw_button_box(dyn_ui_t* ui, dyn_ui_id id, int on, dyn_ui_size w)
{
    const dyn_ui_theme* t = &ui->rt;
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    dyn_ui_layout l = uw_l(w, UW_FIT);
    dyn_ui_style st = uw_s(on ? ((bits & DYN_UI_HOT) ? t->accent_hot : t->accent) : uw_fill(bits, t->button, t->button_hot, t->button_active),
        uw_border(ui, id), t->border_w, t->radius);
    uw_pad(&l, t->pad_x, t->pad_y);
    l.h.min = t->min_target;
    l.align_x = DYN_UI_CENTER;
    l.align_y = DYN_UI_CENTER;
    l.dir = DYN_UI_ROW;
    l.gap = t->pad_x;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    return bits;
}

int dyn_ui_w_button(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = uw_button_box(ui, id, 0, UW_FIT);
    uw_label_fit(ui, s, uw_shown(s, n), ui->rt.text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits);
}

int dyn_ui_w_icon_button(dyn_ui_t* ui, const char* s, size_t n, uint32_t image, float size)
{
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = uw_button_box(ui, id, 0, UW_FIT);
    float z = size > 0 ? size : uw_lh(ui);
    dyn_ui_layout il = uw_l(UW_FIXED(z), UW_FIXED(z));
    dyn_ui_image(ui, 0, image, &il, 0xFFFFFFFFu);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), ui->rt.text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits);
}

int dyn_ui_w_toggle(dyn_ui_t* ui, const char* s, size_t n, int on)
{
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = uw_button_box(ui, id, on, UW_FIT);
    uw_label_fit(ui, s, uw_shown(s, n), on ? ui->rt.on_accent : ui->rt.text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits) ? !on : on;
}

static uint32_t uw_check_row(dyn_ui_t* ui, dyn_ui_id id)
{
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    dyn_ui_layout row = uw_l(UW_FIT, UW_FIT);
    row.dir = DYN_UI_ROW;
    row.gap = ui->rt.pad_x;
    row.align_y = DYN_UI_CENTER;
    row.h.min = ui->rt.min_target;
    row.interactive = 1;
    dyn_ui_open(ui, id, &row, NULL);
    return bits;
}

int dyn_ui_w_checkbox(dyn_ui_t* ui, const char* s, size_t n, int checked)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = uw_check_row(ui, id);
    float z = uw_lh(ui), in = floorf(z / 4);
    dyn_ui_layout bl = uw_l(UW_FIXED(z), UW_FIXED(z));
    dyn_ui_style bs = uw_s(uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, id), t->border_w, t->radius);
    uw_pad(&bl, in, in);
    dyn_ui_open(ui, 0, &bl, &bs);
    if (checked)
        uw_rect(ui, uw_l(UW_GROW(1), UW_GROW(1)), t->accent);
    dyn_ui_close(ui);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits) ? !checked : checked;
}

int dyn_ui_w_radio(dyn_ui_t* ui, const char* id_label, size_t idn, const char* const* items, const size_t* lens, int count, int selected, int horizontal)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, id_label, idn);
    dyn_ui_layout g = uw_l(UW_FIT, UW_FIT);
    float z = uw_lh(ui), in = floorf(z / 4);
    int k, out = selected;
    g.dir = horizontal ? DYN_UI_ROW : DYN_UI_COLUMN;
    g.gap = horizontal ? t->pad_x * 2 : t->spacing;
    dyn_ui_open(ui, gid, &g, NULL);
    for (k = 0; k < count; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        uint32_t bits = uw_check_row(ui, id);
        dyn_ui_layout bl = uw_l(UW_FIXED(z), UW_FIXED(z));
        dyn_ui_style bs = uw_s(uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, id), t->border_w, z / 2);
        uw_pad(&bl, in, in);
        dyn_ui_open(ui, 0, &bl, &bs);
        if (k == selected) {
            dyn_ui_layout dl = uw_l(UW_GROW(1), UW_GROW(1));
            dyn_ui_style ds = uw_s(t->accent, 0, 0, z / 2 - in);
            dyn_ui_open(ui, 0, &dl, &ds);
            dyn_ui_close(ui);
        }
        dyn_ui_close(ui);
        uw_text(ui, items[k], lens[k], t->text);
        dyn_ui_close(ui);
        if (uw_activated(ui, id, bits))
            out = k;
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

int dyn_ui_w_switch(dyn_ui_t* ui, const char* s, size_t n, int on)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = uw_check_row(ui, id);
    float z = uw_lh(ui), k = z - uw_px(ui, 4);
    dyn_ui_layout tl = uw_l(UW_FIXED(z * 2), UW_FIXED(z));
    dyn_ui_style ts = uw_s(on ? ((bits & DYN_UI_HOT) ? t->accent_hot : t->accent) : uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, id),
        t->border_w, z / 2);
    dyn_ui_open(ui, 0, &tl, &ts);
    uw_float_rect(ui, on ? z + uw_px(ui, 2) : uw_px(ui, 2), uw_px(ui, 2), k, k, on ? t->on_accent : t->text_dim, k / 2);
    dyn_ui_close(ui);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits) ? !on : on;
}

int dyn_ui_w_segmented(dyn_ui_t* ui, const char* id_label, size_t idn, const char* const* items, const size_t* lens, int count, int selected)
{
    dyn_ui_id gid = uw_id(ui, id_label, idn);
    dyn_ui_layout g = uw_l(UW_FIT, UW_FIT);
    int k, out = selected;
    g.dir = DYN_UI_ROW;
    dyn_ui_open(ui, gid, &g, NULL);
    for (k = 0; k < count; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        uint32_t bits = uw_button_box(ui, id, k == selected, UW_FIT);
        uw_label_fit(ui, items[k], lens[k], k == selected ? ui->rt.on_accent : ui->rt.text);
        dyn_ui_close(ui);
        if (uw_activated(ui, id, bits))
            out = k;
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

int dyn_ui_w_selectable(dyn_ui_t* ui, const char* s, size_t n, int selected)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style st = uw_s(selected ? t->selection : (bits & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->frame_hot
                                                                                           : 0,
        ui->focus == id && ui->focus_visible ? t->accent : 0,
        t->border_w, t->radius);
    uw_pad(&l, t->pad_x, uw_px(ui, 2));
    l.h.min = t->min_target;
    l.align_y = DYN_UI_CENTER;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_label_fit(ui, s, uw_shown(s, n), t->text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits) ? !selected : selected;
}

int dyn_ui_w_tabs(dyn_ui_t* ui, const char* id_label, size_t idn, const char* const* items, const size_t* lens, int count, int selected)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, id_label, idn);
    dyn_ui_layout g = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style gs = uw_s(0, 0, 0, 0);
    int k, out = selected;
    g.dir = DYN_UI_ROW;
    g.gap = uw_px(ui, 1);
    dyn_ui_open(ui, gid, &g, &gs);
    for (k = 0; k < count; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        uint32_t bits = dyn_ui_interact(ui, id, 1);
        dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
        dyn_ui_style st = uw_s(k == selected ? t->panel : uw_fill(bits, t->title, t->button_hot, t->button_active), ui->focus == id && ui->focus_visible ? t->accent : 0,
            t->border_w, 0);
        uw_pad(&l, t->pad_x * 2, t->pad_y);
        l.h.min = t->min_target;
        l.align_y = DYN_UI_CENTER;
        l.interactive = 1;
        dyn_ui_open(ui, id, &l, &st);
        uw_label_fit(ui, items[k], lens[k], k == selected ? t->text : t->text_dim);
        if (k == selected)
            uw_float_rect(ui, 0, 0, dyn_ui_text_width(ui, t->font, t->font_px, items[k], lens[k]) + t->pad_x * 4, uw_px(ui, 2), t->accent, 0);
        dyn_ui_close(ui);
        if (uw_activated(ui, id, bits))
            out = k;
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

void dyn_ui_w_separator(dyn_ui_t* ui, const char* s, size_t n)
{
    const dyn_ui_theme* t = &ui->rt;
    int in_row = ui->depth > 0 && ui_el_at(ui, ui->stack[ui->depth - 1])->layout.dir == DYN_UI_ROW;
    float bw = t->border_w > 0 ? t->border_w : 1;
    if (!n || in_row) {
        uw_rect(ui, in_row ? uw_l(UW_FIXED(bw), UW_GROW(1)) : uw_l(UW_GROW(1), UW_FIXED(bw)), t->border);
        return;
    }
    {
        dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT);
        row.dir = DYN_UI_ROW;
        row.gap = t->pad_x;
        row.align_y = DYN_UI_CENTER;
        dyn_ui_open(ui, 0, &row, NULL);
        uw_rect(ui, uw_l(UW_FIXED(t->pad_x * 2), UW_FIXED(uw_hair(ui))), t->border);
        uw_text(ui, s, n, t->text_dim);
        uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(uw_hair(ui))), t->border);
        dyn_ui_close(ui);
    }
}

static double uw_snap(double v, double lo, double hi, double step)
{
    if (step > 0 && isfinite(step) && lo <= hi)
        v = lo + round((v - lo) / step) * step;
    if (lo <= hi) {
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
    }
    return v;
}

static float uw_frac(double v, double lo, double hi)
{
    double f;
    if (!(hi > lo) || !isfinite(v))
        return 0;
    f = (v - lo) / (hi - lo);
    return f < 0 ? 0 : f > 1 ? 1
                             : (float)f;
}

static int uw_keys_step(dyn_ui_t* ui, dyn_ui_id id, int vertical)
{
    int d = 0;
    if (ui->focus != id)
        return 0;
    d += ui->fin.key_hit[vertical ? DYN_UI_KEY_UP : DYN_UI_KEY_RIGHT];
    d -= ui->fin.key_hit[vertical ? DYN_UI_KEY_DOWN : DYN_UI_KEY_LEFT];
    return d;
}

static void uw_value_text(dyn_ui_t* ui, double v, int dec, uint32_t color)
{
    struct uw_state* w = uw_get(ui);
    size_t n;
    if (!w)
        return;
    n = uw_fmt(w->scratch, sizeof(w->scratch), v, dec);
    uw_text(ui, w->scratch, n, color);
}

double dyn_ui_w_slider(dyn_ui_t* ui, const char* s, size_t n, double v, double lo, double hi, double step, int dec, int vertical, float len)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE | (vertical ? DYN_UI_DRAG_Y : DYN_UI_DRAG_X));
    dyn_ui_rect tr;
    dyn_ui_layout row = uw_l(vertical || len > 0 ? UW_FIT : UW_GROW(1), UW_FIT), tl, ol;
    dyn_ui_style ts = uw_s(uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, id), t->border_w, t->radius);
    int k;
    float f;
    if ((bits & DYN_UI_ACTIVE) && dyn_ui_box(ui, id, &tr) && hi > lo) {
        float span = (vertical ? tr.h : tr.w) - 2 * t->border_w - uw_px(ui, 10);
        float at = vertical ? tr.y + tr.h - t->border_w - uw_px(ui, 10) / 2 - ui->ptr.y : ui->ptr.x - tr.x - t->border_w - uw_px(ui, 10) / 2;
        float g = span > 0 ? at / span : 0;
        g = g < 0 ? 0 : g > 1 ? 1
                              : g;
        v = uw_snap(lo + (double)g * (hi - lo), lo, hi, step);
    }
    k = uw_keys_step(ui, id, vertical);
    if (k && hi > lo)
        v = uw_snap((isfinite(v) ? v : lo) + k * (step > 0 ? step : (hi - lo) / 100), lo, hi, step);
    if (ui->focus == id && hi > lo) {
        if (ui->fin.key_hit[DYN_UI_KEY_HOME])
            v = lo;
        if (ui->fin.key_hit[DYN_UI_KEY_END])
            v = hi;
    }
    f = uw_frac(v, lo, hi);
    row.dir = vertical ? DYN_UI_COLUMN : DYN_UI_ROW;
    row.gap = t->pad_x;
    row.align_x = row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &row, NULL);
    if (vertical)
        ol = uw_l(UW_FIXED(uw_fh(ui)), UW_FIXED(len > 0 ? len : uw_px(ui, 100)));
    else
        ol = uw_l(len > 0 ? UW_FIXED(len) : UW_GROW(1), UW_FIXED(uw_fh(ui)));
    ol.interactive = 1;
    ol.align_x = ol.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, id, &ol, NULL);
    tl = vertical ? uw_l(UW_FIXED(uw_bar_h(ui)), UW_GROW(1)) : uw_l(UW_GROW(1), UW_FIXED(uw_bar_h(ui)));
    tl.dir = vertical ? DYN_UI_COLUMN : DYN_UI_ROW;
    uw_pad(&tl, t->border_w, t->border_w);
    dyn_ui_open(ui, 0, &tl, &ts);
    if (vertical) {
        uw_rect(ui, uw_l(UW_GROW(1), UW_GROW(1 - f)), 0);
        uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(uw_px(ui, 10))), (bits & DYN_UI_ACTIVE) ? t->accent_hot : t->accent);
        uw_rect(ui, uw_l(UW_GROW(1), UW_GROW(f)), 0);
    } else {
        uw_rect(ui, uw_l(UW_GROW(f), UW_GROW(1)), 0);
        uw_rect(ui, uw_l(UW_FIXED(uw_px(ui, 10)), UW_GROW(1)), (bits & DYN_UI_ACTIVE) ? t->accent_hot : t->accent);
        uw_rect(ui, uw_l(UW_GROW(1 - f), UW_GROW(1)), 0);
    }
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_value_text(ui, v, dec, t->text);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    return v;
}

void dyn_ui_w_range(dyn_ui_t* ui, const char* s, size_t n, double* plo, double* phi, double lo, double hi, double step, int dec)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE | DYN_UI_DRAG_X);
    int* part = dyn_ui_state(ui, uw_sub(id, 1), sizeof(int), NULL);
    dyn_ui_rect tr;
    dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT), tl = uw_l(UW_GROW(1), UW_FIXED(uw_bar_h(ui))), ol = uw_l(UW_GROW(1), UW_FIXED(uw_fh(ui)));
    dyn_ui_style ts = uw_s(uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, id), t->border_w, t->radius);
    double a = *plo, b = *phi;
    float fa, fb;
    if (!part)
        return;
    if (hi > lo && dyn_ui_box(ui, id, &tr) && (bits & DYN_UI_ACTIVE)) {
        float span = tr.w - 2 * t->border_w - 2 * uw_px(ui, 10), g;
        if (bits & DYN_UI_PRESSED) {
            float xa = tr.x + t->border_w + uw_frac(a, lo, hi) * span + uw_px(ui, 10) / 2;
            float xb = tr.x + t->border_w + uw_px(ui, 10) + uw_frac(b, lo, hi) * span + uw_px(ui, 10) / 2;
            *part = fabsf(ui->ptr.x - xb) < fabsf(ui->ptr.x - xa) || (ui->ptr.x > xb) ? 1 : 0;
        }
        g = span > 0 ? (ui->ptr.x - tr.x - t->border_w - uw_px(ui, 10) / 2 - (*part ? uw_px(ui, 10) : 0)) / span : 0;
        g = g < 0 ? 0 : g > 1 ? 1
                              : g;
        if (*part == 0) {
            a = uw_snap(lo + (double)g * (hi - lo), lo, hi, step);
            if (a > b)
                a = b;
        } else {
            b = uw_snap(lo + (double)g * (hi - lo), lo, hi, step);
            if (b < a)
                b = a;
        }
    }
    fa = uw_frac(a, lo, hi);
    fb = uw_frac(b, lo, hi);
    if (fb < fa)
        fb = fa;
    row.dir = DYN_UI_ROW;
    row.gap = t->pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &row, NULL);
    ol.interactive = 1;
    ol.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, id, &ol, NULL);
    tl.dir = DYN_UI_ROW;
    uw_pad(&tl, t->border_w, t->border_w);
    dyn_ui_open(ui, 0, &tl, &ts);
    uw_rect(ui, uw_l(UW_GROW(fa), UW_GROW(1)), 0);
    uw_rect(ui, uw_l(UW_FIXED(uw_px(ui, 10)), UW_GROW(1)), t->accent);
    uw_rect(ui, uw_l(UW_GROW(fb - fa), UW_GROW(1)), t->selection);
    uw_rect(ui, uw_l(UW_FIXED(uw_px(ui, 10)), UW_GROW(1)), t->accent);
    uw_rect(ui, uw_l(UW_GROW(1 - fb), UW_GROW(1)), 0);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_value_text(ui, a, dec, t->text);
    uw_value_text(ui, b, dec, t->text);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    *plo = a;
    *phi = b;
}

typedef struct {
    float x;
    double v;
} uw_drag_state;

double dyn_ui_w_drag(dyn_ui_t* ui, const char* s, size_t n, double v, double speed, double lo, double hi, int dec)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE | DYN_UI_DRAG_X);
    uw_drag_state* d = dyn_ui_state(ui, uw_sub(id, 2), sizeof(uw_drag_state), NULL);
    dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT), bl = uw_l(UW_GROW(1), UW_FIXED(uw_fh(ui)));
    dyn_ui_style bs = uw_s(uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, id), t->border_w, t->radius);
    int k;
    if (!(speed > 0) || !isfinite(speed))
        speed = 1;
    if (d && (bits & DYN_UI_PRESSED)) {
        d->x = ui->ptr.x;
        d->v = isfinite(v) ? v : 0;
    }
    if (d && (bits & DYN_UI_ACTIVE) && !(bits & DYN_UI_PRESSED))
        v = uw_snap(d->v + (double)((ui->ptr.x - d->x) / ui->scale) * speed, lo, hi, 0);
    k = uw_keys_step(ui, id, 0);
    if (k)
        v = uw_snap((isfinite(v) ? v : 0) + k * speed, lo, hi, 0);
    row.dir = DYN_UI_ROW;
    row.gap = t->pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &row, NULL);
    bl.interactive = 1;
    bl.align_x = bl.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, id, &bl, &bs);
    uw_value_text(ui, v, dec, t->text);
    dyn_ui_close(ui);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    return v;
}

double dyn_ui_w_spin(dyn_ui_t* ui, const char* s, size_t n, double v, double lo, double hi, double step, int dec)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n), dn = uw_sub(id, 3), up = uw_sub(id, 4);
    dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT), vl = uw_l(UW_GROW(1), UW_FIXED(uw_fh(ui)));
    dyn_ui_style vs = uw_s(t->frame, t->border, t->border_w, t->radius);
    uint32_t b;
    int k;
    if (!(step > 0) || !isfinite(step))
        step = 1;
    row.dir = DYN_UI_ROW;
    row.gap = uw_px(ui, 2);
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, id, &row, NULL);
    b = uw_button_box(ui, dn, 0, UW_FIT);
    uw_text(ui, "-", 1, t->text);
    dyn_ui_close(ui);
    if (uw_activated(ui, dn, b))
        v = uw_snap((isfinite(v) ? v : 0) - step, lo, hi, 0);
    vl.align_x = vl.align_y = DYN_UI_CENTER;
    k = uw_keys_step(ui, dn, 0) + uw_keys_step(ui, up, 0);
    if (k)
        v = uw_snap((isfinite(v) ? v : 0) + k * step, lo, hi, 0);
    dyn_ui_open(ui, 0, &vl, &vs);
    uw_value_text(ui, v, dec, t->text);
    dyn_ui_close(ui);
    b = uw_button_box(ui, up, 0, UW_FIT);
    uw_text(ui, "+", 1, t->text);
    dyn_ui_close(ui);
    if (uw_activated(ui, up, b))
        v = uw_snap((isfinite(v) ? v : 0) + step, lo, hi, 0);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    ui->last_id = id;
    return v;
}

static int uw_load(uw_str* b, const char* s, size_t n)
{
    size_t i = 0;
    int keep = b->max;
    b->n = 0;
    b->max = 0;
    while (i < n) {
        int cp = (int)ui_utf8(s, n, &i);
        if (!uw_str_insert(b, b->n, &cp, 1)) {
            b->max = keep;
            return -1;
        }
    }
    b->max = keep;
    return 0;
}

static int uw_out(struct uw_state* w, const uw_str* b)
{
    size_t need = (size_t)b->n * 4 + 1, k = 0;
    int i;
    if (need > w->out_cap) {
        char* o = realloc(w->out, need);
        if (!o)
            return -1;
        w->out = o;
        w->out_cap = need;
    }
    for (i = 0; i < b->n; i++) {
        int c = b->s[i];
        k += uw_utf8_put(w->out + k, c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : (uint32_t)c);
    }
    w->out[k] = 0;
    w->out_n = k;
    return 0;
}

static int uw_remember(struct uw_state* w, const char* s, size_t n)
{
    if (n + 1 > w->last_cap) {
        char* p = realloc(w->last, n + 1);
        if (!p)
            return -1;
        w->last = p;
        w->last_cap = n + 1;
    }
    if (n)
        memcpy(w->last, s, n);
    w->last[n] = 0;
    w->last_n = n;
    return 0;
}

static void uw_copy_selection(dyn_ui_t* ui, struct uw_state* w)
{
    int a = w->st.select_start < w->st.select_end ? w->st.select_start : w->st.select_end;
    int z = w->st.select_start < w->st.select_end ? w->st.select_end : w->st.select_start, i;
    size_t k = 0;
    char* o;
    if (a == z || a < 0 || z > w->buf.n)
        return;
    o = malloc((size_t)(z - a) * 4 + 1);
    if (!o)
        return;
    for (i = a; i < z; i++)
        k += uw_utf8_put(o + k, (uint32_t)w->buf.s[i]);
    o[k] = 0;
    if (ui->clip_set)
        ui->clip_set(ui->clip_ud, o, k);
    else {
        free(ui->clip);
        ui->clip = o;
        ui->clip_n = k;
        return;
    }
    free(o);
}

static void uw_paste(dyn_ui_t* ui, struct uw_state* w)
{
    size_t n = 0, i = 0;
    const char* s = ui->clip_get ? ui->clip_get(ui->clip_ud, &n) : ui->clip;
    int* cps;
    int k = 0;
    if (!ui->clip_get)
        n = ui->clip_n;
    if (!s || !n || n > (1u << 24))
        return;
    cps = malloc(n * sizeof(int));
    if (!cps)
        return;
    while (i < n) {
        int c = (int)ui_utf8(s, n, &i);
        if (c == '\r')
            continue;
        if (w->single && (c == '\n' || c == '\t'))
            c = ' ';
        if (c < 0x20 && c != '\n' && c != '\t')
            continue;
        cps[k++] = c;
    }
    if (k)
        stb_textedit_paste(&w->buf, &w->st, cps, k);
    free(cps);
}

static void uw_edit_input(dyn_ui_t* ui, struct uw_state* w, int flags, int* events)
{
    const ui_event* ev = ui->ev_frame.p;
    size_t i;
    int ro = flags & DYN_UI_EDIT_READONLY;
    for (i = 0; i < ui->ev_frame.n && ui->focus == w->edit_id; i++) {
        int m = ev[i].mods, cmd = m & (DYN_UI_MOD_CTRL | DYN_UI_MOD_SUPER), sh = (m & DYN_UI_MOD_SHIFT) ? STB_TEXTEDIT_K_SHIFT : 0;
        int word = m & (DYN_UI_MOD_CTRL | DYN_UI_MOD_ALT), k = 0;
        if (ev[i].kind == 2) {
            uint32_t c = ev[i].cp;
            if (ro || c < 0x20 || c == 0x7F || cmd)
                continue;
            stb_textedit_key(&w->buf, &w->st, (int)c);
            continue;
        }
        switch (ev[i].key) {
        case DYN_UI_KEY_LEFT:
            k = word ? STB_TEXTEDIT_K_WORDLEFT : STB_TEXTEDIT_K_LEFT;
            break;
        case DYN_UI_KEY_RIGHT:
            k = word ? STB_TEXTEDIT_K_WORDRIGHT : STB_TEXTEDIT_K_RIGHT;
            break;
        case DYN_UI_KEY_UP:
            k = (flags & UW_EDIT_KEYLIST) ? 0 : STB_TEXTEDIT_K_UP;
            break;
        case DYN_UI_KEY_DOWN:
            k = (flags & UW_EDIT_KEYLIST) ? 0 : STB_TEXTEDIT_K_DOWN;
            break;
        case DYN_UI_KEY_HOME:
            k = cmd ? STB_TEXTEDIT_K_TEXTSTART : STB_TEXTEDIT_K_LINESTART;
            break;
        case DYN_UI_KEY_END:
            k = cmd ? STB_TEXTEDIT_K_TEXTEND : STB_TEXTEDIT_K_LINEEND;
            break;
        case DYN_UI_KEY_PAGEUP:
            k = STB_TEXTEDIT_K_PGUP;
            break;
        case DYN_UI_KEY_PAGEDOWN:
            k = STB_TEXTEDIT_K_PGDOWN;
            break;
        case DYN_UI_KEY_BACKSPACE:
            k = ro ? 0 : STB_TEXTEDIT_K_BACKSPACE;
            break;
        case DYN_UI_KEY_DELETE:
            k = ro ? 0 : STB_TEXTEDIT_K_DELETE;
            break;
        case DYN_UI_KEY_RETURN:
            if (w->single)
                *events |= DYN_UI_EDIT_SUBMITTED;
            else if (!ro)
                k = '\n';
            sh = 0;
            break;
        case DYN_UI_KEY_ESCAPE:
            ui->focus = 0;
            break;
        case DYN_UI_KEY_A:
            if (cmd) {
                w->st.select_start = 0;
                w->st.select_end = w->buf.n;
                w->st.cursor = w->buf.n;
            }
            break;
        case DYN_UI_KEY_C:
            if (cmd && !(flags & DYN_UI_EDIT_PASSWORD))
                uw_copy_selection(ui, w);
            break;
        case DYN_UI_KEY_X:
            if (cmd && !ro && !(flags & DYN_UI_EDIT_PASSWORD)) {
                uw_copy_selection(ui, w);
                stb_textedit_cut(&w->buf, &w->st);
            }
            break;
        case DYN_UI_KEY_V:
            if (cmd && !ro)
                uw_paste(ui, w);
            break;
        case DYN_UI_KEY_Z:
            if (cmd && !ro)
                k = (m & DYN_UI_MOD_SHIFT) ? STB_TEXTEDIT_K_REDO : STB_TEXTEDIT_K_UNDO;
            sh = 0;
            break;
        case DYN_UI_KEY_Y:
            if (cmd && !ro)
                k = STB_TEXTEDIT_K_REDO;
            break;
        default:
            break;
        }
        if (k)
            stb_textedit_key(&w->buf, &w->st, k | sh);
    }
}

static float uw_prefix_w(dyn_ui_t* ui, const uw_str* b, int from, int to, int password)
{
    float x = 0;
    int i;
    for (i = from; i < to && i < b->n; i++)
        x += uw_cp_width(ui, password ? '*' : b->s[i]);
    return x;
}

static void uw_line_of(const uw_str* b, int at, int* start, int* row)
{
    int i, r = 0, s = 0;
    for (i = 0; i < at && i < b->n; i++)
        if (b->s[i] == '\n') {
            r++;
            s = i + 1;
        }
    *start = s;
    *row = r;
}

static const char* uw_edit(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, int flags, int max_chars, const char* hint, size_t hn, float rows,
    float width, size_t* out_n, int* events, int labelled)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* w = uw_get(ui);
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE | ((flags & DYN_UI_EDIT_MULTILINE) ? 0 : DYN_UI_DRAG_X));
    int multi = (flags & DYN_UI_EDIT_MULTILINE) != 0, pw = (flags & DYN_UI_EDIT_PASSWORD) != 0;
    float lh = uw_lh(ui), fh = multi ? (rows > 0 ? rows : 4) * lh + 2 * t->pad_y : w && w->edit_h > 0 ? w->edit_h
                                                                                                      : uw_fh(ui);
    float* sc = dyn_ui_state(ui, uw_sub(id, 5), 2 * sizeof(float), NULL);
    dyn_ui_rect box;
    dyn_ui_layout fl = uw_l(width > 0 ? UW_FIXED(width) : UW_GROW(1), UW_FIXED(fh)), tl = uw_l(UW_FIT, UW_FIT);
    int focused, have_box;
    dyn_ui_style fs;
    const char* shown = text;
    size_t shown_n = tn;
    *events = 0;
    *out_n = tn;
    if (!w || !sc)
        return text;
    focused = ui->focus == id;
    if (!focused && w->edit_id == id)
        w->edit_id = 0;
    if (focused && (w->edit_id != id || tn != w->last_n || (tn && memcmp(text, w->last, tn)))) {
        int fresh = w->edit_id != id;
        w->buf.max = max_chars > 0 ? max_chars : 0;
        w->single = !multi;
        if (uw_load(&w->buf, text, tn) || uw_remember(w, text, tn)) {
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for the text being edited");
            return text;
        }
        if (fresh) {
            stb_textedit_initialize_state(&w->st, !multi);
            w->st.cursor = w->buf.n;
        }
        stb_textedit_clamp(&w->buf, &w->st);
        w->edit_id = id;
    }
    w->single = !multi;
    have_box = dyn_ui_box(ui, id, &box);
    if (focused && have_box) {
        float lx = ui->ptr.x - box.x - t->pad_x + sc[0], ly = ui->ptr.y - box.y - t->pad_y + sc[1];
        if (bits & DYN_UI_PRESSED) {
            stb_textedit_click(&w->buf, &w->st, lx, ly);
            if (bits & DYN_UI_DOUBLE) {
                w->st.select_start = 0;
                w->st.select_end = w->st.cursor = w->buf.n;
            }
        } else if (bits & DYN_UI_ACTIVE)
            stb_textedit_drag(&w->buf, &w->st, lx, ly);
    }
    if (focused) {
        uw_edit_input(ui, w, flags, events);
        if (uw_out(w, &w->buf)) {
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for the text being edited");
            return text;
        }
        if (w->out_n != tn || (tn && memcmp(w->out, text, tn)))
            *events |= DYN_UI_EDIT_CHANGED;
        if (uw_remember(w, w->out, w->out_n))
            return text;
        *events |= ui->focus == id ? DYN_UI_EDIT_FOCUSED : 0;
        *out_n = w->out_n;
        shown = w->out;
        shown_n = w->out_n;
    }
    fs = uw_s(focused ? t->frame_active : uw_fill(bits, t->frame, t->frame_hot, t->frame_active), w->field_error ? t->error : focused ? t->accent
                                                                                                                                      : t->border,
        t->border_w, t->radius);
    fl.clip_x = fl.clip_y = 1;
    fl.interactive = 1;
    if (!(width > 0))
        fl.w.min = roundf(10 * lh);
    labelled = labelled && uw_shown(s, n) > 0;
    if (labelled) {
        dyn_ui_layout row = uw_l(width > 0 ? UW_FIT : UW_GROW(1), UW_FIT);
        row.dir = DYN_UI_ROW;
        row.gap = t->pad_x;
        row.align_y = multi ? DYN_UI_START : DYN_UI_CENTER;
        dyn_ui_open(ui, 0, &row, NULL);
    }
    dyn_ui_open(ui, id, &fl, &fs);
    if (focused && have_box) {
        int start, row;
        float cx, cy, vw = box.w - 2 * t->pad_x, vh = box.h - 2 * t->pad_y;
        uw_line_of(&w->buf, w->st.cursor, &start, &row);
        cx = uw_prefix_w(ui, &w->buf, start, w->st.cursor, pw);
        cy = (float)row * lh;
        if (cx - sc[0] > vw - 1)
            sc[0] = cx - vw + 1;
        if (cx < sc[0])
            sc[0] = cx;
        if (cy + lh - sc[1] > vh)
            sc[1] = cy + lh - vh;
        if (cy < sc[1])
            sc[1] = cy;
        if (sc[0] < 0)
            sc[0] = 0;
        if (sc[1] < 0)
            sc[1] = 0;
        ui->edit_rect.x = box.x + t->pad_x - sc[0] + cx;
        ui->edit_rect.y = box.y + t->pad_y - sc[1] + cy;
        ui->edit_rect.w = 1;
        ui->edit_rect.h = lh;
    } else if (!focused) {
        sc[0] = sc[1] = 0;
    }
    tl.floating = DYN_UI_FLOAT_PARENT;
    tl.float_x = t->pad_x - sc[0];
    tl.float_y = t->pad_y - sc[1];
    tl.dir = DYN_UI_COLUMN;
    dyn_ui_open(ui, 0, &tl, NULL);
    if (focused && w->st.select_start != w->st.select_end) {
        int a = w->st.select_start < w->st.select_end ? w->st.select_start : w->st.select_end;
        int z = w->st.select_start < w->st.select_end ? w->st.select_end : w->st.select_start, i = a;
        while (i < z) {
            int st, row, e = i;
            uw_line_of(&w->buf, i, &st, &row);
            while (e < z && w->buf.s[e] != '\n')
                e++;
            uw_float_rect(ui, uw_prefix_w(ui, &w->buf, st, i, pw), (float)row * lh,
                uw_prefix_w(ui, &w->buf, i, e, pw) + (e < z ? uw_cp_width(ui, ' ') : 0), lh, t->selection, 0);
            i = e + 1;
        }
    }
    if (shown_n == 0 && hn && !focused)
        uw_text(ui, hint, hn, t->text_dim);
    else if (pw) {
        size_t i = 0, cps = 0;
        char* stars;
        while (i < shown_n) {
            ui_utf8(shown, shown_n, &i);
            cps++;
        }
        stars = malloc(cps + 1);
        if (stars) {
            memset(stars, '*', cps);
            uw_text(ui, stars, cps, t->text);
            free(stars);
        }
    } else
        uw_text(ui, shown, shown_n, t->text);
    if (focused) {
        int start, row;
        float cx;
        uw_line_of(&w->buf, w->st.cursor, &start, &row);
        cx = uw_prefix_w(ui, &w->buf, start, w->st.cursor, pw);
        ui->editing = 1;
        if (ui->ncompose) {
            float cw = dyn_ui_text_width(ui, t->font, t->font_px, ui->compose, ui->ncompose);
            dyn_ui_layout cl = uw_l(UW_FIT, UW_FIT);
            dyn_ui_style cs = uw_s(t->frame_active, 0, 0, 0);
            cl.floating = DYN_UI_FLOAT_PARENT;
            cl.float_x = cx;
            cl.float_y = (float)row * lh;
            dyn_ui_open(ui, 0, &cl, &cs);
            uw_text(ui, ui->compose, ui->ncompose, t->text);
            uw_float_rect(ui, 0, lh - uw_hair(ui), cw, uw_hair(ui), t->text, 0);
            dyn_ui_close(ui);
            cx += cw;
        }
        uw_float_rect(ui, cx, (float)row * lh, 1, lh, t->text, 0);
    }
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    if (labelled) {
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
        dyn_ui_close(ui);
    }
    ui->last_id = id;
    return shown;
}

const char* dyn_ui_w_edit(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, int flags, int max_chars, const char* hint, size_t hn, float rows,
    float width, size_t* out_n, int* events)
{
    return uw_edit(ui, s, n, text, tn, flags, max_chars, hint, hn, rows, width, out_n, events, 1);
}

const char* dyn_ui_w_search(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, const char* hint, size_t hn, float width, size_t* out_n,
    int* events)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = dyn_ui_id_of(ui, s, n), cid = uw_sub(id, 21);
    dyn_ui_layout row = uw_l(width > 0 ? UW_FIXED(width) : UW_GROW(1), UW_FIT);
    const char* out;
    row.dir = DYN_UI_ROW;
    row.gap = uw_px(ui, 2);
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &row, NULL);
    out = uw_edit(ui, s, n, text, tn, 0, 0, hint, hn, 0, 0, out_n, events, 0);
    if (*out_n) {
        uint32_t b = dyn_ui_interact(ui, cid, 0);
        dyn_ui_layout cl = uw_l(UW_FIXED(uw_fh(ui)), UW_FIXED(uw_fh(ui)));
        dyn_ui_style cs = uw_s(uw_fill(b, t->button, t->button_hot, t->button_active), t->border, t->border_w, t->radius);
        uw_pad(&cl, t->pad_y, t->pad_y);
        cl.interactive = 1;
        dyn_ui_open(ui, cid, &cl, &cs);
        uw_cross(ui, t->text);
        dyn_ui_close(ui);
        if (b & DYN_UI_CLICKED) {
            struct uw_state* w = ui->w;
            if (w && w->edit_id == id) {
                w->buf.n = 0;
                stb_textedit_clamp(&w->buf, &w->st);
                uw_remember(w, "", 0);
            }
            *out_n = 0;
            out = "";
            *events |= DYN_UI_EDIT_CHANGED;
        }
    }
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    ui->last_id = id;
    return out;
}

static void uw_arrow(dyn_ui_t* ui, int dir, uint32_t color)
{
    float z = uw_lh(ui), a = z * 0.25f, b = z * 0.75f, m = z * 0.5f;
    dyn_ui_layout l = uw_l(UW_FIXED(z), UW_FIXED(z));
    dyn_ui_vertex v[3];
    if (dir == 0) {
        v[0].x = a, v[0].y = a;
        v[1].x = b, v[1].y = a;
        v[2].x = m, v[2].y = b;
    } else if (dir == 3) {
        v[0].x = a, v[0].y = b;
        v[1].x = b, v[1].y = b;
        v[2].x = m, v[2].y = a;
    } else if (dir == 2) {
        v[0].x = b, v[0].y = a;
        v[1].x = b, v[1].y = b;
        v[2].x = a, v[2].y = m;
    } else {
        v[0].x = a, v[0].y = a;
        v[1].x = b, v[1].y = m;
        v[2].x = a, v[2].y = b;
    }
    dyn_ui_open(ui, 0, &l, NULL);
    dyn_ui_triangles(ui, v, 3, color);
    dyn_ui_close(ui);
}

static int uw_popup_depth(dyn_ui_t* ui)
{
    return ui->w ? ui->w->popup_depth : 0;
}

static void uw_set_popup_depth(dyn_ui_t* ui, int d)
{
    struct uw_state* w = uw_get(ui);
    if (w)
        w->popup_depth = d;
}

static void uw_popup_open_box(dyn_ui_t* ui, dyn_ui_id pid, dyn_ui_layout* l, int level)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_style st = uw_s(t->popup, t->border, t->border_w, t->radius);
    l->z = UW_Z_POPUP + level;
    l->dir = DYN_UI_COLUMN;
    uw_pad(l, t->border_w, t->border_w);
    dyn_ui_open(ui, pid, l, &st);
    uw_set_popup_depth(ui, level + 1);
}

typedef struct {
    int hi, was_open, qn;
    char q[116];
} uw_combo_st;

static int uw_match(const char* s, size_t n, const char* q, size_t qn)
{
    size_t i, k;
    if (!qn)
        return 1;
    for (i = 0; i + qn <= n; i++) {
        for (k = 0; k < qn; k++) {
            unsigned char a = (unsigned char)s[i + k], b = (unsigned char)q[k];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + 32);
            if (a != b)
                break;
        }
        if (k == qn)
            return 1;
    }
    return 0;
}

static int uw_list_keys(dyn_ui_t* ui, int hi, int count, const char* const* items, const size_t* lens, const char* q, size_t qn)
{
    int d = ui->fin.key_hit[DYN_UI_KEY_DOWN] - ui->fin.key_hit[DYN_UI_KEY_UP], k = hi;
    while (d && count > 0) {
        int step = d > 0 ? 1 : -1, j = k;
        do
            j += step;
        while (j >= 0 && j < count && !uw_match(items[j], lens[j], q, qn));
        if (j < 0 || j >= count)
            break;
        k = j;
        d -= step;
    }
    if (k >= 0 && k < count && !uw_match(items[k], lens[k], q, qn))
        k = -1;
    return k;
}

int dyn_ui_w_combo_ex(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count, int selected, int search)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n), pid = uw_sub(id, 6);
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    float widest = 0, lh = uw_lh(ui), wbox;
    int k, out = selected, depth = uw_popup_depth(ui), open, fresh = 0, was;
    dyn_ui_layout row = uw_l(UW_FIT, UW_FIT), bl;
    dyn_ui_style bs = uw_s(uw_fill(bits, t->button, t->button_hot, t->button_active), uw_border(ui, id), t->border_w, t->radius);
    uw_combo_st* cs = dyn_ui_state(ui, uw_sub(id, 26), sizeof(uw_combo_st), &fresh);
    dyn_ui_id qid;
    if (!cs)
        return selected;
    if (fresh)
        cs->hi = -1;
    dyn_ui_push_id(ui, pid);
    qid = dyn_ui_id_of(ui, "##\x01q", 4);
    dyn_ui_pop_id(ui);
    for (k = 0; k < count; k++) {
        float w = dyn_ui_text_width(ui, t->font, t->font_px, items[k], lens[k]);
        if (w > widest)
            widest = w;
    }
    wbox = widest + 3 * t->pad_x + lh + 2 * t->border_w;
    if (search && wbox < roundf(10 * lh))
        wbox = roundf(10 * lh);
    open = dyn_ui_popup_level(ui, pid) >= 0;
    if (uw_commit(ui, bits)) {
        if (!dyn_ui_popup_was_open(ui, pid))
            dyn_ui_open_popup(ui, pid, depth);
    } else if (ui->focus == id && ui->focus_visible && ui->fin.key_hit[DYN_UI_KEY_RETURN] && !(open && cs->hi >= 0 && cs->hi < count)) {
        if (open)
            dyn_ui_close_popups(ui, depth);
        else
            dyn_ui_open_popup(ui, pid, depth);
    }
    open = dyn_ui_popup_level(ui, pid) >= 0;
    was = cs->was_open;
    if (open && !was) {
        cs->hi = selected;
        cs->qn = 0;
        if (search) {
            ui->focus = qid;
            ui->focus_visible = 1;
        }
    }
    if (open && was && (ui->focus == id || ui->focus == qid)) {
        cs->hi = uw_list_keys(ui, cs->hi, count, items, lens, cs->q, (size_t)cs->qn);
        if (ui->fin.key_hit[DYN_UI_KEY_RETURN] && cs->hi >= 0 && cs->hi < count) {
            out = cs->hi;
            dyn_ui_close_popups(ui, depth);
            ui->focus = id;
            open = 0;
        }
    }
    if (!open && ui->focus == qid)
        ui->focus = id;
    cs->was_open = open;
    row.dir = DYN_UI_ROW;
    row.gap = t->pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &row, NULL);
    bl = uw_l(UW_FIXED(wbox), UW_FIXED(uw_fh(ui)));
    bl.dir = DYN_UI_ROW;
    bl.align_y = DYN_UI_CENTER;
    bl.interactive = 1;
    uw_pad(&bl, t->pad_x, 0);
    bl.gap = t->pad_x;
    dyn_ui_open(ui, id, &bl, &bs);
    if (selected >= 0 && selected < count)
        uw_label_fit(ui, items[selected], lens[selected], t->text);
    uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(1)), 0);
    uw_arrow(ui, 0, t->text_dim);
    if (open) {
        dyn_ui_layout pl = uw_l(UW_FIXED(wbox), UW_FIT);
        int ferr = ui->w ? ui->w->field_error : 0;
        pl.floating = DYN_UI_FLOAT_POPUP;
        pl.anchor = DYN_UI_ANCHOR_BOTTOM_LEFT;
        if (count > 12) {
            pl.h = UW_FIXED(12 * (lh + 4) + 2 * t->border_w + (search ? uw_fh(ui) : 0));
            pl.clip_y = 1;
        }
        uw_popup_open_box(ui, pid, &pl, depth);
        if (ui->w)
            ui->w->field_error = 0;
        if (search) {
            size_t qn = 0;
            int ev = 0;
            const char* q;
            dyn_ui_push_id(ui, pid);
            q = uw_edit(ui, "##\x01q", 4, cs->q, (size_t)cs->qn, UW_EDIT_KEYLIST, (int)sizeof(cs->q) - 1, "Search", 6, 1, wbox - 2 * t->border_w, &qn, &ev, 0);
            dyn_ui_pop_id(ui);
            if (qn >= sizeof(cs->q))
                qn = sizeof(cs->q) - 1;
            if (q != cs->q)
                memmove(cs->q, q, qn);
            if ((int)qn != cs->qn) {
                cs->qn = (int)qn;
                if (cs->hi >= 0 && cs->hi < count && !uw_match(items[cs->hi], lens[cs->hi], cs->q, qn))
                    cs->hi = uw_list_keys(ui, -1, count, items, lens, cs->q, qn) < 0 ? -1 : cs->hi;
            }
            cs->qn = (int)qn;
        }
        for (k = 0; k < count; k++) {
            dyn_ui_id iid = uw_sub(pid, (uint32_t)k);
            uint32_t ib;
            dyn_ui_layout il = uw_l(UW_GROW(1), UW_FIT);
            dyn_ui_style is;
            if (!uw_match(items[k], lens[k], cs->q, (size_t)cs->qn))
                continue;
            ib = dyn_ui_interact(ui, iid, 0);
            is = uw_s(k == selected ? t->selection : (ib & DYN_UI_HOT) || k == cs->hi ? t->button_hot
                                                                                      : 0,
                0, 0, 0);
            uw_pad(&il, t->pad_x, uw_px(ui, 2));
            il.h.min = t->min_target;
            il.align_y = DYN_UI_CENTER;
            il.interactive = 1;
            dyn_ui_open(ui, iid, &il, &is);
            uw_text(ui, items[k], lens[k], t->text);
            dyn_ui_close(ui);
            if (ib & DYN_UI_CLICKED) {
                out = k;
                dyn_ui_close_popups(ui, depth);
                if (ui->focus == qid)
                    ui->focus = id;
            }
        }
        dyn_ui_close(ui);
        if (ui->w)
            ui->w->field_error = ferr;
        uw_set_popup_depth(ui, depth);
    }
    dyn_ui_close(ui);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    uw_items(ui, id, pid, 0);
    return out;
}

int dyn_ui_w_combo(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count, int selected)
{
    return dyn_ui_w_combo_ex(ui, s, n, items, lens, count, selected, 0);
}

static float uw_row_h(dyn_ui_t* ui)
{
    float h = uw_lh(ui) + uw_px(ui, 4);
    return h > ui->rt.min_target ? h : ui->rt.min_target;
}

static void uw_scroll_to_row(dyn_ui_t* ui, dyn_ui_id id, int row, float rh, float view)
{
    dyn_ui_scroll_view v;
    float y;
    if (dyn_ui_scroll_info(ui, id, &v))
        return;
    y = v.y;
    if ((float)row * rh < y)
        y = (float)row * rh;
    if ((float)(row + 1) * rh > y + view)
        y = (float)(row + 1) * rh - view;
    dyn_ui_scroll(ui, id, NULL, &y);
}

static void uw_visible_rows(dyn_ui_t* ui, dyn_ui_id id, int count, float rh, float view, int* first, int* last)
{
    dyn_ui_scroll_view v;
    float y = dyn_ui_scroll_info(ui, id, &v) ? 0 : v.y, top = (float)count * rh - view;
    int f, l;
    if (y > top)
        y = top;
    if (y < 0)
        y = 0;
    f = (int)floorf(y / rh);
    l = (int)ceilf((y + view) / rh);
    if (f < 0)
        f = 0;
    if (l > count)
        l = count;
    if (f > l)
        f = l;
    *first = f;
    *last = l;
}

int dyn_ui_w_list(dyn_ui_t* ui, const char* s, size_t n, int count, dyn_ui_item_fn item, void* ud, int selected, int visible, int selectable, int follow,
    int* activated)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, selectable);
    float rh = uw_row_h(ui), view = (float)(visible > 0 ? visible : 8) * rh, maxc = UI_MAX_COORD / 2;
    dyn_ui_layout bl = uw_l(UW_GROW(1), UW_FIXED(view + 2 * t->border_w));
    dyn_ui_style bs = uw_s(t->frame, uw_border(ui, id), t->border_w, t->radius);
    int first, last, k, out = selected;
    dyn_ui_scroll_view sv;
    *activated = -1;
    if (count < 0)
        count = 0;
    if ((float)count * rh > maxc)
        count = (int)(maxc / rh);
    if (selectable && ui->focus == id && count > 0) {
        int d = ui->fin.key_hit[DYN_UI_KEY_DOWN] - ui->fin.key_hit[DYN_UI_KEY_UP];
        d += (ui->fin.key_hit[DYN_UI_KEY_PAGEDOWN] - ui->fin.key_hit[DYN_UI_KEY_PAGEUP]) * (visible > 0 ? visible : 8);
        if (d) {
            out = (out < 0 ? (d > 0 ? -1 : count) : out) + d;
            out = out < 0 ? 0 : out >= count ? count - 1
                                             : out;
            uw_scroll_to_row(ui, id, out, rh, view);
        }
        if (ui->fin.key_hit[DYN_UI_KEY_RETURN] && out >= 0)
            *activated = out;
    }
    if (follow && dyn_ui_scroll_info(ui, id, &sv) == 0 && sv.y >= sv.content_h - sv.view_h - 1) {
        float y = (float)count * rh;
        dyn_ui_scroll(ui, id, NULL, &y);
    }
    uw_visible_rows(ui, id, count, rh, view, &first, &last);
    bl.dir = DYN_UI_COLUMN;
    bl.clip_y = 1;
    bl.interactive = 1;
    uw_pad(&bl, t->border_w, t->border_w);
    dyn_ui_open(ui, id, &bl, &bs);
    if (first > 0)
        uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED((float)first * rh)), 0);
    for (k = first; k < last; k++) {
        size_t tn = 0;
        const char* txt = item(ud, k, &tn);
        dyn_ui_id rid = uw_sub(id, 0x100 + (uint32_t)k);
        uint32_t rb = selectable ? dyn_ui_interact(ui, rid, 0) : 0;
        dyn_ui_layout rl = uw_l(UW_GROW(1), UW_FIXED(rh));
        dyn_ui_style rs = uw_s(k == out ? t->selection : (rb & DYN_UI_HOT) ? t->frame_hot
                                                                           : 0,
            0, 0, 0);
        if (!txt)
            break;
        rl.pad_l = t->pad_x;
        rl.align_y = DYN_UI_CENTER;
        rl.clip_x = 1;
        rl.interactive = (uint8_t)selectable;
        dyn_ui_open(ui, rid, &rl, &rs);
        uw_label_fit(ui, txt, tn, t->text);
        dyn_ui_close(ui);
        if (uw_commit(ui, rb)) {
            out = k;
            ui->focus = id;
        }
        if (rb & DYN_UI_CONTEXT) {
            out = k;
            ui->context_id = id;
        }
        if (rb & DYN_UI_DOUBLE)
            *activated = k;
    }
    if (last < count)
        uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED((float)(count - last) * rh)), 0);
    dyn_ui_close(ui);
    (void)bits;
    uw_items(ui, id, id, 0x100);
    return out;
}

uint32_t dyn_ui_w_color(dyn_ui_t* ui, const char* s, size_t n, uint32_t rgba)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    dyn_ui_layout col = uw_l(UW_GROW(1), UW_FIT), top = uw_l(UW_GROW(1), UW_FIT), sw = uw_l(UW_FIXED(uw_fh(ui) * 2), UW_FIXED(uw_fh(ui)));
    dyn_ui_style ss = uw_s(rgba, t->border, t->border_w, t->radius);
    static const char* const names[4] = { "R", "G", "B", "A" };
    struct uw_state* w = uw_get(ui);
    int k;
    uint32_t out = 0;
    if (!w)
        return rgba;
    col.dir = DYN_UI_COLUMN;
    col.gap = t->spacing;
    top.dir = DYN_UI_ROW;
    top.gap = t->pad_x;
    top.align_y = DYN_UI_CENTER;
    dyn_ui_push_id(ui, id);
    dyn_ui_open(ui, id, &col, NULL);
    dyn_ui_open(ui, 0, &top, NULL);
    dyn_ui_open(ui, 0, &sw, &ss);
    dyn_ui_close(ui);
    {
        static const char hex[] = "0123456789ABCDEF";
        char hx[9];
        int i;
        for (i = 0; i < 8; i++)
            hx[i] = hex[(rgba >> (28 - 4 * i)) & 0xF];
        hx[8] = 0;
        uw_text(ui, "#", 1, t->text_dim);
        uw_text(ui, hx, 8, t->text);
    }
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    for (k = 0; k < 4; k++) {
        double c = (double)((rgba >> (24 - 8 * k)) & 0xFFu);
        c = dyn_ui_w_slider(ui, names[k], 1, c, 0, 255, 1, 0, 0, 0);
        out |= (uint32_t)c << (24 - 8 * k);
    }
    dyn_ui_close(ui);
    dyn_ui_pop_id(ui);
    ui->last_id = id;
    return out;
}

static void uw_follow_col(dyn_ui_t* ui, dyn_ui_id id, const float* cw, int col)
{
    dyn_ui_scroll_view v;
    float x0 = 0, x;
    int c;
    if (dyn_ui_scroll_info(ui, id, &v))
        return;
    for (c = 0; c < col; c++)
        x0 += cw[c];
    x = v.x;
    if (x0 < x)
        x = x0;
    if (x0 + cw[col] > x + v.view_w)
        x = x0 + cw[col] - v.view_w;
    dyn_ui_scroll(ui, id, &x, NULL);
}

typedef struct {
    int editing, pending, pend_row, pend_col;
} uw_table_edit_st;

int dyn_ui_w_table_ex(dyn_ui_t* ui, const char* s, size_t n, const char* const* heads, const size_t* hlens, const float* widths, int ncols, int rows,
    dyn_ui_cell_fn cell, void* ud, int visible, dyn_ui_table_state* tst)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* uws = uw_get(ui);
    dyn_ui_id id = uw_id(ui, s, n), bid = uw_sub(id, 7), eid;
    float total_w = 0, rh = uw_row_h(ui), view = (float)(visible > 0 ? visible : 10) * rh, maxc = UI_MAX_COORD / 2, *cw;
    int fresh = 0, c, k, first, last, out = tst->selected, oc = tst->column, cells = tst->cells, er, ec;
    uw_table_edit_st* es;
    dyn_ui_layout wrap = uw_l(UW_GROW(1), UW_FIT), head = uw_l(UW_FIT, UW_FIT), body;
    dyn_ui_style ws = uw_s(t->frame, t->border, t->border_w, t->radius), hs = uw_s(t->title, 0, 0, 0);
    tst->activated = 0;
    tst->committed = tst->cancelled = 0;
    tst->edit_out = NULL;
    tst->edit_out_n = 0;
    if (ncols <= 0 || ncols > 256 || !uws)
        return tst->selected;
    cw = dyn_ui_state(ui, uw_sub(id, 8), (size_t)ncols * sizeof(float), &fresh);
    es = dyn_ui_state(ui, uw_sub(id, 9), sizeof(uw_table_edit_st), NULL);
    if (!cw || !es)
        return tst->selected;
    if (fresh)
        for (c = 0; c < ncols; c++)
            cw[c] = widths && widths[c] > 0 ? widths[c] : uw_px(ui, 100);
    if (rows < 0)
        rows = 0;
    if ((float)rows * rh > maxc)
        rows = (int)(maxc / rh);
    if (es->pending && !tst->edit) {
        out = es->pend_row;
        oc = es->pend_col;
        es->pending = 0;
    }
    if (out >= rows)
        out = -1;
    if (cells && (oc >= ncols || out < 0))
        oc = -1;
    er = out;
    ec = oc;
    if (ui->focus == bid && rows > 0) {
        int d = ui->fin.key_hit[DYN_UI_KEY_DOWN] - ui->fin.key_hit[DYN_UI_KEY_UP];
        d += (ui->fin.key_hit[DYN_UI_KEY_PAGEDOWN] - ui->fin.key_hit[DYN_UI_KEY_PAGEUP]) * (visible > 0 ? visible : 10);
        if (d) {
            out = (out < 0 ? (d > 0 ? -1 : rows) : out) + d;
            out = out < 0 ? 0 : out >= rows ? rows - 1
                                            : out;
            uw_scroll_to_row(ui, bid, out, rh, view);
            if (cells && oc < 0)
                oc = 0;
        }
        if (cells) {
            int dc = ui->fin.key_hit[DYN_UI_KEY_RIGHT] - ui->fin.key_hit[DYN_UI_KEY_LEFT], nc = oc;
            int hit = ui->fin.key_hit[DYN_UI_KEY_HOME] || ui->fin.key_hit[DYN_UI_KEY_END] || dc;
            if (ui->fin.key_hit[DYN_UI_KEY_HOME])
                nc = 0;
            else if (ui->fin.key_hit[DYN_UI_KEY_END])
                nc = ncols - 1;
            else if (dc)
                nc = nc < 0 ? (dc > 0 ? 0 : ncols - 1) : nc + dc;
            nc = nc < 0 ? 0 : nc >= ncols ? ncols - 1
                                          : nc;
            if (hit && nc != oc) {
                oc = nc;
                if (out < 0)
                    out = 0;
                uw_follow_col(ui, id, cw, oc);
            }
        }
        if (ui->fin.key_hit[DYN_UI_KEY_RETURN] && out >= 0 && !(cells && tst->edit))
            tst->activated = 1;
    }
    for (c = 0; c < ncols; c++)
        total_w += cw[c];
    wrap.dir = DYN_UI_COLUMN;
    wrap.clip_x = 1;
    uw_pad(&wrap, t->border_w, t->border_w);
    dyn_ui_open(ui, id, &wrap, &ws);
    head.dir = DYN_UI_ROW;
    head.w = UW_GROW(1);
    head.w.min = total_w;
    dyn_ui_open(ui, 0, &head, &hs);
    for (c = 0; c < ncols; c++) {
        dyn_ui_id hid = uw_sub(id, 0x10000 + (uint32_t)c), gid = uw_sub(id, 0x20000 + (uint32_t)c);
        uint32_t hb = dyn_ui_interact(ui, hid, 0), gb = dyn_ui_interact(ui, gid, DYN_UI_DRAG_X);
        uw_drag_state* d = dyn_ui_state(ui, uw_sub(gid, 1), sizeof(uw_drag_state), NULL);
        dyn_ui_layout hl = uw_l(UW_FIXED(cw[c]), UW_FIXED(rh));
        dyn_ui_style hcs = uw_s((hb & DYN_UI_HOT) ? t->button_hot : 0, 0, 0, 0);
        if (d && (gb & DYN_UI_PRESSED)) {
            d->x = ui->ptr.x;
            d->v = (double)cw[c];
        }
        if (d && (gb & DYN_UI_ACTIVE) && !(gb & DYN_UI_PRESSED)) {
            cw[c] = (float)d->v + ui->ptr.x - d->x;
            if (cw[c] < uw_px(ui, 20))
                cw[c] = uw_px(ui, 20);
            if (cw[c] > uw_px(ui, 4000))
                cw[c] = uw_px(ui, 4000);
            hl.w = UW_FIXED(cw[c]);
        }
        if (hb & DYN_UI_CLICKED) {
            if (tst->sort_col == c)
                tst->sort_desc = !tst->sort_desc;
            else {
                tst->sort_col = c;
                tst->sort_desc = 0;
            }
        }
        hl.dir = DYN_UI_ROW;
        hl.pad_l = t->pad_x;
        hl.align_y = DYN_UI_CENTER;
        hl.gap = t->pad_x;
        hl.clip_x = 1;
        hl.interactive = 1;
        dyn_ui_open(ui, hid, &hl, &hcs);
        uw_text(ui, heads[c], hlens[c], t->text);
        if (tst->sort_col == c)
            uw_arrow(ui, tst->sort_desc ? 0 : 1, t->text_dim);
        {
            dyn_ui_layout gl = uw_l(UW_FIXED(uw_px(ui, 5)), UW_FIXED(rh));
            dyn_ui_style gs = uw_s((gb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->accent : t->border, 0, 0, 0);
            gl.floating = DYN_UI_FLOAT_PARENT;
            gl.float_x = cw[c] - uw_px(ui, 5);
            gl.interactive = 1;
            gl.w = UW_FIXED((gb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? 5 : 1);
            gl.float_x = cw[c] - ((gb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? 5 : 1);
            dyn_ui_open(ui, gid, &gl, &gs);
            dyn_ui_close(ui);
        }
        dyn_ui_close(ui);
    }
    dyn_ui_close(ui);
    uw_visible_rows(ui, bid, rows, rh, view, &first, &last);
    body = uw_l(UW_GROW(1), UW_FIXED(view));
    body.w.min = total_w;
    body.dir = DYN_UI_COLUMN;
    body.clip_y = 1;
    body.interactive = 1;
    dyn_ui_interact(ui, bid, 1);
    dyn_ui_open(ui, bid, &body, NULL);
    if (first > 0)
        uw_rect(ui, uw_l(UW_FIXED(1), UW_FIXED((float)first * rh)), 0);
    eid = 0;
    for (k = first; k < last; k++) {
        dyn_ui_id rid = uw_sub(id, 0x40000000u + (uint32_t)k);
        uint32_t rb = dyn_ui_interact(ui, rid, 0);
        dyn_ui_layout rl = uw_l(UW_GROW(1), UW_FIXED(rh));
        dyn_ui_style rs = uw_s(k == out ? t->selection : (rb & DYN_UI_HOT) ? t->frame_hot
                : (k & 1)                                                  ? t->panel
                                                                           : 0,
            0, 0, 0);
        rl.dir = DYN_UI_ROW;
        rl.interactive = 1;
        dyn_ui_open(ui, rid, &rl, &rs);
        for (c = 0; c < ncols; c++) {
            size_t tn = 0;
            const char* txt = cell(ud, k, c, &tn);
            dyn_ui_id cid = cells ? uw_sub(rid, (uint32_t)c) : 0;
            dyn_ui_layout cl = uw_l(UW_FIXED(cw[c]), UW_FIXED(rh));
            uint32_t cb = cells ? dyn_ui_interact(ui, cid, 0) : 0;
            int on = cells && (tst->edit ? k == er && c == ec : k == out && c == oc);
            dyn_ui_style cs = uw_s(0, on ? t->accent : 0, on ? fmaxf(t->border_w, uw_px(ui, 2)) : 0, 0);
            if (!txt) {
                k = last;
                break;
            }
            cl.pad_l = t->pad_x;
            cl.align_y = DYN_UI_CENTER;
            cl.clip_x = 1;
            cl.interactive = (uint8_t)cells;
            if (on && tst->edit) {
                size_t on_n = 0;
                int ev = 0;
                const char* r;
                cl.pad_l = 0;
                dyn_ui_open(ui, cid, &cl, &cs);
                dyn_ui_push_id(ui, id);
                eid = dyn_ui_id_of(ui, "##\x01"
                                       "cell",
                    6);
                if (!es->editing) {
                    ui->focus = eid;
                    ui->focus_visible = 1;
                }
                if (ui->focus == eid && ui->fin.key_hit[DYN_UI_KEY_ESCAPE])
                    tst->cancelled = 1;
                uws->edit_h = rh;
                r = uw_edit(ui, "##\x01"
                                "cell",
                    6, tst->edit, tst->edit_n, 0, 0, NULL, 0, 1, cw[c], &on_n, &ev, 0);
                uws->edit_h = 0;
                dyn_ui_pop_id(ui);
                dyn_ui_close(ui);
                tst->edit_out = r;
                tst->edit_out_n = on_n;
                if (ev & DYN_UI_EDIT_SUBMITTED)
                    tst->committed = 1;
                if (tst->committed || tst->cancelled) {
                    ui->focus = bid;
                    ui->focus_visible = 1;
                }
            } else {
                dyn_ui_open(ui, cid, &cl, cells ? &cs : NULL);
                uw_label_fit(ui, txt, tn, t->text);
                dyn_ui_close(ui);
            }
            if (cells && !(on && tst->edit) && uw_commit(ui, cb) && k < rows) {
                out = k;
                oc = c;
                ui->focus = bid;
            }
            if (cells && (cb & DYN_UI_DOUBLE) && k < rows)
                tst->activated = 1;
        }
        dyn_ui_close(ui);
        if (uw_commit(ui, rb)) {
            out = k < rows ? k : out;
            ui->focus = bid;
        }
        if ((rb & DYN_UI_DOUBLE) && k < rows && k == out)
            tst->activated = 1;
        if ((rb & DYN_UI_CONTEXT) && k < rows) {
            out = k;
            ui->context_id = id;
        }
    }
    if (!eid && cells && tst->edit) {
        dyn_ui_push_id(ui, id);
        eid = dyn_ui_id_of(ui, "##\x01"
                               "cell",
            6);
        dyn_ui_pop_id(ui);
        if (er < 0 || ec < 0)
            tst->cancelled = 1;
        else if (es->editing && (ui->focus != eid || out != er || oc != ec)) {
            tst->committed = 1;
            tst->edit_out = tst->edit;
            tst->edit_out_n = tst->edit_n;
        } else if (!es->editing)
            eid = 0;
    }
    if (eid && es->editing && !tst->committed && !tst->cancelled && (ui->focus != eid || out != er || oc != ec))
        tst->committed = 1;
    if (eid && tst->committed && (out != er || oc != ec)) {
        es->pending = 1;
        es->pend_row = out;
        es->pend_col = oc;
        out = er;
        oc = ec;
    }
    es->editing = eid != 0 && !tst->committed && !tst->cancelled;
    if (last < rows)
        uw_rect(ui, uw_l(UW_FIXED(1), UW_FIXED((float)(rows - last) * rh)), 0);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    tst->selected = out;
    tst->column = cells ? oc : -1;
    uw_items(ui, id, id, 0x40000000u);
    memcpy(ui->last_cols, cw, (size_t)ncols * sizeof(float));
    ui->last_cols_n = ncols;
    return out;
}

int dyn_ui_w_table(dyn_ui_t* ui, const char* s, size_t n, const char* const* heads, const size_t* hlens, const float* widths, int ncols, int rows,
    dyn_ui_cell_fn cell, void* ud, int visible, int selected, int* sort_col, int* sort_desc)
{
    dyn_ui_table_state st;
    memset(&st, 0, sizeof(st));
    st.selected = selected;
    st.column = -1;
    st.sort_col = sort_col ? *sort_col : -1;
    st.sort_desc = sort_desc ? *sort_desc : 0;
    dyn_ui_w_table_ex(ui, s, n, heads, hlens, widths, ncols, rows, cell, ud, visible, &st);
    if (sort_col)
        *sort_col = st.sort_col;
    if (sort_desc)
        *sort_desc = st.sort_desc;
    return st.selected;
}

void dyn_ui_w_plot(dyn_ui_t* ui, const char* s, size_t n, const double* v, int count, double lo, double hi, float h, int bars)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    dyn_ui_layout bl = uw_l(UW_GROW(1), UW_FIXED(h > 0 ? h : 80));
    dyn_ui_style bs = uw_s(t->frame, t->border, t->border_w, t->radius);
    int k;
    if (!(lo < hi)) {
        lo = (double)INFINITY;
        hi = -(double)INFINITY;
        for (k = 0; k < count; k++)
            if (isfinite(v[k])) {
                if (v[k] < lo)
                    lo = v[k];
                if (v[k] > hi)
                    hi = v[k];
            }
        if (bars && lo > 0)
            lo = 0;
        if (!(lo < hi)) {
            lo = isfinite(lo) ? lo - 1 : 0;
            hi = lo + 2;
        }
    }
    bl.dir = DYN_UI_ROW;
    bl.gap = bars ? 1 : 0;
    uw_pad(&bl, t->border_w + uw_px(ui, 2), t->border_w + uw_px(ui, 2));
    dyn_ui_open(ui, id, &bl, &bs);
    if (bars) {
        for (k = 0; k < count && k < 4096; k++) {
            dyn_ui_layout cl = uw_l(UW_GROW(1), UW_GROW(1));
            float f = uw_frac(v[k], lo, hi);
            cl.dir = DYN_UI_COLUMN;
            cl.align_y = DYN_UI_END;
            dyn_ui_open(ui, 0, &cl, NULL);
            if (f > 0)
                uw_rect(ui, uw_l(UW_GROW(1), UW_PCT(f)), t->accent);
            dyn_ui_close(ui);
        }
    } else if (count >= 2) {
        dyn_ui_vertex* pts = ui_polyline_points(ui, (uint32_t)count, t->accent, 1.5f * ui->scale);
        if (pts)
            for (k = 0; k < count; k++) {
                pts[k].x = ui_finite((float)k / (float)(count - 1), 0, 1);
                pts[k].y = ui_finite(1 - uw_frac(v[k], lo, hi), 0, 1);
            }
    }
    dyn_ui_close(ui);
}

void dyn_ui_w_property_begin(dyn_ui_t* ui, const char* s, size_t n)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT), name = uw_l(UW_FIXED(t->label_w), UW_FIT), val = uw_l(UW_GROW(1), UW_FIT);
    row.dir = DYN_UI_ROW;
    row.gap = t->pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, uw_sub(dyn_ui_id_of(ui, s, n), 22), &row, NULL);
    uw_block_push(ui, uw_sub(dyn_ui_id_of(ui, s, n), 22));
    name.clip_x = 1;
    dyn_ui_open(ui, 0, &name, NULL);
    uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    val.dir = DYN_UI_COLUMN;
    val.gap = t->spacing;
    dyn_ui_push_id(ui, dyn_ui_id_of(ui, s, n));
    dyn_ui_open(ui, 0, &val, NULL);
}

void dyn_ui_w_property_end(dyn_ui_t* ui)
{
    dyn_ui_close(ui);
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

#define UW_SCREEN_ID 0x5C3EE700u

int dyn_ui_w_begin(dyn_ui_t* ui, float width, float height)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* w;
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_GROW(1));
    dyn_ui_style s = uw_s(t->bg, 0, 0, 0);
    int rc = dyn_ui_begin(ui, width, height);
    if (rc)
        return rc;
    w = uw_get(ui);
    if (!w)
        return DYN_UI_ERR_NOMEM;
    w->popup_depth = 0;
    w->nblocks = 0;
    w->field_error = 0;
    w->dock_t = w->dock_b = 0;
    l.dir = DYN_UI_COLUMN;
    l.pad_l = t->window_pad + ui->insets[0];
    l.pad_t = t->window_pad + ui->insets[1];
    l.pad_r = t->window_pad + ui->insets[2];
    l.pad_b = t->window_pad + ui->insets[3];
    l.gap = t->spacing;
    l.clip_y = 1;
    dyn_ui_open(ui, UW_SCREEN_ID, &l, &s);
    w->screen = (int32_t)(ui->els.n - 1);
    return DYN_UI_OK;
}

static void uw_toasts(dyn_ui_t* ui);

int dyn_ui_w_end(dyn_ui_t* ui, int* changed)
{
    if (ui->frame_open && ui->depth == 2) {
        uw_toasts(ui);
        if (ui->w)
            ui->w->dock_b_last = ui->w->dock_b;
        dyn_ui_close(ui);
    } else if (ui->frame_open)
        ui_fail(ui, DYN_UI_ERR_NESTING, "end() with widgets still open");
    return dyn_ui_end(ui, changed);
}

static int uw_at_screen(dyn_ui_t* ui)
{
    return ui->w && ui->w->screen >= 0 && ui->depth == 2 && ui->stack[1] == ui->w->screen;
}

static void uw_dock(dyn_ui_t* ui, dyn_ui_layout* l, float h, int bottom)
{
    if (uw_at_screen(ui)) {
        ui_el* sc = ui_el_at(ui, ui->w->screen);
        if (bottom)
            sc->layout.pad_b += h;
        else
            sc->layout.pad_t += h;
        int outer = bottom ? ui->w->dock_b == 0 : ui->w->dock_t == 0;
        float ext = outer ? ui->insets[bottom ? 3 : 1] : 0;
        l->floating = DYN_UI_FLOAT_ROOT;
        l->float_x = 0;
        l->float_y = bottom ? ui->height - ui->insets[3] - ui->w->dock_b - h : ui->insets[1] + ui->w->dock_t - ext;
        l->h = UW_FIXED(h + ext);
        if (bottom)
            l->pad_b = ext;
        else
            l->pad_t = ext;
        if (bottom)
            ui->w->dock_b += h;
        else
            ui->w->dock_t += h;
        l->w = UW_FIXED(ui->width);
        l->pad_l = ui->insets[0];
        l->pad_r = ui->insets[2];
        l->z = UW_Z_WINDOW - 2;
    }
}

void dyn_ui_w_spacer(dyn_ui_t* ui, float w, float h)
{
    uw_rect(ui, uw_l(w > 0 ? UW_FIXED(w) : UW_GROW(1), h > 0 ? UW_FIXED(h) : UW_GROW(1)), 0);
}

typedef struct {
    float x, y, w, h, dx, dy;
    uint32_t stamp;
    int collapsed;
} uw_win;

int dyn_ui_w_window_begin(dyn_ui_t* ui, const char* s, size_t n, float x, float y, float width, float height, int flags, int* closed,
    dyn_ui_window_frame* fr)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* ws = uw_get(ui);
    dyn_ui_id id = uw_id(ui, s, n), tid = uw_sub(id, 10), cid = uw_sub(id, 12);
    int fresh = 0, front;
    uw_win* st = dyn_ui_state(ui, id, sizeof(uw_win), &fresh);
    uint32_t tb, cb;
    float fh = uw_fh(ui), lh = uw_lh(ui);
    dyn_ui_layout wl, tl, bl;
    dyn_ui_style wsl, tsl;
    *closed = 0;
    fr->id = id;
    fr->flags = flags;
    fr->visible = 0;
    fr->depth = ws ? ws->popup_depth : 0;
    if (!st || !ws)
        return 0;
    if (fresh) {
        st->x = x;
        st->y = y;
        st->w = width > 0 ? width : 300;
        st->h = height > 0 ? height : 200;
        st->stamp = ++ws->window_stamp;
    }
    if ((ui->fin.pressed & 7u) && ui->hot_root == id && st->stamp != ws->window_stamp)
        st->stamp = ++ws->window_stamp;
    front = st->stamp == ws->window_stamp;
    tb = dyn_ui_interact(ui, tid, (flags & DYN_UI_WIN_FIXED) ? 0 : DYN_UI_DRAG_X | DYN_UI_DRAG_Y);
    if (!(flags & DYN_UI_WIN_FIXED)) {
        if (tb & DYN_UI_PRESSED) {
            st->dx = ui->ptr.x - st->x;
            st->dy = ui->ptr.y - st->y;
        } else if (tb & DYN_UI_ACTIVE) {
            st->x = ui->ptr.x - st->dx;
            st->y = ui->ptr.y - st->dy;
        }
    }
    if ((flags & DYN_UI_WIN_COLLAPSIBLE) && (tb & DYN_UI_DOUBLE))
        st->collapsed = !st->collapsed;
    if (st->w > ui->width - ui->insets[0] - ui->insets[2])
        st->w = ui->width - ui->insets[0] - ui->insets[2];
    if (st->h > ui->height - ui->insets[1] - ui->insets[3])
        st->h = ui->height - ui->insets[1] - ui->insets[3];
    if (st->x > ui->width - uw_px(ui, 40))
        st->x = ui->width - uw_px(ui, 40);
    if (st->x < 40 - st->w)
        st->x = 40 - st->w;
    if (st->y > ui->height - fh)
        st->y = ui->height - fh;
    if (st->y < 0)
        st->y = 0;
    if (flags & DYN_UI_WIN_RESIZABLE) {
        dyn_ui_id gid = uw_sub(id, 11);
        uint32_t gb = dyn_ui_interact(ui, gid, DYN_UI_DRAG_X | DYN_UI_DRAG_Y);
        if (gb & DYN_UI_PRESSED) {
            st->dx = st->x + st->w - ui->ptr.x;
            st->dy = st->y + st->h - ui->ptr.y;
        } else if (gb & DYN_UI_ACTIVE) {
            st->w = ui->ptr.x + st->dx - st->x;
            st->h = ui->ptr.y + st->dy - st->y;
        }
        if (st->w < uw_px(ui, 80))
            st->w = uw_px(ui, 80);
        if (st->h < fh * 2)
            st->h = fh * 2;
    }
    wl = uw_l(UW_FIXED(st->w), st->collapsed ? UW_FIT : UW_FIXED(st->h));
    wl.floating = DYN_UI_FLOAT_SAFE;
    wl.float_x = st->x;
    wl.float_y = st->y;
    wl.z = UW_Z_WINDOW + (int32_t)(st->stamp & 0x0FFFFFFFu);
    wl.dir = DYN_UI_COLUMN;
    wl.interactive = 1;
    uw_pad(&wl, t->border_w, t->border_w);
    wsl = uw_s(t->panel, front ? t->accent : t->border, t->border_w, t->radius);
    dyn_ui_open(ui, id, &wl, &wsl);
    tl = uw_l(UW_GROW(1), UW_FIXED(fh));
    tl.dir = DYN_UI_ROW;
    tl.align_y = DYN_UI_CENTER;
    tl.gap = t->pad_x;
    tl.pad_l = t->pad_x;
    tl.pad_r = t->pad_y;
    tl.interactive = 1;
    tl.clip_x = 1;
    tsl = uw_s(front ? t->title_active : t->title, 0, 0, 0);
    dyn_ui_open(ui, tid, &tl, &tsl);
    uw_label_fit(ui, s, uw_shown(s, n), t->text);
    uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(1)), 0);
    if (flags & DYN_UI_WIN_CLOSABLE) {
        dyn_ui_layout cl = uw_l(UW_FIXED(lh), UW_FIXED(lh));
        dyn_ui_style cs;
        cb = dyn_ui_interact(ui, cid, 0);
        cs = uw_s((cb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->button_hot : 0, 0, 0, t->radius);
        cl.interactive = 1;
        dyn_ui_open(ui, cid, &cl, &cs);
        uw_cross(ui, t->text);
        dyn_ui_close(ui);
        if (cb & DYN_UI_CLICKED)
            *closed = 1;
    }
    dyn_ui_close(ui);
    if (!st->collapsed) {
        bl = uw_l(UW_GROW(1), UW_GROW(1));
        bl.dir = DYN_UI_COLUMN;
        bl.clip_x = bl.clip_y = 1;
        uw_pad(&bl, t->window_pad, t->window_pad);
        bl.gap = t->spacing;
        dyn_ui_open(ui, uw_sub(id, 13), &bl, NULL);
        fr->visible = 1;
    }
    ws->popup_depth = 0;
    return fr->visible;
}

void dyn_ui_w_window_end(dyn_ui_t* ui, const dyn_ui_window_frame* fr)
{
    const dyn_ui_theme* t = &ui->rt;
    uw_win* st = dyn_ui_state(ui, fr->id, sizeof(uw_win), NULL);
    if (fr->visible)
        dyn_ui_close(ui);
    if ((fr->flags & DYN_UI_WIN_RESIZABLE) && st && !st->collapsed) {
        dyn_ui_id gid = uw_sub(fr->id, 11);
        uint32_t gb = dyn_ui_interact(ui, gid, DYN_UI_DRAG_X | DYN_UI_DRAG_Y);
        float g1 = uw_px(ui, 1), g10 = uw_px(ui, 10);
        dyn_ui_vertex v[3] = { { st->w - 2 * t->border_w - g1, st->h - 2 * t->border_w - g10 }, { st->w - 2 * t->border_w - g1, st->h - 2 * t->border_w - g1 },
            { st->w - 2 * t->border_w - g10, st->h - 2 * t->border_w - g1 } };
        dyn_ui_layout gl = uw_l(UW_FIXED(uw_px(ui, 12)), UW_FIXED(uw_px(ui, 12)));
        gl.floating = DYN_UI_FLOAT_PARENT;
        gl.float_x = st->w - uw_px(ui, 12);
        gl.float_y = st->h - uw_px(ui, 12);
        gl.interactive = 1;
        dyn_ui_triangles(ui, v, 3, (gb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->accent : t->border);
        dyn_ui_open(ui, gid, &gl, NULL);
        dyn_ui_close(ui);
    }
    dyn_ui_close(ui);
    if (ui->w)
        ui->w->popup_depth = fr->depth;
    ui->last_id = fr->id;
}

void dyn_ui_w_panel_begin(dyn_ui_t* ui, const char* s, size_t n, float w, float h, int border)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    dyn_ui_layout l = uw_l(w > 0 ? UW_FIXED(w) : UW_GROW(1), h > 0 ? UW_FIXED(h) : UW_GROW(1));
    dyn_ui_style st = uw_s(border ? t->panel : 0, border ? t->border : 0, border ? t->border_w : 0, t->radius);
    l.dir = DYN_UI_COLUMN;
    l.clip_x = l.clip_y = 1;
    uw_pad(&l, border ? t->window_pad : 0, border ? t->window_pad : 0);
    l.gap = t->spacing;
    dyn_ui_open(ui, id, &l, &st);
    uw_block_push(ui, id);
    dyn_ui_push_id(ui, id);
}

void dyn_ui_w_panel_end(dyn_ui_t* ui)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

void dyn_ui_w_group_begin(dyn_ui_t* ui, const char* s, size_t n)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style st = uw_s(0, t->border, t->border_w, t->radius);
    l.dir = DYN_UI_COLUMN;
    uw_pad(&l, t->window_pad, t->window_pad);
    l.gap = t->spacing;
    dyn_ui_id gid = dyn_ui_id_of(ui, s, n);
    dyn_ui_open(ui, uw_sub(gid, 22), &l, &st);
    uw_block_push(ui, uw_sub(gid, 22));
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_push_id(ui, gid);
}

void dyn_ui_w_group_end(dyn_ui_t* ui)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

static int uw_open_state(dyn_ui_t* ui, dyn_ui_id id, int default_open, int toggle)
{
    int fresh = 0;
    int* o = dyn_ui_state(ui, uw_sub(id, 16), sizeof(int), &fresh);
    if (!o)
        return default_open;
    if (fresh)
        *o = default_open ? 1 : 0;
    if (toggle)
        *o = !*o;
    return *o;
}

int dyn_ui_w_collapsing(dyn_ui_t* ui, const char* s, size_t n, int default_open)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    int open = uw_open_state(ui, id, default_open, uw_activated(ui, id, bits));
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIXED(uw_fh(ui)));
    dyn_ui_style st = uw_s(uw_fill(bits, t->button, t->button_hot, t->button_active), uw_border(ui, id), ui->focus == id && ui->focus_visible ? t->border_w : 0, t->radius);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.pad_l = t->pad_x / 2;
    l.gap = t->pad_x / 2;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_arrow(ui, open ? 0 : 1, t->text);
    uw_label_fit(ui, s, uw_shown(s, n), t->text);
    dyn_ui_close(ui);
    return open;
}

int dyn_ui_w_tree_begin(dyn_ui_t* ui, const char* s, size_t n, int default_open, int leaf)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    int open = leaf ? 0 : uw_open_state(ui, id, default_open, uw_activated(ui, id, bits));
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT), cl = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style st = uw_s((bits & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->frame_hot : 0, ui->focus == id && ui->focus_visible ? t->accent : 0, t->border_w, 0);
    float lh = uw_lh(ui);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.pad_t = l.pad_b = uw_px(ui, 2);
    l.gap = uw_px(ui, 2);
    l.h.min = t->min_target;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    if (leaf)
        uw_rect(ui, uw_l(UW_FIXED(lh), UW_FIXED(lh)), 0);
    else
        uw_arrow(ui, open ? 0 : 1, t->text_dim);
    uw_label_fit(ui, s, uw_shown(s, n), t->text);
    dyn_ui_close(ui);
    if (!open)
        return 0;
    cl.dir = DYN_UI_COLUMN;
    cl.pad_l = t->indent;
    cl.gap = 0;
    dyn_ui_open(ui, 0, &cl, NULL);
    uw_block_push(ui, id);
    dyn_ui_push_id(ui, id);
    return 1;
}

void dyn_ui_w_tree_end(dyn_ui_t* ui)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

typedef struct {
    float x, y;
} uw_at;

void dyn_ui_w_open_popup(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_id pid = dyn_ui_id_of(ui, s, n);
    uw_at* at = dyn_ui_state(ui, uw_sub(pid, 17), sizeof(uw_at), NULL);
    if (at) {
        at->x = ui->ptr.x;
        at->y = ui->ptr.y;
    }
    dyn_ui_open_popup(ui, pid, uw_popup_depth(ui));
}

int dyn_ui_w_popup_begin(dyn_ui_t* ui, const char* s, size_t n, int* depth)
{
    dyn_ui_id pid = dyn_ui_id_of(ui, s, n);
    int level = dyn_ui_popup_level(ui, pid);
    uw_at* at;
    dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
    *depth = uw_popup_depth(ui);
    if (level < 0)
        return 0;
    at = dyn_ui_state(ui, uw_sub(pid, 17), sizeof(uw_at), NULL);
    l.floating = DYN_UI_FLOAT_SAFE;
    l.float_x = at ? at->x : 0;
    l.float_y = at ? at->y : 0;
    l.w.min = uw_px(ui, 80);
    uw_popup_open_box(ui, pid, &l, level);
    l.pad_l = 0;
    {
        dyn_ui_layout inner = uw_l(UW_GROW(1), UW_FIT);
        inner.dir = DYN_UI_COLUMN;
        inner.gap = ui->rt.spacing;
        uw_pad(&inner, ui->rt.pad_x, ui->rt.pad_y);
        dyn_ui_open(ui, 0, &inner, NULL);
    }
    uw_block_push(ui, pid);
    dyn_ui_push_id(ui, pid);
    return 1;
}

void dyn_ui_w_popup_end(dyn_ui_t* ui, int depth)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_set_popup_depth(ui, depth);
    uw_block_pop(ui);
}

int dyn_ui_w_context_begin(dyn_ui_t* ui, const char* s, size_t n, int* depth)
{
    dyn_ui_id target = ui->last_id;
    if (target && ui->context_id == target)
        dyn_ui_w_open_popup(ui, s, n);
    return dyn_ui_w_popup_begin(ui, s, n, depth);
}

void dyn_ui_w_close_popup(dyn_ui_t* ui)
{
    int d = uw_popup_depth(ui);
    dyn_ui_close_popups(ui, d > 0 ? d - 1 : 0);
}

void dyn_ui_w_open_modal(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_open_popup(ui, uw_overlay_id(s, n), uw_popup_depth(ui));
}

int dyn_ui_w_modal_begin(dyn_ui_t* ui, const char* s, size_t n, float width, int* depth)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id pid = uw_overlay_id(s, n);
    int level = dyn_ui_popup_level(ui, pid);
    dyn_ui_layout sh = uw_l(UW_GROW(1), UW_GROW(1)), dl = uw_l(width > 0 ? UW_FIXED(width) : UW_FIT, UW_FIT);
    dyn_ui_style shs = uw_s(t->shade, 0, 0, 0), ds = uw_s(t->popup, t->border, t->border_w, t->radius);
    *depth = uw_popup_depth(ui);
    if (level < 0)
        return 0;
    sh.floating = DYN_UI_FLOAT_ROOT;
    sh.z = UW_Z_MODAL + level;
    sh.dir = DYN_UI_COLUMN;
    sh.align_x = sh.align_y = DYN_UI_CENTER;
    sh.pad_l = ui->insets[0];
    sh.pad_t = ui->insets[1];
    sh.pad_r = ui->insets[2];
    sh.pad_b = ui->insets[3];
    dyn_ui_open(ui, pid, &sh, &shs);
    dl.dir = DYN_UI_COLUMN;
    uw_pad(&dl, t->window_pad, t->window_pad);
    dl.gap = t->spacing;
    dl.interactive = 1;
    dyn_ui_open(ui, uw_sub(pid, 1), &dl, &ds);
    if (uw_shown(s, n)) {
        uw_text(ui, s, uw_shown(s, n), t->text);
        uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(uw_hair(ui))), t->border);
    }
    uw_set_popup_depth(ui, level + 1);
    uw_block_push(ui, uw_sub(pid, 1));
    dyn_ui_push_id(ui, pid);
    return 1;
}

void dyn_ui_w_modal_end(dyn_ui_t* ui, int depth)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_set_popup_depth(ui, depth);
    uw_block_pop(ui);
}

typedef struct {
    double since;
    int on;
} uw_hover;

void dyn_ui_w_tooltip(dyn_ui_t* ui, const char* s, size_t n, double delay)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id target = ui->last_id;
    uw_hover* hv;
    if (!target)
        return;
    hv = dyn_ui_state(ui, uw_sub(target, 18), sizeof(uw_hover), NULL);
    if (!hv)
        return;
    if (!(delay >= 0) || !isfinite(delay))
        delay = 0.5;
    if (ui->ptr.kind != DYN_UI_POINTER_MOUSE) {
        if (!(ui->long_fired && ui->pressing && !ui->drag_scroll && ui->hot == target)) {
            hv->on = 0;
            return;
        }
    } else {
        if (ui->hot != target || ui->active) {
            hv->on = 0;
            return;
        }
        if (!hv->on) {
            hv->on = 1;
            hv->since = ui->now;
        }
        if (ui->now - hv->since < delay) {
            dyn_ui_wake_at(ui, hv->since + delay);
            return;
        }
    }
    {
        dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
        dyn_ui_style st = uw_s(t->tooltip, t->border, t->border_w, t->radius);
        l.floating = DYN_UI_FLOAT_SAFE;
        l.float_x = ui->ptr.x + uw_px(ui, 12);
        l.float_y = ui->ptr.y + uw_px(ui, 16);
        l.z = UW_Z_TOOLTIP;
        l.w.max = uw_px(ui, 320);
        uw_pad(&l, t->pad_x, t->pad_y);
        dyn_ui_open(ui, 0, &l, &st);
        dyn_ui_text(ui, 0, s, n, t->font, t->font_px, t->text, 1, DYN_UI_START);
        dyn_ui_close(ui);
    }
}

float dyn_ui_w_split_begin(dyn_ui_t* ui, const char* s, size_t n, float ratio, int vertical, dyn_ui_split_frame* fr)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n), hid = uw_sub(id, 14);
    uint32_t hb = dyn_ui_interact(ui, hid, vertical ? DYN_UI_DRAG_Y : DYN_UI_DRAG_X);
    dyn_ui_rect box;
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_GROW(1)), a;
    if (!(ratio >= 0.05f))
        ratio = ratio == ratio && ratio < 0.05f ? 0.05f : 0.5f;
    if (ratio > 0.95f)
        ratio = 0.95f;
    if ((hb & DYN_UI_ACTIVE) && dyn_ui_box(ui, id, &box)) {
        float span = vertical ? box.h : box.w, at = vertical ? ui->ptr.y - box.y : ui->ptr.x - box.x;
        if (span > 0)
            ratio = (at - uw_px(ui, 2)) / span;
        ratio = ratio < 0.05f ? 0.05f : ratio > 0.95f ? 0.95f
                                                      : ratio;
    }
    fr->id = id;
    fr->vertical = vertical;
    l.dir = vertical ? DYN_UI_COLUMN : DYN_UI_ROW;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, NULL);
    a = vertical ? uw_l(UW_GROW(1), UW_PCT(ratio)) : uw_l(UW_PCT(ratio), UW_GROW(1));
    a.dir = DYN_UI_COLUMN;
    a.clip_x = a.clip_y = 1;
    a.gap = t->spacing;
    dyn_ui_open(ui, uw_sub(id, 19), &a, NULL);
    uw_block_push(ui, id);
    dyn_ui_push_id(ui, id);
    return ratio;
}

void dyn_ui_w_split_next(dyn_ui_t* ui, const dyn_ui_split_frame* fr)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id hid = uw_sub(fr->id, 14);
    uint32_t hb = dyn_ui_interact(ui, hid, fr->vertical ? DYN_UI_DRAG_Y : DYN_UI_DRAG_X);
    dyn_ui_layout h = fr->vertical ? uw_l(UW_GROW(1), UW_FIXED(uw_px(ui, 4))) : uw_l(UW_FIXED(uw_px(ui, 4)), UW_GROW(1)), b = uw_l(UW_GROW(1), UW_GROW(1));
    dyn_ui_style hs = uw_s((hb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->accent : t->border, 0, 0, 0);
    dyn_ui_close(ui);
    h.interactive = 1;
    dyn_ui_open(ui, hid, &h, &hs);
    dyn_ui_close(ui);
    b.dir = DYN_UI_COLUMN;
    b.clip_x = b.clip_y = 1;
    b.gap = t->spacing;
    dyn_ui_open(ui, uw_sub(fr->id, 20), &b, NULL);
}

void dyn_ui_w_split_end(dyn_ui_t* ui)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

void dyn_ui_w_menubar_begin(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->rt;
    float fh = uw_fh(ui);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIXED(fh));
    dyn_ui_style st = uw_s(t->title, 0, 0, 0);
    uw_dock(ui, &l, fh, 0);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0x3E7B4A00u, &l, &st);
    uw_block_push(ui, 0x3E7B4A00u);
}

void dyn_ui_w_menubar_end(dyn_ui_t* ui)
{
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

int dyn_ui_w_menu_begin(dyn_ui_t* ui, const char* s, size_t n, dyn_ui_menu_frame* fr)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n), pid = uw_sub(id, 15);
    int depth = uw_popup_depth(ui), sub = depth > 0, open;
    uint32_t bits = dyn_ui_interact(ui, id, sub ? 0 : 1);
    dyn_ui_layout l = sub ? uw_l(UW_GROW(1), UW_FIT) : uw_l(UW_FIT, UW_GROW(1));
    dyn_ui_style st;
    fr->depth = depth;
    if (!sub) {
        if (uw_commit(ui, bits)) {
            if (!dyn_ui_popup_was_open(ui, pid))
                dyn_ui_open_popup(ui, pid, 0);
        } else if ((bits & DYN_UI_HOT) && ui->npopups > 0 && ui->popups[0] != pid && ui->nprev_popups > 0 && ui->prev_popups[0] == ui->popups[0]) {
            ui_el* bar = ui_el_at(ui, ui->stack[ui->depth - 1]);
            if (bar->id == 0x3E7B4A00u)
                dyn_ui_open_popup(ui, pid, 0);
        }
    } else if (bits & DYN_UI_HOT) {
        dyn_ui_open_popup(ui, pid, depth);
    }
    open = dyn_ui_popup_level(ui, pid) >= 0;
    st = uw_s(open ? t->accent : (bits & DYN_UI_HOT) ? t->button_hot
                                                     : 0,
        0, 0, 0);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.gap = t->pad_x;
    if (sub) {
        l.pad_l = t->pad_x + uw_lh(ui) + t->pad_x;
        l.pad_r = uw_px(ui, 2);
        l.pad_t = l.pad_b = uw_px(ui, 2);
    } else {
        uw_pad(&l, t->pad_x, 0);
    }
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_text(ui, s, uw_shown(s, n), open ? t->on_accent : t->text);
    if (sub) {
        uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(1)), 0);
        uw_arrow(ui, 1, t->text_dim);
    }
    if (!open) {
        dyn_ui_close(ui);
        return 0;
    }
    {
        dyn_ui_layout pl = uw_l(UW_FIT, UW_FIT);
        pl.floating = DYN_UI_FLOAT_POPUP;
        pl.anchor = sub ? DYN_UI_ANCHOR_TOP_RIGHT : DYN_UI_ANCHOR_BOTTOM_LEFT;
        pl.float_y = sub ? -t->border_w - uw_px(ui, 2) : 0;
        pl.w.min = uw_px(ui, 160);
        uw_popup_open_box(ui, pid, &pl, dyn_ui_popup_level(ui, pid));
        pl.pad_t = uw_px(ui, 2);
    }
    uw_block_push(ui, id);
    dyn_ui_push_id(ui, id);
    return 1;
}

void dyn_ui_w_menu_end(dyn_ui_t* ui, const dyn_ui_menu_frame* fr)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_set_popup_depth(ui, fr->depth);
    uw_block_pop(ui);
}

int dyn_ui_w_menu_item(dyn_ui_t* ui, const char* s, size_t n, const char* key, size_t kn, int checked, int enabled)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, 0);
    float lh = uw_lh(ui);
    int depth = uw_popup_depth(ui);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT), cl = uw_l(UW_FIXED(lh), UW_FIXED(lh));
    dyn_ui_style st = uw_s(enabled && (bits & DYN_UI_HOT) ? t->accent : 0, 0, 0, 0);
    if ((bits & DYN_UI_HOT) && depth > 0)
        dyn_ui_close_popups(ui, depth);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.gap = t->pad_x;
    uw_pad(&l, t->pad_x, uw_px(ui, 2));
    l.pad_r = t->pad_x + uw_px(ui, 2);
    l.h.min = t->min_target;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_pad(&cl, floorf(lh / 4), floorf(lh / 4));
    dyn_ui_open(ui, 0, &cl, NULL);
    if (checked > 0)
        uw_rect(ui, uw_l(UW_GROW(1), UW_GROW(1)), (bits & DYN_UI_HOT) && enabled ? t->on_accent : t->accent);
    dyn_ui_close(ui);
    uw_label_fit(ui, s, uw_shown(s, n), enabled ? ((bits & DYN_UI_HOT) ? t->on_accent : t->text) : t->text_dim);
    uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(1)), 0);
    if (kn)
        uw_text(ui, key, kn, t->text_dim);
    dyn_ui_close(ui);
    if (enabled && (bits & DYN_UI_CLICKED)) {
        dyn_ui_close_popups(ui, 0);
        return 1;
    }
    return 0;
}

void dyn_ui_w_toolbar_begin(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style st = uw_s(t->panel, t->border, t->border_w, t->radius);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.gap = uw_px(ui, 2);
    uw_pad(&l, uw_px(ui, 2), uw_px(ui, 2));
    dyn_ui_id bid = uw_sub(dyn_ui_id_of(ui, "\x01toolbar", 8), 22);
    dyn_ui_open(ui, bid, &l, &st);
    uw_block_push(ui, bid);
}

void dyn_ui_w_statusbar_begin(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->rt;
    float sh = uw_lh(ui) + uw_px(ui, 4);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIXED(sh));
    dyn_ui_style st = uw_s(t->title, 0, 0, 0);
    uw_dock(ui, &l, sh, 1);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.gap = t->pad_x * 2;
    l.pad_l += t->pad_x;
    l.pad_r += t->pad_x;
    dyn_ui_id bid = uw_sub(dyn_ui_id_of(ui, "\x01statusbar", 10), 22);
    dyn_ui_open(ui, bid, &l, &st);
    uw_block_push(ui, bid);
}

void dyn_ui_w_bar_end(dyn_ui_t* ui)
{
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

void dyn_ui_w_wrap_begin(dyn_ui_t* ui)
{
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT);
    l.dir = DYN_UI_ROW;
    l.wrap = 1;
    l.gap = ui->rt.spacing;
    l.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &l, NULL);
}

void dyn_ui_w_card_begin(dyn_ui_t* ui, const char* s, size_t n, float width)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout l = uw_l(width > 0 ? UW_FIXED(width) : UW_GROW(1), UW_FIT);
    dyn_ui_style st = uw_s(t->panel, t->border, t->border_w, t->radius);
    l.dir = DYN_UI_COLUMN;
    uw_pad(&l, t->window_pad, t->window_pad);
    l.gap = t->spacing;
    if (width <= 0)
        l.w.min = 0;
    dyn_ui_id cid = dyn_ui_id_of(ui, s, n);
    dyn_ui_open(ui, uw_sub(cid, 22), &l, &st);
    uw_block_push(ui, uw_sub(cid, 22));
    if (uw_shown(s, n))
        uw_label_fit(ui, s, uw_shown(s, n), t->text);
    dyn_ui_push_id(ui, cid);
}

void dyn_ui_w_card_end(dyn_ui_t* ui)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
}

int dyn_ui_w_chip(dyn_ui_t* ui, const char* s, size_t n, int selected)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE);
    float h = uw_lh(ui) + t->pad_y;
    dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
    dyn_ui_style st = uw_s(selected ? ((bits & DYN_UI_HOT) ? t->accent_hot : t->accent) : uw_fill(bits, t->button, t->button_hot, t->button_active),
        uw_border(ui, id), t->border_w, h / 2);
    l.pad_l = l.pad_r = t->pad_x + floorf(h / 4);
    l.pad_t = l.pad_b = floorf(t->pad_y / 2);
    l.h.min = t->min_target;
    l.align_y = DYN_UI_CENTER;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_text(ui, s, uw_shown(s, n), selected ? t->on_accent : t->text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits) ? !selected : selected;
}

void dyn_ui_w_app_bar_begin(dyn_ui_t* ui, const char* s, size_t n)
{
    const dyn_ui_theme* t = &ui->rt;
    float h = uw_fh(ui) + 2 * t->pad_y;
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIXED(h));
    dyn_ui_style st = uw_s(t->title, 0, 0, 0);
    uw_dock(ui, &l, h, 0);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.gap = t->pad_x;
    l.pad_l += t->window_pad;
    l.pad_r += t->window_pad;
    dyn_ui_open(ui, 0x3E7B4A01u, &l, &st);
    uw_block_push(ui, 0x3E7B4A01u);
    if (uw_shown(s, n)) {
        dyn_ui_text(ui, 0, s, uw_shown(s, n), uw_font(ui, t->font_px * 1.25f), t->font_px * 1.25f, t->text, DYN_UI_TEXT_ELLIPSIS, DYN_UI_START);
    }
    uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(1)), 0);
}

int dyn_ui_w_nav(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, const uint32_t* icons, int count, int selected,
    int rail)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, s, n);
    int k, out = selected, any_icon = 0;
    for (k = 0; icons && k < count; k++)
        any_icon |= icons[k] != 0;
    float lh = uw_lh(ui), icon = uw_lhx(ui, 1.5f), ih = (any_icon ? icon + t->pad_y : 0) + lh + 2 * t->pad_y, h = ih > t->min_target ? ih : t->min_target;
    dyn_ui_layout g = rail ? uw_l(UW_FIT, UW_GROW(1)) : uw_l(UW_GROW(1), UW_FIXED(h));
    dyn_ui_style gs = uw_s(t->title, 0, 0, 0);
    if (!rail)
        uw_dock(ui, &g, h, 1);
    g.dir = rail ? DYN_UI_COLUMN : DYN_UI_ROW;
    g.gap = rail ? t->spacing : 0;
    if (rail) {
        g.w.min = roundf(72 * ui->scale);
        uw_pad(&g, t->pad_y, t->window_pad);
    }
    dyn_ui_open(ui, gid, &g, &gs);
    for (k = 0; k < count; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE);
        int on = k == selected;
        dyn_ui_layout il = rail ? uw_l(UW_GROW(1), UW_FIT) : uw_l(UW_GROW(1), UW_GROW(1));
        dyn_ui_style is = uw_s(on && rail ? t->selection : (bits & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->frame_hot
                                                                                                 : 0,
            ui->focus == id && ui->focus_visible ? t->accent : 0,
            t->border_w, t->radius);
        il.dir = DYN_UI_COLUMN;
        il.align_x = il.align_y = DYN_UI_CENTER;
        il.gap = floorf(t->pad_y / 2);
        il.h.min = t->min_target;
        uw_pad(&il, t->pad_x, t->pad_y);
        il.interactive = 1;
        dyn_ui_open(ui, id, &il, &is);
        if (on && !rail) {
            dyn_ui_layout bl = uw_l(UW_GROW(1), UW_FIXED(uw_px(ui, 2)));
            dyn_ui_style bs = uw_s(t->accent, 0, 0, 0);
            bl.floating = DYN_UI_FLOAT_PARENT;
            bl.float_x = t->pad_x;
            dyn_ui_open(ui, 0, &bl, &bs);
            dyn_ui_close(ui);
        }
        if (icons && icons[k]) {
            dyn_ui_layout gl = uw_l(UW_FIXED(icon), UW_FIXED(icon));
            int iw, ihh;
            if (dyn_ui_image_size(ui, icons[k], &iw, &ihh) == 0)
                dyn_ui_image(ui, 0, icons[k], &gl, on ? t->accent : t->text_dim);
        }
        uw_label_fit(ui, items[k], lens[k], on ? t->accent : t->text_dim);
        dyn_ui_close(ui);
        if (uw_activated(ui, id, bits))
            out = k;
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

static int uw_overlay_begin(dyn_ui_t* ui, dyn_ui_id pid, int* depth, dyn_ui_layout* panel, const dyn_ui_style* ps, int dir, int align_x, int align_y)
{
    const dyn_ui_theme* t = &ui->rt;
    int level = dyn_ui_popup_level(ui, pid);
    uint32_t bits;
    dyn_ui_layout sh = uw_l(UW_GROW(1), UW_GROW(1));
    dyn_ui_style shs = uw_s(t->shade, 0, 0, 0);
    *depth = uw_popup_depth(ui);
    if (level < 0)
        return 0;
    bits = dyn_ui_interact(ui, pid, 0);
    if (bits & DYN_UI_CLICKED) {
        dyn_ui_close_popups(ui, level);
        return 0;
    }
    sh.floating = DYN_UI_FLOAT_ROOT;
    sh.z = UW_Z_MODAL + level;
    sh.interactive = 1;
    sh.dir = (uint8_t)dir;
    sh.align_x = (uint8_t)align_x;
    sh.align_y = (uint8_t)align_y;
    dyn_ui_open(ui, pid, &sh, &shs);
    panel->interactive = 1;
    dyn_ui_open(ui, uw_sub(pid, 1), panel, ps);
    uw_set_popup_depth(ui, level + 1);
    uw_block_push(ui, uw_sub(pid, 1));
    dyn_ui_push_id(ui, pid);
    return 1;
}

void dyn_ui_w_overlay_end(dyn_ui_t* ui, int depth)
{
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_set_popup_depth(ui, depth);
    uw_block_pop(ui);
}

int dyn_ui_w_drawer_begin(dyn_ui_t* ui, const char* s, size_t n, int right, float width, int* depth)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id pid = uw_overlay_id(s, n);
    float avail = ui->width - ui->insets[0] - ui->insets[2], w = width > 0 ? width : roundf(300 * ui->scale);
    dyn_ui_layout pl;
    dyn_ui_style ps = uw_s(t->panel, t->border, t->border_w, 0);
    if (w > avail * 0.85f)
        w = roundf(avail * 0.85f);
    pl = uw_l(UW_FIXED(w), UW_GROW(1));
    pl.dir = DYN_UI_COLUMN;
    pl.pad_l = t->window_pad + (right ? 0 : ui->insets[0]);
    pl.pad_r = t->window_pad + (right ? ui->insets[2] : 0);
    pl.pad_t = t->window_pad + ui->insets[1];
    pl.pad_b = t->window_pad + ui->insets[3];
    pl.gap = t->spacing;
    pl.clip_y = 1;
    return uw_overlay_begin(ui, pid, depth, &pl, &ps, DYN_UI_ROW, right ? DYN_UI_END : DYN_UI_START, DYN_UI_START);
}

int dyn_ui_w_sheet_begin(dyn_ui_t* ui, const char* s, size_t n, int* depth)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id pid = uw_overlay_id(s, n);
    float r = t->radius > 0 ? t->radius * 2 : roundf(12 * ui->scale);
    dyn_ui_layout pl = uw_l(UW_FIXED(ui->width), UW_FIT);
    dyn_ui_style ps = uw_s(t->popup, t->border, t->border_w, r);
    int open;
    pl.h.max = roundf(ui->height * 0.9f);
    pl.dir = DYN_UI_COLUMN;
    pl.align_x = DYN_UI_START;
    pl.pad_l = t->window_pad + ui->insets[0];
    pl.pad_r = t->window_pad + ui->insets[2];
    pl.pad_t = t->window_pad;
    pl.pad_b = t->window_pad + ui->insets[3] + r;
    pl.gap = t->spacing;
    pl.clip_y = 1;
    open = uw_overlay_begin(ui, pid, depth, &pl, &ps, DYN_UI_COLUMN, DYN_UI_START, DYN_UI_END);
    if (open) {
        dyn_ui_layout hl = uw_l(UW_GROW(1), UW_FIT);
        hl.align_x = DYN_UI_CENTER;
        dyn_ui_open(ui, 0, &hl, NULL);
        {
            dyn_ui_layout gl = uw_l(UW_FIXED(roundf(32 * ui->scale)), UW_FIXED(fmaxf(2, roundf(4 * ui->scale))));
            dyn_ui_style gs = uw_s(t->border, 0, 0, 2 * ui->scale);
            dyn_ui_open(ui, 0, &gl, &gs);
            dyn_ui_close(ui);
        }
        dyn_ui_close(ui);
        if (uw_shown(s, n))
            uw_label_fit(ui, s, uw_shown(s, n), t->text);
    }
    return open;
}

void dyn_ui_w_open_overlay(dyn_ui_t* ui, const char* s, size_t n)
{
    dyn_ui_open_popup(ui, uw_overlay_id(s, n), uw_popup_depth(ui));
}

int dyn_ui_w_toast(dyn_ui_t* ui, const char* s, size_t n, double seconds)
{
    struct uw_state* w = uw_get(ui);
    char* c;
    if (!w)
        return -1;
    if (n > 4096)
        n = 4096;
    c = malloc(n + 1);
    if (!c)
        return -1;
    memcpy(c, s, n);
    c[n] = 0;
    if (w->ntoasts == 8) {
        free(w->toasts[0].text);
        memmove(w->toasts, w->toasts + 1, 7 * sizeof(w->toasts[0]));
        w->ntoasts--;
    }
    w->toasts[w->ntoasts].text = c;
    w->toasts[w->ntoasts].n = n;
    w->toasts[w->ntoasts].until = ui->now + (seconds > 0 && isfinite(seconds) ? seconds : 3);
    w->ntoasts++;
    return 0;
}

static void uw_toasts(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* w = ui->w;
    int k, j = 0;
    dyn_ui_layout col = uw_l(UW_FIXED(ui->width), UW_FIT);
    if (!w || !w->ntoasts)
        return;
    for (k = 0; k < w->ntoasts; k++) {
        if (w->toasts[k].until <= ui->now)
            free(w->toasts[k].text);
        else
            w->toasts[j++] = w->toasts[k];
    }
    w->ntoasts = j;
    if (!j)
        return;
    col.floating = DYN_UI_FLOAT_ROOT;
    col.anchor = DYN_UI_ANCHOR_BOTTOM_LEFT;
    col.float_y = ui->height - ui->insets[3] - (w->dock_b > w->dock_b_last ? w->dock_b : w->dock_b_last);
    col.z = UW_Z_TOOLTIP - 1;
    col.dir = DYN_UI_COLUMN;
    col.align_x = DYN_UI_CENTER;
    col.gap = t->spacing;
    col.pad_b = t->window_pad * 2;
    col.pad_l = t->window_pad + ui->insets[0];
    col.pad_r = t->window_pad + ui->insets[2];
    dyn_ui_open(ui, 0, &col, NULL);
    for (k = 0; k < j; k++) {
        dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
        dyn_ui_style st = uw_s(t->tooltip, t->border, t->border_w, t->radius);
        l.w.max = roundf(560 * ui->scale);
        uw_pad(&l, t->pad_x * 2, t->pad_y * 2);
        dyn_ui_open(ui, 0, &l, &st);
        dyn_ui_text(ui, 0, w->toasts[k].text, w->toasts[k].n, t->font, t->font_px, t->text, DYN_UI_TEXT_WRAP, DYN_UI_START);
        dyn_ui_close(ui);
        dyn_ui_wake_at(ui, w->toasts[k].until);
    }
    dyn_ui_close(ui);
}

int dyn_ui_w_fab(dyn_ui_t* ui, const char* s, size_t n, uint32_t image)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n);
    uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE);
    float z = roundf(56 * ui->scale), m = roundf(16 * ui->scale);
    int extended = !image && uw_shown(s, n) > 0, iw, ih;
    dyn_ui_layout l = uw_l(extended ? UW_FIT : UW_FIXED(z), UW_FIXED(z));
    dyn_ui_style st = uw_s((bits & DYN_UI_ACTIVE) ? t->accent : (bits & DYN_UI_HOT) ? t->accent_hot
                                                                                    : t->accent,
        uw_border(ui, id),
        ui->focus == id && ui->focus_visible ? t->border_w : 0, z / 2);
    l.floating = DYN_UI_FLOAT_ROOT;
    l.float_x = ui->width - ui->insets[2] - m - z;
    l.float_y = ui->height - ui->insets[3] - (ui->w ? (ui->w->dock_b > ui->w->dock_b_last ? ui->w->dock_b : ui->w->dock_b_last) : 0) - m - z;
    l.w.min = z;
    l.z = UW_Z_WINDOW - 1;
    l.align_x = l.align_y = DYN_UI_CENTER;
    l.pad_l = l.pad_r = extended ? roundf(z / 3) : 0;
    l.interactive = 1;
    if (extended) {
        float ew = dyn_ui_text_width(ui, t->font, t->font_px, s, uw_shown(s, n)) + 2 * l.pad_l;
        l.float_x = ui->width - ui->insets[2] - m - (ew > z ? ew : z);
    }
    dyn_ui_open(ui, id, &l, &st);
    if (image && dyn_ui_image_size(ui, image, &iw, &ih) == 0) {
        dyn_ui_layout gl = uw_l(UW_FIXED(roundf(z * 0.43f)), UW_FIXED(roundf(z * 0.43f)));
        dyn_ui_image(ui, 0, image, &gl, t->on_accent);
    } else if (extended)
        uw_text(ui, s, uw_shown(s, n), t->on_accent);
    else {
        float a = z * 0.33f, b = z * 0.67f, c = z * 0.5f, d = fmaxf(1, roundf(z / 28));
        dyn_ui_vertex pv[4] = { { a, c }, { b, c }, { c, a }, { c, b } };
        dyn_ui_vertex nv[4];
        int k;
        for (k = 0; k < 4; k++) {
            nv[k].x = pv[k].x / z;
            nv[k].y = pv[k].y / z;
        }
        dyn_ui_polyline(ui, nv, 2, t->on_accent, 2 * d);
        dyn_ui_polyline(ui, nv + 2, 2, t->on_accent, 2 * d);
    }
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits);
}

int dyn_ui_w_breadcrumbs(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, s, n);
    dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT);
    int k, out = -1;
    row.dir = DYN_UI_ROW;
    row.wrap = 1;
    row.gap = t->pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, gid, &row, NULL);
    for (k = 0; k < count; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        if (k)
            uw_text(ui, "/", 1, t->text_dim);
        if (k == count - 1) {
            dyn_ui_text(ui, id, items[k], lens[k], t->font, t->font_px, t->text, 0, DYN_UI_START);
        } else {
            uint32_t bits = dyn_ui_interact(ui, id, DYN_UI_FOCUSABLE);
            dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
            l.h.min = t->min_target;
            l.align_y = DYN_UI_CENTER;
            l.interactive = 1;
            dyn_ui_open(ui, id, &l, NULL);
            uw_text(ui, items[k], lens[k], t->link);
            if (bits & (DYN_UI_HOT | DYN_UI_ACTIVE)) {
                float lh = uw_lh(ui), hh = lh > t->min_target ? lh : t->min_target;
                uw_float_rect(ui, 0, roundf((hh - lh) / 2) + lh - uw_hair(ui), dyn_ui_text_width(ui, t->font, t->font_px, items[k], lens[k]), uw_hair(ui), t->link, 0);
            }
            dyn_ui_close(ui);
            if (uw_activated(ui, id, bits))
                out = k;
        }
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

int dyn_ui_w_pagination(dyn_ui_t* ui, const char* s, size_t n, int page, int pages)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, s, n);
    dyn_ui_layout row = uw_l(UW_FIT, UW_FIT);
    struct uw_state* w = uw_get(ui);
    int k, out = page, shown[9], ns = 0;
    if (!w || pages < 1)
        return page;
    if (page < 0)
        page = 0;
    if (page >= pages)
        page = pages - 1;
    for (k = 0; k < pages && pages <= 7; k++)
        shown[ns++] = k;
    if (pages > 7) {
        int a = page - 1 < 1 ? 1 : page - 1, b = page + 1 > pages - 2 ? pages - 2 : page + 1;
        if (b - a < 2) {
            if (a == 1)
                b = 3;
            else
                a = pages - 4;
        }
        shown[ns++] = 0;
        if (a > 1)
            shown[ns++] = -1;
        for (k = a; k <= b; k++)
            shown[ns++] = k;
        if (b < pages - 2)
            shown[ns++] = -1;
        shown[ns++] = pages - 1;
    }
    row.dir = DYN_UI_ROW;
    row.gap = uw_px(ui, 2);
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, gid, &row, NULL);
    for (k = -1; k <= ns; k++) {
        dyn_ui_id id;
        uint32_t bits;
        size_t ln;
        int target, on;
        if (k >= 0 && k < ns && shown[k] < 0) {
            uw_text(ui, "...", 3, t->text_dim);
            continue;
        }
        target = k < 0 ? page - 1 : k == ns ? page + 1
                                            : shown[k];
        id = k < 0 ? uw_sub(gid, 1) : k == ns ? uw_sub(gid, 2)
                                              : uw_sub(gid, 0x1000u + (uint32_t)target);
        on = k >= 0 && k < ns && target == page;
        bits = uw_button_box(ui, id, on, UW_FIT);
        if (k < 0)
            uw_arrow(ui, 2, (page > 0) ? t->text : t->text_dim);
        else if (k == ns)
            uw_arrow(ui, 1, (page < pages - 1) ? t->text : t->text_dim);
        else {
            ln = uw_fmt(w->scratch, sizeof(w->scratch), (double)target + 1, 0);
            uw_text(ui, w->scratch, ln, on ? t->on_accent : t->text);
        }
        dyn_ui_close(ui);
        if (uw_activated(ui, id, bits) && target >= 0 && target < pages)
            out = target;
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0x1000u);
    return out;
}

int dyn_ui_w_stepper(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, int count, int current)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, s, n);
    struct uw_state* w = uw_get(ui);
    float d = uw_lhx(ui, 1.6f);
    dyn_ui_layout row = uw_l(UW_GROW(1), UW_FIT);
    int k, out = -1;
    if (!w)
        return -1;
    row.dir = DYN_UI_ROW;
    row.gap = t->pad_x;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, gid, &row, NULL);
    for (k = 0; k < count; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        uint32_t bits = dyn_ui_interact(ui, id, 0);
        int done = k < current, on = k == current;
        dyn_ui_layout il = uw_l(UW_FIT, UW_FIT), cl = uw_l(UW_FIXED(d), UW_FIXED(d));
        dyn_ui_style cs = uw_s(done || on ? t->accent : t->frame, on ? t->accent_hot : t->border, t->border_w, d / 2);
        size_t ln = uw_fmt(w->scratch, sizeof(w->scratch), (double)k + 1, 0);
        if (k)
            uw_rect(ui, uw_l(UW_GROW(1), UW_FIXED(fmaxf(1, t->border_w))), done || on ? t->accent : t->border);
        il.dir = DYN_UI_ROW;
        il.gap = t->pad_x;
        il.align_y = DYN_UI_CENTER;
        il.h.min = t->min_target;
        il.interactive = 1;
        dyn_ui_open(ui, id, &il, NULL);
        cl.align_x = cl.align_y = DYN_UI_CENTER;
        dyn_ui_open(ui, 0, &cl, &cs);
        uw_text(ui, w->scratch, ln, done || on ? t->on_accent : t->text_dim);
        dyn_ui_close(ui);
        uw_label_fit(ui, items[k], lens[k], on ? t->text : t->text_dim);
        dyn_ui_close(ui);
        if (bits & DYN_UI_CLICKED)
            out = k;
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

static void uw_star(dyn_ui_t* ui, float z, uint32_t fill, uint32_t line)
{
    dyn_ui_vertex pts[10], tri[30], ring[11];
    int k;
    for (k = 0; k < 10; k++) {
        float a = -1.5707963f + (float)k * 0.62831853f, r = (k & 1) ? 0.2f : 0.48f;
        pts[k].x = 0.5f + r * cosf(a);
        pts[k].y = 0.53f + r * sinf(a);
    }
    if (fill & 0xFFu) {
        for (k = 0; k < 10; k++) {
            tri[3 * k].x = 0.5f * z, tri[3 * k].y = 0.53f * z;
            tri[3 * k + 1].x = pts[k].x * z, tri[3 * k + 1].y = pts[k].y * z;
            tri[3 * k + 2].x = pts[(k + 1) % 10].x * z, tri[3 * k + 2].y = pts[(k + 1) % 10].y * z;
        }
        dyn_ui_triangles(ui, tri, 30, fill);
    }
    for (k = 0; k < 11; k++)
        ring[k] = pts[k % 10];
    dyn_ui_polyline(ui, ring, 11, line, fmaxf(1, ui->scale));
}

int dyn_ui_w_rating(dyn_ui_t* ui, const char* s, size_t n, int value, int max)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, s, n);
    float z = uw_lhx(ui, 1.4f), h = z > t->min_target ? z : t->min_target;
    dyn_ui_layout row = uw_l(UW_FIT, UW_FIT);
    int k, out = value, preview = -1;
    if (max < 1)
        max = 1;
    if (max > 20)
        max = 20;
    row.dir = DYN_UI_ROW;
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, gid, &row, NULL);
    for (k = 0; k < max; k++)
        if (ui->hot == uw_sub(gid, (uint32_t)k) && ui->ptr.kind == DYN_UI_POINTER_MOUSE)
            preview = k + 1;
    for (k = 0; k < max; k++) {
        dyn_ui_id id = uw_sub(gid, (uint32_t)k);
        uint32_t bits = dyn_ui_interact(ui, id, k == 0 ? DYN_UI_FOCUSABLE : 0);
        int lit = k < (preview >= 0 ? preview : value);
        dyn_ui_layout l = uw_l(UW_FIXED(h), UW_FIXED(h));
        l.interactive = 1;
        l.align_x = l.align_y = DYN_UI_CENTER;
        dyn_ui_open(ui, id, &l, NULL);
        {
            dyn_ui_layout sl = uw_l(UW_FIXED(z), UW_FIXED(z));
            dyn_ui_open(ui, 0, &sl, NULL);
            uw_star(ui, z, lit ? t->accent : 0, lit ? t->accent : t->text_dim);
            dyn_ui_close(ui);
        }
        dyn_ui_close(ui);
        if (bits & DYN_UI_CLICKED)
            out = (k + 1 == value) ? 0 : k + 1;
        if (k == 0 && ui->focus == id) {
            int d = ui->fin.key_hit[DYN_UI_KEY_RIGHT] - ui->fin.key_hit[DYN_UI_KEY_LEFT];
            out = out + d < 0 ? 0 : out + d > max ? max
                                                  : out + d;
        }
    }
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return out;
}

static int uw_days_in(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[(m - 1) % 12];
}

static int uw_weekday(int y, int m, int d)
{
    static const int tt[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + tt[m - 1] + d) % 7;
}

typedef struct {
    int y, m, init;
} uw_cal;

int dyn_ui_w_date(dyn_ui_t* ui, const char* s, size_t n, int* py, int* pm, int* pd, int first_day)
{
    static const char* const months[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    static const char* const wd[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_id(ui, s, n), prev = uw_sub(id, 30), next = uw_sub(id, 31);
    struct uw_state* w = uw_get(ui);
    uw_cal* cal = dyn_ui_state(ui, uw_sub(id, 32), sizeof(uw_cal), NULL);
    float cell = uw_fh(ui) > roundf(uw_lh(ui) * 2) ? uw_fh(ui) : roundf(uw_lh(ui) * 2);
    dyn_ui_layout col = uw_l(UW_FIT, UW_FIT), head = uw_l(UW_GROW(1), UW_FIT), grid;
    int y = *py, m = *pm, d = *pd, k, lead, days, changed = 0;
    uint32_t b;
    if (!w || !cal)
        return 0;
    if (m < 1 || m > 12 || y < 1 || y > 9999)
        return 0;
    if (d < 1)
        d = 1;
    if (d > uw_days_in(y, m))
        d = uw_days_in(y, m);
    if (!cal->init) {
        cal->y = y;
        cal->m = m;
        cal->init = 1;
    }
    first_day = ((first_day % 7) + 7) % 7;
    col.dir = DYN_UI_COLUMN;
    col.gap = t->spacing;
    dyn_ui_open(ui, id, &col, NULL);
    head.dir = DYN_UI_ROW;
    head.align_y = DYN_UI_CENTER;
    head.gap = t->pad_x;
    dyn_ui_open(ui, 0, &head, NULL);
    b = uw_button_box(ui, prev, 0, UW_FIT);
    uw_arrow(ui, 2, t->text);
    dyn_ui_close(ui);
    if (uw_activated(ui, prev, b) && !(cal->y == 1 && cal->m == 1)) {
        cal->m--;
        if (cal->m < 1) {
            cal->m = 12;
            cal->y--;
        }
    }
    {
        dyn_ui_layout ml = uw_l(UW_GROW(1), UW_FIT);
        size_t ln = (size_t)snprintf(w->scratch, sizeof(w->scratch), "%s %d", months[cal->m - 1], cal->y);
        ml.align_x = DYN_UI_CENTER;
        dyn_ui_open(ui, 0, &ml, NULL);
        uw_text(ui, w->scratch, ln, t->text);
        dyn_ui_close(ui);
    }
    b = uw_button_box(ui, next, 0, UW_FIT);
    uw_arrow(ui, 1, t->text);
    dyn_ui_close(ui);
    if (uw_activated(ui, next, b) && !(cal->y == 9999 && cal->m == 12)) {
        cal->m++;
        if (cal->m > 12) {
            cal->m = 1;
            cal->y++;
        }
    }
    dyn_ui_close(ui);
    grid = uw_l(UW_FIXED(cell * 7), UW_FIT);
    grid.dir = DYN_UI_ROW;
    grid.wrap = 1;
    dyn_ui_open(ui, 0, &grid, NULL);
    for (k = 0; k < 7; k++) {
        dyn_ui_layout hl = uw_l(UW_FIXED(cell), UW_FIXED(uw_lh(ui) + t->pad_y));
        hl.align_x = hl.align_y = DYN_UI_CENTER;
        dyn_ui_open(ui, 0, &hl, NULL);
        uw_text(ui, wd[(k + first_day) % 7], 2, t->text_dim);
        dyn_ui_close(ui);
    }
    lead = (uw_weekday(cal->y, cal->m, 1) - first_day + 7) % 7;
    days = uw_days_in(cal->y, cal->m);
    for (k = 0; k < lead; k++)
        uw_rect(ui, uw_l(UW_FIXED(cell), UW_FIXED(cell)), 0);
    for (k = 1; k <= days; k++) {
        dyn_ui_id did = uw_sub(id, 0x100 + (uint32_t)k);
        uint32_t bits = dyn_ui_interact(ui, did, 0);
        int on = cal->y == y && cal->m == m && k == d;
        dyn_ui_layout dl = uw_l(UW_FIXED(cell), UW_FIXED(cell));
        dyn_ui_style ds = uw_s(on ? t->accent : (bits & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->frame_hot
                                                                                      : 0,
            0, 0, cell / 2);
        size_t ln = uw_fmt(w->scratch, sizeof(w->scratch), (double)k, 0);
        dl.align_x = dl.align_y = DYN_UI_CENTER;
        dl.interactive = 1;
        dyn_ui_open(ui, did, &dl, &ds);
        uw_text(ui, w->scratch, ln, on ? t->on_accent : t->text);
        dyn_ui_close(ui);
        if (bits & DYN_UI_CLICKED) {
            y = cal->y;
            m = cal->m;
            d = k;
            changed = 1;
        }
    }
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    uw_items(ui, id, id, 0x100);
    *py = y;
    *pm = m;
    *pd = d;
    return changed;
}

void dyn_ui_w_avatar(dyn_ui_t* ui, const char* s, size_t n, uint32_t image, float size)
{
    const dyn_ui_theme* t = &ui->rt;
    float z = size > 0 ? size : uw_lhx(ui, 2.5f);
    dyn_ui_layout l = uw_l(UW_FIXED(z), UW_FIXED(z));
    char ini[8];
    size_t k = 0, i = 0;
    int iw, ih, start = 1;
    dyn_ui_id h = dyn_ui_hash(0, s, n);
    static const uint32_t palette[6] = { 0x3D7EFFFFu, 0x2E9E6BFFu, 0xC2563AFFu, 0x8A5CD6FFu, 0xD49A1EFFu, 0x2A9DB0FFu };
    dyn_ui_style st = uw_s(palette[h % 6], 0, 0, z / 2);
    if (image && dyn_ui_image_size(ui, image, &iw, &ih) == 0) {
        dyn_ui_image(ui, 0, image, &l, 0xFFFFFFFFu);
        return;
    }
    while (i < n && k < 2) {
        size_t at = i;
        uint32_t cp = ui_utf8(s, n, &i);
        if (cp == ' ') {
            start = 1;
            continue;
        }
        if (start) {
            if (cp >= 'a' && cp <= 'z')
                cp -= 32;
            if (cp < 0x80)
                ini[k++] = (char)cp;
            else if (i - at <= 4 && k == 0) {
                memcpy(ini, s + at, i - at);
                k = i - at;
                break;
            }
            start = 0;
        }
    }
    l.align_x = l.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &l, &st);
    if (k)
        dyn_ui_text(ui, 0, ini, k, uw_font(ui, roundf(z * 0.4f)), roundf(z * 0.4f), t->on_accent, 0, DYN_UI_START);
    dyn_ui_close(ui);
}

void dyn_ui_w_skeleton(dyn_ui_t* ui, float w, float h)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout l = uw_l(w > 0 ? UW_FIXED(w) : UW_GROW(1), UW_FIXED(h > 0 ? h : uw_lh(ui)));
    dyn_ui_style st = uw_s(t->frame_hot, 0, 0, t->radius > 0 ? t->radius : roundf(3 * ui->scale));
    dyn_ui_open(ui, 0, &l, &st);
    dyn_ui_close(ui);
}

void dyn_ui_w_field_error(dyn_ui_t* ui, int on)
{
    struct uw_state* w = uw_get(ui);
    if (w)
        w->field_error = on;
}

void dyn_ui_w_field_note(dyn_ui_t* ui, const char* s, size_t n, int error)
{
    dyn_ui_text(ui, 0, s, n, ui->rt.font, ui->rt.font_px, error ? ui->rt.error : ui->rt.text_dim, DYN_UI_TEXT_WRAP, DYN_UI_START);
}

int dyn_ui_w_drag_source(dyn_ui_t* ui, const char* s, size_t n)
{
    const dyn_ui_theme* t = &ui->rt;
    int r = dyn_ui_drag_source(ui);
    if (r) {
        dyn_ui_layout l = uw_l(UW_FIT, UW_FIT);
        dyn_ui_style st = uw_s(t->popup, t->accent, t->border_w > 0 ? t->border_w : 1, t->radius);
        l.floating = DYN_UI_FLOAT_SAFE;
        l.float_x = ui->ptr.x + uw_px(ui, 12);
        l.float_y = ui->ptr.y + uw_px(ui, 8);
        l.z = UW_Z_TOOLTIP - 2;
        l.w.max = uw_px(ui, 320);
        uw_pad(&l, t->pad_x, t->pad_y);
        dyn_ui_open(ui, 0, &l, &st);
        if (n)
            uw_label_fit(ui, s, n, t->text);
        else
            uw_rect(ui, uw_l(UW_FIXED(uw_lh(ui)), UW_FIXED(uw_lh(ui))), 0);
        dyn_ui_close(ui);
    }
    return r;
}

int dyn_ui_w_drop_target(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = ui->last_id;
    int r = dyn_ui_drop_target(ui);
    dyn_ui_rect b;
    if (r == 1 && dyn_ui_box(ui, id, &b)) {
        dyn_ui_layout l = uw_l(UW_FIXED(b.w), UW_FIXED(b.h));
        dyn_ui_style st = uw_s(t->selection, t->accent, fmaxf(t->border_w, uw_px(ui, 2)), t->radius);
        l.floating = DYN_UI_FLOAT_ROOT;
        l.float_x = b.x;
        l.float_y = b.y;
        l.z = UW_Z_TOOLTIP - 3;
        dyn_ui_open(ui, 0, &l, &st);
        dyn_ui_close(ui);
    }
    return r;
}

typedef struct {
    int hi, dismissed;
} uw_auto_st;

const char* dyn_ui_w_autocomplete(dyn_ui_t* ui, const char* s, size_t n, const char* text, size_t tn, const char* const* items, const size_t* lens, int count,
    const char* hint, size_t hn, float width, size_t* out_n, int* picked)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = dyn_ui_id_of(ui, s, n), pid = uw_sub(id, 28);
    int fresh = 0, ev = 0, depth = uw_popup_depth(ui), show, k, level;
    uw_auto_st* st = dyn_ui_state(ui, uw_sub(id, 27), sizeof(uw_auto_st), &fresh);
    const char* r;
    dyn_ui_rect box;
    *picked = -1;
    *out_n = tn;
    if (!st)
        return text;
    if (fresh)
        st->hi = -1;
    show = ui->focus == id && count > 0 && !st->dismissed;
    if (show && dyn_ui_popup_level(ui, pid) >= 0) {
        st->hi = uw_list_keys(ui, st->hi, count, items, lens, "", 0);
        if (ui->fin.key_hit[DYN_UI_KEY_RETURN] && st->hi >= 0 && st->hi < count)
            *picked = st->hi;
        if (ui->fin.key_hit[DYN_UI_KEY_ESCAPE])
            st->dismissed = 1;
    }
    r = uw_edit(ui, s, n, text, tn, UW_EDIT_KEYLIST, 0, hint, hn, 1, width, out_n, &ev, 1);
    if (ev & DYN_UI_EDIT_CHANGED) {
        st->dismissed = 0;
        st->hi = -1;
    }
    if (ui->focus != id) {
        st->dismissed = 0;
        st->hi = -1;
    }
    show = ui->focus == id && count > 0 && !st->dismissed && *picked < 0;
    level = dyn_ui_popup_level(ui, pid);
    if (show && level < 0) {
        dyn_ui_open_popup(ui, pid, depth);
        level = dyn_ui_popup_level(ui, pid);
    } else if (!show && level >= 0) {
        dyn_ui_close_popups(ui, level);
        level = -1;
    }
    if (show && level >= 0 && dyn_ui_box(ui, id, &box)) {
        dyn_ui_layout pl = uw_l(UW_FIXED(box.w), UW_FIT);
        int ferr = ui->w ? ui->w->field_error : 0;
        pl.floating = DYN_UI_FLOAT_SAFE;
        pl.float_x = box.x;
        pl.float_y = box.y + box.h;
        if (count > 8) {
            pl.h = UW_FIXED(8 * (uw_lh(ui) + 4) + 2 * t->border_w);
            pl.clip_y = 1;
        }
        uw_popup_open_box(ui, pid, &pl, level);
        if (ui->w)
            ui->w->field_error = 0;
        for (k = 0; k < count; k++) {
            dyn_ui_id iid = uw_sub(pid, (uint32_t)k);
            uint32_t ib = dyn_ui_interact(ui, iid, 0);
            dyn_ui_layout il = uw_l(UW_GROW(1), UW_FIT);
            dyn_ui_style is = uw_s((ib & DYN_UI_HOT) || k == st->hi ? t->button_hot : 0, 0, 0, 0);
            uw_pad(&il, t->pad_x, uw_px(ui, 2));
            il.h.min = t->min_target;
            il.align_y = DYN_UI_CENTER;
            il.clip_x = 1;
            il.interactive = 1;
            dyn_ui_open(ui, iid, &il, &is);
            uw_label_fit(ui, items[k], lens[k], t->text);
            dyn_ui_close(ui);
            if (ib & DYN_UI_CLICKED)
                *picked = k;
        }
        dyn_ui_close(ui);
        if (ui->w)
            ui->w->field_error = ferr;
        uw_set_popup_depth(ui, depth);
    }
    if (*picked >= 0) {
        st->dismissed = 1;
        st->hi = -1;
        ui->focus = id;
        if (ui->w && ui->w->edit_id == id)
            ui->w->edit_id = 0;
        *out_n = lens[*picked];
        r = items[*picked];
    }
    uw_items(ui, id, pid, 0);
    return r;
}

typedef struct {
    int digit;
    dyn_ui_id seg;
} uw_time_st;

static int uw_time_seg(dyn_ui_t* ui, dyn_ui_id sid, const char* txt, size_t tn, int* delta)
{
    const dyn_ui_theme* t = &ui->rt;
    uint32_t bits = dyn_ui_interact(ui, sid, DYN_UI_FOCUSABLE);
    dyn_ui_id up = uw_sub(sid, 1), dn = uw_sub(sid, 2);
    uint32_t ub = dyn_ui_interact(ui, up, 0), db = dyn_ui_interact(ui, dn, 0);
    float lh = uw_lh(ui), fh = uw_fh(ui);
    dyn_ui_layout l = uw_l(UW_FIT, UW_FIXED(fh)), al = uw_l(UW_FIXED(lh), UW_FIXED(fh / 2));
    dyn_ui_style st = uw_s(ui->focus == sid ? t->frame_active : uw_fill(bits, t->frame, t->frame_hot, t->frame_active), uw_border(ui, sid), t->border_w, t->radius);
    dyn_ui_style us = uw_s((ub & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->button_hot : 0, 0, 0, 0), ds = uw_s((db & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->button_hot : 0, 0, 0, 0);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.pad_l = t->pad_x;
    l.interactive = 1;
    dyn_ui_open(ui, sid, &l, &st);
    uw_text(ui, txt, tn, t->text);
    {
        dyn_ui_layout cl = uw_l(UW_FIT, UW_FIT);
        cl.dir = DYN_UI_COLUMN;
        dyn_ui_open(ui, 0, &cl, NULL);
        al.interactive = 1;
        al.align_y = DYN_UI_CENTER;
        dyn_ui_open(ui, up, &al, &us);
        uw_arrow(ui, 3, t->text_dim);
        dyn_ui_close(ui);
        dyn_ui_open(ui, dn, &al, &ds);
        uw_arrow(ui, 0, t->text_dim);
        dyn_ui_close(ui);
        dyn_ui_close(ui);
    }
    dyn_ui_close(ui);
    *delta = 0;
    if (ub & DYN_UI_CLICKED)
        *delta += 1;
    if (db & DYN_UI_CLICKED)
        *delta -= 1;
    if ((bits & DYN_UI_HOT) && ui->fin.wheel_y != 0)
        *delta += ui->fin.wheel_y > 0 ? 1 : -1;
    if (ui->focus == sid)
        *delta += ui->fin.key_hit[DYN_UI_KEY_UP] - ui->fin.key_hit[DYN_UI_KEY_DOWN];
    return ui->focus == sid;
}

static int uw_wrap_add(int v, int d, int m)
{
    v = (v + d) % m;
    return v < 0 ? v + m : v;
}

int dyn_ui_w_time(dyn_ui_t* ui, const char* s, size_t n, int* hour, int* minute, int step, int flags)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* w = uw_get(ui);
    dyn_ui_id gid = uw_id(ui, s, n);
    dyn_ui_layout row = uw_l(UW_FIT, UW_FIT);
    uw_time_st* ts = dyn_ui_state(ui, uw_sub(gid, 29), sizeof(uw_time_st), NULL);
    int h = *hour, m = *minute, h12 = (flags & DYN_UI_TIME_H12) != 0, d, k, focused;
    char buf[8];
    if (!w || !ts)
        return 0;
    if (step < 1 || step > 60)
        step = 1;
    h = h < 0 ? 0 : h > 23 ? 23
                           : h;
    m = m < 0 ? 0 : m > 59 ? 59
                           : m;
    row.dir = DYN_UI_ROW;
    row.gap = uw_px(ui, 2);
    row.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, gid, &row, NULL);
    for (k = 0; k < 2; k++) {
        dyn_ui_id sid = uw_sub(gid, (uint32_t)k);
        int v = k == 0 ? (h12 ? (h % 12 == 0 ? 12 : h % 12) : h) : m;
        buf[0] = (char)('0' + v / 10);
        buf[1] = (char)('0' + v % 10);
        focused = uw_time_seg(ui, sid, buf, 2, &d);
        if (k == 0)
            h = uw_wrap_add(h, d, 24);
        else if (d)
            m = uw_wrap_add(m - m % step, d * step, 60);
        if (focused) {
            const char* tp = ui->fin.typed;
            size_t i;
            if (ts->seg != sid)
                ts->digit = -1;
            ts->seg = sid;
            for (i = 0; i < ui->fin.ntyped; i++) {
                int c = tp[i] - '0', nv;
                if (c < 0 || c > 9)
                    continue;
                nv = ts->digit >= 0 ? ts->digit * 10 + c : c;
                if (k == 0) {
                    if (h12) {
                        int pm = h >= 12;
                        if (nv < 1 || nv > 12)
                            nv = c;
                        h = (nv % 12) + (pm ? 12 : 0);
                    } else
                        h = nv <= 23 ? nv : c;
                } else
                    m = nv <= 59 ? nv : c;
                ts->digit = ts->digit >= 0 ? -1 : c;
            }
        }
        if (k == 0)
            uw_text(ui, ":", 1, t->text);
    }
    if (h12) {
        dyn_ui_id sid = uw_sub(gid, 2);
        uint32_t bits = uw_button_box(ui, sid, 0, UW_FIT);
        uw_text(ui, h >= 12 ? "PM" : "AM", 2, t->text);
        dyn_ui_close(ui);
        if (uw_activated(ui, sid, bits))
            h = (h + 12) % 24;
    }
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    d = h != *hour || m != *minute;
    *hour = h;
    *minute = m;
    return d;
}

typedef struct {
    int qn, an;
    char q[256], a[512];
} uw_tags_st;

static int uw_tag_known(const char* const* tags, const size_t* lens, int count, const char* buf, size_t used, const char* p, size_t len)
{
    size_t o = 0;
    int k;
    for (k = 0; k < count; k++)
        if (lens[k] == len && !memcmp(tags[k], p, len))
            return 1;
    while (o < used) {
        size_t l = (unsigned char)buf[o];
        if (l == len && !memcmp(buf + o + 1, p, len))
            return 1;
        o += 1 + l;
    }
    return 0;
}

int dyn_ui_w_tags(dyn_ui_t* ui, const char* s, size_t n, const char* const* tags, const size_t* lens, int count, const char* hint, size_t hn, const char** add,
    size_t* add_n)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id gid = uw_id(ui, s, n), eid;
    uw_tags_st* ts = dyn_ui_state(ui, uw_sub(gid, 30), sizeof(uw_tags_st), NULL);
    dyn_ui_layout box = uw_l(UW_GROW(1), UW_FIT), outer = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style bs;
    float h = uw_lh(ui) + t->pad_y;
    int k, removed = -1, ev = 0;
    size_t qn = 0, i, a, b;
    const char* q;
    *add = NULL;
    *add_n = 0;
    if (!ts)
        return -1;
    dyn_ui_push_id(ui, gid);
    eid = dyn_ui_id_of(ui, "##\x01t", 4);
    dyn_ui_pop_id(ui);
    bs = uw_s(t->frame, ui->w && ui->w->field_error ? t->error : ui->focus == eid ? t->accent
                                                                                  : t->border,
        t->border_w, t->radius);
    outer.dir = DYN_UI_ROW;
    outer.gap = t->pad_x;
    outer.align_y = DYN_UI_CENTER;
    dyn_ui_open(ui, 0, &outer, NULL);
    box.dir = DYN_UI_ROW;
    box.wrap = 1;
    box.gap = uw_px(ui, 4);
    box.align_y = DYN_UI_CENTER;
    uw_pad(&box, uw_px(ui, 4), uw_px(ui, 4));
    dyn_ui_open(ui, gid, &box, &bs);
    for (k = 0; k < count; k++) {
        dyn_ui_id cid = uw_sub(gid, (uint32_t)k), xid = uw_sub(cid, 1);
        uint32_t xb = dyn_ui_interact(ui, xid, 0);
        dyn_ui_layout cl = uw_l(UW_FIT, UW_FIXED(h)), xl = uw_l(UW_FIXED(uw_lh(ui)), UW_FIXED(uw_lh(ui)));
        dyn_ui_style cs = uw_s(t->button, 0, 0, h / 2), xs = uw_s((xb & (DYN_UI_HOT | DYN_UI_ACTIVE)) ? t->button_hot : 0, 0, 0, uw_lh(ui) / 2);
        cl.dir = DYN_UI_ROW;
        cl.align_y = DYN_UI_CENTER;
        cl.gap = uw_px(ui, 2);
        cl.pad_l = t->pad_x;
        cl.pad_r = uw_px(ui, 2);
        dyn_ui_open(ui, cid, &cl, &cs);
        uw_label_fit(ui, tags[k], lens[k], t->text);
        xl.interactive = 1;
        dyn_ui_open(ui, xid, &xl, &xs);
        uw_cross(ui, t->text_dim);
        dyn_ui_close(ui);
        dyn_ui_close(ui);
        if (xb & DYN_UI_CLICKED)
            removed = k;
    }
    if (ui->focus == eid && ts->qn == 0 && ui->fin.key_hit[DYN_UI_KEY_BACKSPACE] && count > 0)
        removed = count - 1;
    {
        dyn_ui_layout el = uw_l(UW_GROW(1), UW_FIT);
        int ferr = ui->w ? ui->w->field_error : 0;
        el.w.min = roundf(6 * uw_lh(ui));
        dyn_ui_open(ui, 0, &el, NULL);
        if (ui->w)
            ui->w->field_error = 0;
        dyn_ui_push_id(ui, gid);
        q = uw_edit(ui, "##\x01t", 4, ts->q, (size_t)ts->qn, 0, (int)sizeof(ts->q) - 1, hint, hn, 1, 0, &qn, &ev, 0);
        dyn_ui_pop_id(ui);
        if (ui->w)
            ui->w->field_error = ferr;
        dyn_ui_close(ui);
    }
    if (qn >= sizeof(ts->q))
        qn = sizeof(ts->q) - 1;
    if (q != ts->q)
        memmove(ts->q, q, qn);
    ts->qn = (int)qn;
    ts->an = 0;
    {
        size_t start = 0, used = 0;
        int submitted = (ev & DYN_UI_EDIT_SUBMITTED) != 0, cut_any = 0;
        for (i = 0; i <= qn; i++) {
            if (i < qn && ts->q[i] != ',')
                continue;
            if (i == qn && !submitted)
                break;
            a = start;
            b = i;
            while (a < b && (ts->q[a] == ' ' || ts->q[a] == '\t'))
                a++;
            while (b > a && (ts->q[b - 1] == ' ' || ts->q[b - 1] == '\t'))
                b--;
            if (b > a && used + 1 + (b - a) <= sizeof(ts->a) && !uw_tag_known(tags, lens, count, ts->a, used, ts->q + a, b - a)) {
                ts->a[used] = (char)(unsigned char)(b - a);
                memcpy(ts->a + used + 1, ts->q + a, b - a);
                used += 1 + (b - a);
            }
            start = i < qn ? i + 1 : qn;
            cut_any = 1;
        }
        if (cut_any) {
            memmove(ts->q, ts->q + start, qn - start);
            ts->qn = (int)(qn - start);
            ui->focus = eid;
        }
        if (used) {
            ts->an = (int)used;
            *add = ts->a;
            *add_n = used;
        }
    }
    dyn_ui_close(ui);
    if (uw_shown(s, n))
        uw_text(ui, s, uw_shown(s, n), t->text_dim);
    dyn_ui_close(ui);
    uw_items(ui, gid, gid, 0);
    return removed;
}

void dyn_ui_w_accordion_begin(dyn_ui_t* ui, const char* s, size_t n)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* w = uw_get(ui);
    dyn_ui_id gid = uw_id(ui, s, n);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT);
    dyn_ui_style st = uw_s(0, t->border, t->border_w, t->radius);
    l.dir = DYN_UI_COLUMN;
    l.gap = uw_px(ui, 1);
    uw_pad(&l, t->border_w, t->border_w);
    dyn_ui_open(ui, uw_sub(gid, 22), &l, &st);
    uw_block_push(ui, uw_sub(gid, 22));
    if (w && w->naccs < 16)
        w->accs[w->naccs] = gid;
    if (w)
        w->naccs++;
    dyn_ui_push_id(ui, gid);
}

static dyn_ui_id uw_acc_top(dyn_ui_t* ui)
{
    struct uw_state* w = ui->w;
    return w && w->naccs > 0 && w->naccs <= 16 ? w->accs[w->naccs - 1] : 0;
}

int dyn_ui_w_accordion_item(dyn_ui_t* ui, int index, const char* s, size_t n, int open)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_id id = uw_sub(uw_acc_top(ui), (uint32_t)index);
    uint32_t bits = dyn_ui_interact(ui, id, 1);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIXED(uw_fh(ui)));
    dyn_ui_style st = uw_s(open ? t->button_active : uw_fill(bits, t->button, t->button_hot, t->button_active), uw_border(ui, id),
        ui->focus == id && ui->focus_visible ? t->border_w : 0, 0);
    l.dir = DYN_UI_ROW;
    l.align_y = DYN_UI_CENTER;
    l.pad_l = t->pad_x / 2;
    l.gap = t->pad_x / 2;
    l.interactive = 1;
    dyn_ui_open(ui, id, &l, &st);
    uw_arrow(ui, open ? 0 : 1, t->text);
    uw_label_fit(ui, s, uw_shown(s, n), t->text);
    dyn_ui_close(ui);
    return uw_activated(ui, id, bits);
}

void dyn_ui_w_accordion_section_begin(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->rt;
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_FIT);
    l.dir = DYN_UI_COLUMN;
    l.gap = t->spacing;
    uw_pad(&l, t->window_pad, t->window_pad);
    dyn_ui_open(ui, 0, &l, NULL);
}

void dyn_ui_w_accordion_section_end(dyn_ui_t* ui)
{
    dyn_ui_close(ui);
}

void dyn_ui_w_accordion_end(dyn_ui_t* ui)
{
    struct uw_state* w = ui->w;
    dyn_ui_id gid = uw_acc_top(ui);
    dyn_ui_pop_id(ui);
    dyn_ui_close(ui);
    uw_block_pop(ui);
    if (w && w->naccs > 0)
        w->naccs--;
    if (gid)
        uw_items(ui, ui->last_id, gid, 0);
}

int dyn_ui_w_scaffold_begin(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, const uint32_t* icons, int count, int selected)
{
    const dyn_ui_theme* t = &ui->rt;
    struct uw_state* w = uw_get(ui);
    dyn_ui_id sid = uw_sub(dyn_ui_id_of(ui, s, n), 22);
    dyn_ui_layout l = uw_l(UW_GROW(1), UW_GROW(1));
    int rail = ui->width < 600 * ui->scale ? 0 : 1;
    if (!w)
        return selected;
    w->scaffold_rail = rail;
    w->scaffold_sel = selected;
    w->scaffold_id = sid;
    l.dir = rail ? DYN_UI_ROW : DYN_UI_COLUMN;
    l.gap = rail ? 0 : t->spacing;
    dyn_ui_open(ui, sid, &l, NULL);
    uw_block_push(ui, sid);
    if (rail) {
        dyn_ui_layout c = uw_l(UW_GROW(1), UW_GROW(1));
        w->scaffold_sel = dyn_ui_w_nav(ui, s, n, items, lens, icons, count, selected, 1);
        c.dir = DYN_UI_COLUMN;
        c.gap = t->spacing;
        c.pad_l = t->window_pad;
        dyn_ui_open(ui, 0, &c, NULL);
    }
    return w->scaffold_sel;
}

int dyn_ui_w_scaffold_end(dyn_ui_t* ui, const char* s, size_t n, const char* const* items, const size_t* lens, const uint32_t* icons, int count, int selected)
{
    struct uw_state* w = ui->w;
    int rail = w ? w->scaffold_rail : 0, sel = w ? w->scaffold_sel : selected;
    dyn_ui_id sid = w ? w->scaffold_id : 0;
    if (rail)
        dyn_ui_close(ui);
    dyn_ui_close(ui);
    if (!rail)
        sel = dyn_ui_w_nav(ui, s, n, items, lens, icons, count, selected, 0);
    uw_block_pop(ui);
    uw_items(ui, sid ? sid : ui->last_id, dyn_ui_id_of(ui, s, n), 0);
    return sel;
}
