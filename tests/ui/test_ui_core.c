/* dyn-ui core: layout numbers derived by hand from the sizing rules, interaction across frames,
   draw-command order, error refusals, state lifetime, and whole-frame rasters compared pixel by
   pixel against frames composed independently here (same blend rule, own placement arithmetic). */
#include "dyn-ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

static const unsigned char proggy[] = {
#include "proggy_clean.inc"
};

static int passes, fails;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (c)                                          \
            passes++;                                   \
        else {                                          \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static dyn_ui_size sz(int kind, float v)
{
    dyn_ui_size s;
    memset(&s, 0, sizeof(s));
    s.kind = (uint8_t)kind;
    s.value = v;
    return s;
}
#define FIT sz(DYN_UI_FIT, 0)
#define GROW(v) sz(DYN_UI_GROW, v)
#define FIXED(v) sz(DYN_UI_FIXED, v)
#define PCT(v) sz(DYN_UI_PERCENT, v)

static dyn_ui_layout lay(dyn_ui_size w, dyn_ui_size h)
{
    dyn_ui_layout l;
    memset(&l, 0, sizeof(l));
    l.w = w;
    l.h = h;
    l.interactive = 1;
    return l;
}

static dyn_ui_style fill(uint32_t c)
{
    dyn_ui_style s;
    memset(&s, 0, sizeof(s));
    s.fill = c;
    return s;
}

static void leaf(dyn_ui_t* ui, dyn_ui_id id, dyn_ui_layout l)
{
    dyn_ui_open(ui, id, &l, NULL);
    dyn_ui_close(ui);
}

static dyn_ui_rect box(dyn_ui_t* ui, dyn_ui_id id)
{
    dyn_ui_rect r = { -1, -1, -1, -1 };
    dyn_ui_box(ui, id, &r);
    return r;
}

static int box_is(dyn_ui_t* ui, dyn_ui_id id, float x, float y, float w, float h)
{
    dyn_ui_rect r = box(ui, id);
    if (r.x == x && r.y == y && r.w == w && r.h == h)
        return 1;
    printf("  box %u is {%g,%g,%g,%g}, expected {%g,%g,%g,%g}\n", id, r.x, r.y, r.w, r.h, x, y, w, h);
    return 0;
}

static void pointer(dyn_ui_t* ui, float x, float y, uint32_t buttons, double t)
{
    dyn_ui_pointer p;
    memset(&p, 0, sizeof(p));
    p.x = x;
    p.y = y;
    p.buttons = buttons;
    p.time = t;
    dyn_ui_input_pointer(ui, &p);
}

static void wheel(dyn_ui_t* ui, float x, float y, float wx, float wy)
{
    dyn_ui_pointer p;
    memset(&p, 0, sizeof(p));
    p.x = x;
    p.y = y;
    p.wheel_x = wx;
    p.wheel_y = wy;
    dyn_ui_input_pointer(ui, &p);
}

static dyn_ui_t* fresh(int* font)
{
    dyn_ui_t* ui = dyn_ui_new();
    *font = dyn_ui_font_add(ui, proggy, sizeof(proggy), 0);
    return ui;
}

static void test_grow_distribution(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout row = lay(GROW(1), GROW(1));
    row.dir = DYN_UI_ROW;
    row.pad_l = row.pad_t = row.pad_r = row.pad_b = 10;
    row.gap = 5;
    dyn_ui_begin(ui, 200, 100);
    dyn_ui_open(ui, 1, &row, NULL);
    leaf(ui, 2, lay(FIXED(30), FIXED(20)));
    leaf(ui, 3, lay(GROW(1), FIXED(20)));
    leaf(ui, 4, lay(GROW(2), FIXED(20)));
    dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == 0, "end: %s", dyn_ui_error_text(ui));
    CHECK(box_is(ui, 1, 0, 0, 200, 100), "row fills the root");
    CHECK(box_is(ui, 2, 10, 10, 30, 20), "fixed child at the padding");
    CHECK(box_is(ui, 3, 45, 10, 47, 20), "grow 1 takes a third of 140, edges rounded");
    CHECK(box_is(ui, 4, 97, 10, 93, 20), "grow 2 takes two thirds and ends at the padding");
    dyn_ui_free(ui);
}

static void test_align_percent_max(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout col = lay(GROW(1), GROW(1));
    dyn_ui_layout row = lay(FIXED(300), FIXED(50));
    dyn_ui_layout capped = lay(GROW(1), FIXED(10));
    col.dir = DYN_UI_COLUMN;
    col.align_x = DYN_UI_CENTER;
    col.align_y = DYN_UI_END;
    row.dir = DYN_UI_ROW;
    capped.w.max = 40;
    dyn_ui_begin(ui, 400, 200);
    dyn_ui_open(ui, 10, &col, NULL);
    leaf(ui, 11, lay(FIXED(50), FIXED(30)));
    dyn_ui_open(ui, 20, &row, NULL);
    leaf(ui, 21, lay(PCT(0.5f), FIXED(10)));
    leaf(ui, 22, capped);
    leaf(ui, 23, lay(GROW(1), FIXED(10)));
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == 0, "end: %s", dyn_ui_error_text(ui));
    CHECK(box_is(ui, 11, 175, 120, 50, 30), "centered across, bottom-aligned along: (400-50)/2, 200-30-50");
    CHECK(box_is(ui, 20, 50, 150, 300, 50), "fixed row centered");
    CHECK(box_is(ui, 21, 50, 150, 150, 10), "50 percent of 300");
    CHECK(box_is(ui, 22, 200, 150, 40, 10), "grow stops at its max");
    CHECK(box_is(ui, 23, 240, 150, 110, 10), "the other grow takes what the capped one left");
    dyn_ui_free(ui);
}

static void test_text_and_wrap(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout col = lay(FIXED(50), FIXED(100));
    dyn_ui_layout wrapbox = lay(FIT, FIT);
    dyn_ui_layout row = lay(FIXED(100), FIT);
    const dyn_ui_cmd* cmds;
    size_t n, i, glyph_cmds = 0, quads = 0;
    CHECK(font == 0, "ProggyClean loads as font 0");
    CHECK(dyn_ui_text_width(ui, font, 13, "Hello", 5) == 35, "13px ProggyClean advances 7 per glyph");
    CHECK(dyn_ui_line_height(ui, font, 13) == 13, "line height 13");
    CHECK(dyn_ui_ascent(ui, font, 13) == 10, "ascent 9.5 rounds to 10");
    col.dir = DYN_UI_COLUMN;
    row.dir = DYN_UI_ROW;
    dyn_ui_begin(ui, 400, 200);
    dyn_ui_open(ui, 30, &col, NULL);
    dyn_ui_open(ui, 31, &wrapbox, NULL);
    dyn_ui_text(ui, 0, "aaa bbb ccc", 11, font, 13, 0xFFFFFFFFu, 1, DYN_UI_START);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 40, &row, NULL);
    dyn_ui_open(ui, 41, &wrapbox, NULL);
    dyn_ui_text(ui, 0, "aaaa bbbb", 9, font, 13, 0xFFFFFFFFu, 1, DYN_UI_START);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 42, &wrapbox, NULL);
    dyn_ui_text(ui, 0, "aaaa bbbb", 9, font, 13, 0xFFFFFFFFu, 1, DYN_UI_START);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == 0, "end: %s", dyn_ui_error_text(ui));
    CHECK(box_is(ui, 31, 0, 0, 50, 26), "77px of text capped to 50 wraps after 'aaa bbb' (49px): two lines");
    CHECK(box_is(ui, 41, 0, 100, 50, 26), "two 63px texts in 100: each shrinks by 13 toward its 28px word, wraps");
    CHECK(box_is(ui, 42, 50, 100, 50, 26), "the second one follows");
    cmds = dyn_ui_commands(ui, &n);
    for (i = 0; i < n; i++)
        if (cmds[i].type == DYN_UI_CMD_GLYPHS) {
            glyph_cmds++;
            quads += cmds[i].count;
        }
    CHECK(glyph_cmds == 3 && quads == 9 + 8 + 8, "spaces draw nothing: %zu runs, %zu quads", glyph_cmds, quads);
    dyn_ui_free(ui);
}

static void scroll_scene(dyn_ui_t* ui)
{
    dyn_ui_layout col = lay(FIXED(100), FIXED(50));
    dyn_ui_layout bar = lay(FIXED(4), FIXED(10));
    dyn_ui_id k;
    col.dir = DYN_UI_COLUMN;
    col.clip_y = 1;
    bar.floating = DYN_UI_FLOAT_PARENT;
    bar.float_x = 96;
    dyn_ui_open(ui, 50, &col, NULL);
    for (k = 0; k < 5; k++)
        leaf(ui, 51 + k, lay(FIXED(100), FIXED(20)));
    leaf(ui, 59, bar);
    dyn_ui_close(ui);
}

static void test_scroll(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_scroll_view v;
    float y = 1000;
    const dyn_ui_cmd* cmds;
    size_t n;
    dyn_ui_begin(ui, 200, 200);
    dyn_ui_scroll(ui, 50, NULL, &y);
    scroll_scene(ui);
    dyn_ui_end(ui, NULL);
    CHECK(box_is(ui, 53, 0, -10, 100, 20), "scroll clamped to content 100 - view 50: third row at 40-50");
    CHECK(box_is(ui, 59, 96, 0, 4, 10), "a PARENT float is placed on the box and does not scroll");
    CHECK(dyn_ui_scroll_info(ui, 50, &v) == 0 && v.y == 50 && v.content_h == 100 && v.view_h == 50, "scroll info %g %g %g", v.y,
        v.content_h, v.view_h);
    cmds = dyn_ui_commands(ui, &n);
    CHECK(n >= 2 && cmds[0].type == DYN_UI_CMD_CLIP && cmds[0].rect.x == 0 && cmds[0].rect.w == 100 && cmds[0].rect.h == 50
            && cmds[n - 1].type == DYN_UI_CMD_UNCLIP,
        "clip wraps the children");
    wheel(ui, 10, 10, 0, 1);
    dyn_ui_begin(ui, 200, 200);
    scroll_scene(ui);
    dyn_ui_end(ui, NULL);
    CHECK(box_is(ui, 53, 0, 29, 100, 20), "one wheel notch scrolls 3 lines (39 px at 13 px) back: 40 - (50 - 39)");
    dyn_ui_free(ui);
}

static void nested_scroll_scene(dyn_ui_t* ui)
{
    dyn_ui_layout outer = lay(FIXED(100), FIXED(50));
    dyn_ui_layout inner = lay(FIXED(100), FIXED(30));
    outer.dir = DYN_UI_COLUMN;
    outer.clip_y = 1;
    inner.dir = DYN_UI_ROW;
    inner.clip_x = 1;
    dyn_ui_open(ui, 60, &outer, NULL);
    dyn_ui_open(ui, 61, &inner, NULL);
    leaf(ui, 62, lay(FIXED(300), FIXED(30)));
    dyn_ui_close(ui);
    leaf(ui, 63, lay(FIXED(100), FIXED(100)));
    dyn_ui_close(ui);
}

static void test_nested_scroll(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_scroll_view a, b;
    dyn_ui_begin(ui, 200, 200);
    nested_scroll_scene(ui);
    dyn_ui_end(ui, NULL);
    wheel(ui, 10, 10, -1, 0);
    dyn_ui_begin(ui, 200, 200);
    nested_scroll_scene(ui);
    dyn_ui_end(ui, NULL);
    dyn_ui_scroll_info(ui, 60, &a);
    dyn_ui_scroll_info(ui, 61, &b);
    CHECK(a.y == 0 && b.x == 39, "a horizontal wheel goes to the inner, horizontal-only scroller: %g %g", a.y, b.x);
    wheel(ui, 10, 10, 0, -1);
    dyn_ui_begin(ui, 200, 200);
    nested_scroll_scene(ui);
    dyn_ui_end(ui, NULL);
    dyn_ui_scroll_info(ui, 60, &a);
    dyn_ui_scroll_info(ui, 61, &b);
    CHECK(a.y == 39 && b.x == 39, "a vertical wheel over it passes through to the outer one: %g %g", a.y, b.x);
    CHECK(box_is(ui, 62, -39, -39, 300, 30), "inner content moves by both offsets");
    dyn_ui_free(ui);
}

static void test_floating(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout parent = lay(FIXED(50), FIXED(50));
    dyn_ui_layout a = lay(FIXED(10), FIXED(10)), b = lay(FIXED(10), FIXED(10)), pop = lay(FIXED(20), FIXED(5));
    dyn_ui_layout shade = lay(GROW(1), GROW(1));
    dyn_ui_style s1 = fill(0x111111FFu), s2 = fill(0x222222FFu), s3 = fill(0x333333FFu), s4 = fill(0x444444FFu);
    const dyn_ui_cmd* cmds;
    size_t n;
    parent.pad_l = 7;
    a.floating = b.floating = shade.floating = DYN_UI_FLOAT_ROOT;
    a.float_x = 5;
    a.float_y = 6;
    a.z = 5;
    b.z = 1;
    pop.floating = DYN_UI_FLOAT_POPUP;
    pop.float_x = 3;
    pop.float_y = 50;
    shade.z = 9;
    dyn_ui_begin(ui, 120, 80);
    leaf(ui, 69, lay(FIXED(30), FIXED(30)));
    dyn_ui_open(ui, 70, &parent, &s1);
    dyn_ui_open(ui, 71, &a, &s2);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 72, &b, &s3);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 73, &pop, &s4);
    dyn_ui_close(ui);
    leaf(ui, 74, shade);
    dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == 0, "end: %s", dyn_ui_error_text(ui));
    CHECK(box_is(ui, 70, 0, 30, 50, 50), "flow box after a 30px sibling");
    CHECK(box_is(ui, 71, 5, 6, 10, 10), "ROOT float is absolute");
    CHECK(box_is(ui, 73, 3, 75, 20, 5), "POPUP float is placed from its parent's box (3, 30+50) and then kept on screen: 80-5");
    CHECK(box_is(ui, 74, 0, 0, 120, 80), "a GROW ROOT float covers the screen");
    cmds = dyn_ui_commands(ui, &n);
    CHECK(n == 4 && cmds[0].color == 0x111111FFu && cmds[1].color == 0x444444FFu && cmds[2].color == 0x333333FFu
            && cmds[3].color == 0x222222FFu,
        "the tree first, then floats by z: popup(0), b(1), a(5); n=%zu", n);
    dyn_ui_free(ui);
}

static uint32_t press_scene(dyn_ui_t* ui, uint32_t* other, int with_shade)
{
    dyn_ui_layout row = lay(FIT, FIT), shade = lay(GROW(1), GROW(1));
    uint32_t bits;
    row.dir = DYN_UI_ROW;
    row.interactive = 0;
    shade.floating = DYN_UI_FLOAT_ROOT;
    shade.interactive = 0;
    dyn_ui_open(ui, 99, &row, NULL);
    leaf(ui, 100, lay(FIXED(20), FIXED(20)));
    leaf(ui, 101, lay(FIXED(20), FIXED(20)));
    dyn_ui_close(ui);
    if (with_shade)
        leaf(ui, 0, shade);
    bits = dyn_ui_interact(ui, 100, 1);
    if (other)
        *other = dyn_ui_interact(ui, 101, 1);
    return bits;
}

static uint32_t step(dyn_ui_t* ui, uint32_t* other, int with_shade)
{
    uint32_t bits;
    dyn_ui_begin(ui, 100, 100);
    bits = press_scene(ui, other, with_shade);
    dyn_ui_end(ui, NULL);
    return bits;
}

static void test_interaction(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    uint32_t b, o;
    b = step(ui, &o, 0);
    CHECK(b == 0 && o == 0, "nothing is hot before a layout exists");
    pointer(ui, 5, 5, 0, 1.0);
    b = step(ui, &o, 0);
    CHECK(b == DYN_UI_HOT && o == 0, "hover: %x %x", b, o);
    pointer(ui, 5, 5, 1, 1.1);
    b = step(ui, &o, 0);
    CHECK(b == (DYN_UI_HOT | DYN_UI_PRESSED | DYN_UI_ACTIVE | DYN_UI_FOCUSED), "press: %x", b);
    pointer(ui, 25, 5, 1, 1.2);
    b = step(ui, &o, 0);
    CHECK(b == (DYN_UI_ACTIVE | DYN_UI_FOCUSED) && o == 0, "drag off: the pressed one stays active, the other is not hot: %x %x", b, o);
    pointer(ui, 25, 5, 0, 1.3);
    b = step(ui, &o, 0);
    CHECK(b == (DYN_UI_RELEASED | DYN_UI_FOCUSED), "release elsewhere: released, not active, no click: %x", b);
    CHECK(dyn_ui_active(ui) == 0, "release clears active");
    b = step(ui, &o, 0);
    CHECK(o == DYN_UI_HOT && b == DYN_UI_FOCUSED, "after release the other one is hot again: %x %x", b, o);
    pointer(ui, 5, 5, 0, 2.0);
    pointer(ui, 5, 5, 1, 2.0);
    pointer(ui, 5, 5, 0, 2.01);
    b = step(ui, NULL, 0);
    CHECK((b & (DYN_UI_PRESSED | DYN_UI_RELEASED | DYN_UI_CLICKED)) == (DYN_UI_PRESSED | DYN_UI_RELEASED | DYN_UI_CLICKED) && !(b & DYN_UI_DOUBLE),
        "press and release inside one frame is a click: %x", b);
    pointer(ui, 5, 5, 1, 2.2);
    pointer(ui, 5, 5, 0, 2.21);
    b = step(ui, NULL, 0);
    CHECK((b & DYN_UI_CLICKED) && (b & DYN_UI_DOUBLE), "a second click within 0.35 s is a double: %x", b);
    pointer(ui, 5, 5, 1, 2.3);
    pointer(ui, 5, 5, 0, 2.31);
    b = step(ui, NULL, 0);
    CHECK((b & DYN_UI_CLICKED) && !(b & DYN_UI_DOUBLE), "the third click starts over: %x", b);
    pointer(ui, 5, 5, 1, 3.0);
    pointer(ui, 5, 5, 0, 3.0);
    b = step(ui, NULL, 0);
    pointer(ui, 5, 5, 1, 3.5);
    pointer(ui, 5, 5, 0, 3.5);
    b = step(ui, NULL, 0);
    CHECK((b & DYN_UI_CLICKED) && !(b & DYN_UI_DOUBLE), "0.5 s apart is two clicks: %x", b);
    pointer(ui, 90, 90, 1, 4.0);
    pointer(ui, 90, 90, 0, 4.0);
    b = step(ui, NULL, 0);
    CHECK(b == 0 && dyn_ui_focus(ui) == 0, "pressing empty space clears focus: %x", b);
    pointer(ui, 5, 5, 0, 5.0);
    step(ui, NULL, 1);
    b = step(ui, NULL, 1);
    CHECK(b == 0, "a ROOT float over the button blocks it although the float is not interactive: %x", b);
    dyn_ui_free(ui);
    ui = fresh(&font);
    step(ui, NULL, 0);
    pointer(ui, 5, 5, 1, 0);
    pointer(ui, 5, 5, 0, 0);
    step(ui, NULL, 0);
    pointer(ui, 5, 5, 1, 0);
    pointer(ui, 5, 5, 0, 0);
    b = step(ui, NULL, 0);
    CHECK((b & DYN_UI_CLICKED) && !(b & DYN_UI_DOUBLE), "without timestamps there is no double click: %x", b);
    dyn_ui_free(ui);
}

static void test_command_order(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout l = lay(FIXED(40), FIXED(40));
    dyn_ui_style s = fill(0xFF0000FFu);
    const dyn_ui_cmd* c;
    size_t n;
    int changed = -1;
    int k;
    l.clip_x = l.clip_y = 1;
    s.border = 0x0000FFFFu;
    s.border_w = 2;
    for (k = 0; k < 3; k++) {
        dyn_ui_begin(ui, 100, 100);
        dyn_ui_open(ui, 200, &l, &s);
        {
            dyn_ui_style cs = fill(k == 2 ? 0x00FF00FEu : 0x00FF00FFu);
            dyn_ui_layout cl = lay(FIXED(10), FIXED(10));
            dyn_ui_open(ui, 201, &cl, &cs);
            dyn_ui_close(ui);
        }
        dyn_ui_close(ui);
        dyn_ui_end(ui, &changed);
        if (k == 0)
            CHECK(changed == 1, "the first frame is a change");
        if (k == 1)
            CHECK(changed == 0, "an identical frame is not");
        if (k == 2)
            CHECK(changed == 1, "one alpha step in one fill is");
    }
    c = dyn_ui_commands(ui, &n);
    CHECK(n == 5 && c[0].type == DYN_UI_CMD_RECT && c[1].type == DYN_UI_CMD_CLIP && c[2].type == DYN_UI_CMD_RECT
            && c[3].type == DYN_UI_CMD_UNCLIP && c[4].type == DYN_UI_CMD_BORDER && c[4].width == 2,
        "fill, clip, child, unclip, border last");
    dyn_ui_free(ui);
}

static void test_errors(void)
{
    int font, i;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout l = lay(FIT, FIT);
    dyn_ui_vertex v[4];
    memset(v, 0, sizeof(v));
    CHECK(dyn_ui_open(ui, 1, &l, NULL) != 0, "open outside a frame refuses");
    CHECK(dyn_ui_end(ui, NULL) == DYN_UI_ERR_NESTING, "end without begin refuses");
    dyn_ui_begin(ui, 10, 10);
    CHECK(dyn_ui_begin(ui, 10, 10) == DYN_UI_ERR_NESTING, "begin inside a frame refuses");
    dyn_ui_open(ui, 1, &l, NULL);
    CHECK(dyn_ui_end(ui, NULL) == DYN_UI_ERR_NESTING, "end with an open element refuses");
    CHECK(strstr(dyn_ui_error_text(ui), "still open") != NULL, "and says so: %s", dyn_ui_error_text(ui));
    dyn_ui_begin(ui, 10, 10);
    dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == DYN_UI_ERR_NESTING && strstr(dyn_ui_error_text(ui), "without a matching open"), "close of the root: %s",
        dyn_ui_error_text(ui));
    dyn_ui_begin(ui, 10, 10);
    dyn_ui_push_id(ui, 7);
    CHECK(dyn_ui_end(ui, NULL) == DYN_UI_ERR_NESTING && strstr(dyn_ui_error_text(ui), "pushId"), "unbalanced pushId: %s",
        dyn_ui_error_text(ui));
    dyn_ui_begin(ui, 10, 10);
    dyn_ui_pop_id(ui);
    CHECK(dyn_ui_end(ui, NULL) == DYN_UI_ERR_NESTING, "popId without push");
    dyn_ui_begin(ui, 10, 10);
    CHECK(dyn_ui_text(ui, 0, "x", 1, 5, 13, 0xFFu, 0, 0) == DYN_UI_ERR_FONT, "text with an unknown font");
    dyn_ui_end(ui, NULL);
    dyn_ui_begin(ui, 10, 10);
    CHECK(dyn_ui_text(ui, 0, "x", 1, font, 0.5f, 0xFFu, 0, 0) == DYN_UI_ERR_FONT, "text below 1px");
    dyn_ui_end(ui, NULL);
    dyn_ui_begin(ui, 10, 10);
    CHECK(dyn_ui_triangles(ui, v, 3, 0xFFu) == DYN_UI_ERR_NESTING, "triangles need an open element");
    dyn_ui_end(ui, NULL);
    dyn_ui_begin(ui, 10, 10);
    dyn_ui_open(ui, 1, &l, NULL);
    CHECK(dyn_ui_triangles(ui, v, 4, 0xFFu) == DYN_UI_ERR_LIMIT, "4 vertices is not triangles");
    dyn_ui_close(ui);
    dyn_ui_end(ui, NULL);
    dyn_ui_begin(ui, 10, 10);
    for (i = 0; i < 300; i++)
        dyn_ui_open(ui, (dyn_ui_id)(i + 1), &l, NULL);
    CHECK(dyn_ui_error(ui) == DYN_UI_ERR_NESTING && strstr(dyn_ui_error_text(ui), "256"), "depth limit: %s", dyn_ui_error_text(ui));
    for (i = 0; i < 300; i++)
        dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == DYN_UI_ERR_NESTING, "the first error is the one kept");
    CHECK(dyn_ui_begin(ui, 0, 10) == DYN_UI_ERR_LIMIT && dyn_ui_begin(ui, NAN, 10) == DYN_UI_ERR_LIMIT
            && dyn_ui_begin(ui, 16385, 10) == DYN_UI_ERR_LIMIT,
        "frame sizes outside 1..16384 refuse");
    CHECK(dyn_ui_begin(ui, 10, 10) == 0 && dyn_ui_end(ui, NULL) == 0 && dyn_ui_error(ui) == 0, "a clean frame clears the error");
    CHECK(dyn_ui_font_add(ui, (const uint8_t*)"not a font at all", 17, 1) < 0, "a non-font is refused");
    {
        uint8_t bad[64];
        memcpy(bad, proggy, sizeof(bad));
        CHECK(dyn_ui_font_add(ui, bad, sizeof(bad), 1) < 0, "a font whose table directory points past its end is refused");
    }
    dyn_ui_free(ui);
}

static void test_state(void)
{
    int font, f1, f2, f3;
    dyn_ui_t* ui = fresh(&font);
    int* p;
    int k;
    dyn_ui_begin(ui, 10, 10);
    p = dyn_ui_state(ui, 77, sizeof(int), &f1);
    *p = 42;
    dyn_ui_end(ui, NULL);
    dyn_ui_begin(ui, 10, 10);
    p = dyn_ui_state(ui, 77, sizeof(int), &f2);
    dyn_ui_end(ui, NULL);
    CHECK(f1 == 1 && f2 == 0 && *p == 42, "state persists while it is used");
    for (k = 0; k < 4; k++) {
        dyn_ui_begin(ui, 10, 10);
        dyn_ui_end(ui, NULL);
    }
    dyn_ui_begin(ui, 10, 10);
    p = dyn_ui_state(ui, 77, sizeof(int), &f3);
    dyn_ui_end(ui, NULL);
    CHECK(f3 == 1 && *p == 0, "state not used for three frames is dropped and comes back zeroed");
    for (k = 0; k < 5000; k++) {
        dyn_ui_begin(ui, 10, 10);
        dyn_ui_state(ui, (dyn_ui_id)(1000 + k), 64, NULL);
        dyn_ui_end(ui, NULL);
    }
    dyn_ui_free(ui);
}

typedef struct {
    int w, h;
    uint8_t* px;
    uint8_t* band;
} frame;

static frame frame_new(int w, int h, uint32_t clear)
{
    frame f;
    int i;
    f.w = w;
    f.h = h;
    f.px = malloc((size_t)w * h * 4);
    f.band = calloc((size_t)w * h, 1);
    for (i = 0; i < w * h; i++) {
        f.px[i * 4] = (uint8_t)(clear >> 24);
        f.px[i * 4 + 1] = (uint8_t)(clear >> 16);
        f.px[i * 4 + 2] = (uint8_t)(clear >> 8);
        f.px[i * 4 + 3] = (uint8_t)clear;
    }
    return f;
}

static void put(frame* f, int x, int y, uint32_t c, unsigned cov)
{
    uint8_t* p;
    unsigned a = ((c & 0xFF) * cov + 127) / 255, ia = 255 - a;
    if (x < 0 || y < 0 || x >= f->w || y >= f->h || a == 0)
        return;
    p = f->px + ((size_t)y * f->w + x) * 4;
    p[0] = (uint8_t)(((c >> 24) * a + p[0] * ia + 127) / 255);
    p[1] = (uint8_t)((((c >> 16) & 0xFF) * a + p[1] * ia + 127) / 255);
    p[2] = (uint8_t)((((c >> 8) & 0xFF) * a + p[2] * ia + 127) / 255);
    p[3] = (uint8_t)(a + (p[3] * ia + 127) / 255);
}

static void rect(frame* f, int x0, int y0, int w, int h, uint32_t c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            put(f, x, y, c, 255);
}

static int compare(const frame* want, const uint8_t* got, const char* what)
{
    int x, y, bad = 0, shown = 0;
    for (y = 0; y < want->h; y++)
        for (x = 0; x < want->w; x++) {
            const uint8_t* a = want->px + ((size_t)y * want->w + x) * 4;
            const uint8_t* b = got + ((size_t)y * want->w + x) * 4;
            int ok = 1, ch;
            if (want->band[y * want->w + x]) {
                continue;
            }
            for (ch = 0; ch < 4; ch++)
                if (a[ch] != b[ch])
                    ok = 0;
            if (!ok) {
                bad++;
                if (shown++ < 5)
                    printf("  %s (%d,%d): got %02x%02x%02x%02x want %02x%02x%02x%02x\n", what, x, y, b[0], b[1], b[2], b[3], a[0], a[1], a[2],
                        a[3]);
            }
        }
    if (bad) {
        printf("  %s: %d pixels differ; map (x = differs, ~ = band):\n", what, bad);
        for (y = 0; y < want->h; y++) {
            printf("  ");
            for (x = 0; x < want->w; x++) {
                const uint8_t* a = want->px + ((size_t)y * want->w + x) * 4;
                const uint8_t* b = got + ((size_t)y * want->w + x) * 4;
                putchar(want->band[y * want->w + x] ? '~' : memcmp(a, b, 4) ? 'x'
                                                                            : '.');
            }
            putchar('\n');
        }
    }
    return bad;
}

static void band_between(frame* f, const uint8_t* got, int x, int y, uint32_t bg, uint32_t fg, int* bad)
{
    const uint8_t* g = got + ((size_t)y * f->w + x) * 4;
    int ch;
    for (ch = 0; ch < 3; ch++) {
        int lo = (int)((bg >> (24 - 8 * ch)) & 0xFF), hi = (int)((fg >> (24 - 8 * ch)) & 0xFF), t;
        if (lo > hi) {
            t = lo;
            lo = hi;
            hi = t;
        }
        if (g[ch] < lo || g[ch] > hi)
            (*bad)++;
    }
    f->band[y * f->w + x] = 1;
}

static void test_raster_whole_frame(void)
{
    int font, img, x, y, gx, gy, bad = 0;
    dyn_ui_t* ui = fresh(&font);
    const int W = 72, H = 48;
    const uint32_t BG = 0x102030FFu, RED = 0xFF0000FFu, GREEN = 0x00FF00FFu, BLUE = 0x0000FFFFu, HALF = 0xFFFFFF80u, YEL = 0xFFFF00FFu;
    uint8_t img_px[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    uint8_t* got = malloc((size_t)W * H * 4);
    frame want = frame_new(W, H, BG);
    dyn_ui_layout a = lay(FIXED(20), FIXED(10)), b = a, c = lay(FIXED(20), FIXED(12)), d = lay(FIXED(30), FIXED(30)), e = lay(FIT, FIT),
                  im = lay(FIXED(8), FIXED(8)), child = lay(FIXED(30), FIXED(30));
    dyn_ui_style sa = fill(RED), sb = fill(0), sc = fill(BLUE), sh = fill(HALF);
    dyn_ui_vertex tri[3] = { { 0, 0 }, { 30, 0 }, { 0, 30 } };
    stbtt_fontinfo fi;
    float scale;
    int pen;
    const char* hi = "Hi";
    a.floating = b.floating = c.floating = d.floating = e.floating = im.floating = DYN_UI_FLOAT_ROOT;
    a.float_x = 2, a.float_y = 2;
    b.float_x = 30, b.float_y = 2;
    c.float_x = 2, c.float_y = 16;
    c.clip_x = c.clip_y = 1;
    d.float_x = 40, d.float_y = 16;
    e.float_x = 2, e.float_y = 32;
    im.float_x = 60, im.float_y = 2;
    sb.border = GREEN;
    sb.border_w = 2;
    img = dyn_ui_image_add(ui, 2, 2, img_px);
    CHECK(img > 0, "image registered");
    dyn_ui_begin(ui, (float)W, (float)H);
    dyn_ui_open(ui, 1, &a, &sa);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 2, &b, &sb);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 0, &c, &sc);
    dyn_ui_open(ui, 4, &child, &sh);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 5, &d, NULL);
    dyn_ui_triangles(ui, tri, 3, YEL);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 6, &e, NULL);
    dyn_ui_text(ui, 0, hi, 2, font, 13, 0xFFFFFFFFu, 0, DYN_UI_START);
    dyn_ui_close(ui);
    dyn_ui_image(ui, 7, (uint32_t)img, &im, 0xFFFFFFFFu);
    CHECK(dyn_ui_end(ui, NULL) == 0, "end: %s", dyn_ui_error_text(ui));
    CHECK(dyn_ui_raster(ui, got, W, H, (size_t)W * 4, BG) == 0, "raster");
    rect(&want, 2, 2, 20, 10, RED);
    rect(&want, 30, 2, 20, 2, GREEN);
    rect(&want, 30, 10, 20, 2, GREEN);
    rect(&want, 30, 4, 2, 6, GREEN);
    rect(&want, 48, 4, 2, 6, GREEN);
    rect(&want, 2, 16, 20, 12, BLUE);
    rect(&want, 2, 16, 20, 12, HALF);
    for (y = 16; y < 46; y++)
        for (x = 40; x < 70; x++) {
            int lx = x - 40, ly = y - 16;
            if (lx + ly + 2 <= 30)
                put(&want, x, y, YEL, 255);
            else if (lx + ly >= 30)
                ;
            else
                band_between(&want, got, x, y, BG, YEL, &bad);
        }
    CHECK(bad == 0, "triangle edge pixels are blends of the two colours (%d channels outside)", bad);
    stbtt_InitFont(&fi, proggy, 0);
    {
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
        scale = 13.0f / (float)(asc - desc);
        pen = 2;
        for (; *hi; hi++) {
            int w, h, xo, yo, adv, lsb;
            unsigned char* bm = stbtt_GetCodepointBitmap(&fi, scale, scale, *hi, &w, &h, &xo, &yo);
            for (gy = 0; gy < h; gy++)
                for (gx = 0; gx < w; gx++)
                    put(&want, pen + xo + gx, 32 + (int)roundf((float)asc * scale) + yo + gy, 0xFFFFFFFFu, bm[gy * w + gx]);
            stbtt_FreeBitmap(bm, NULL);
            stbtt_GetCodepointHMetrics(&fi, *hi, &adv, &lsb);
            pen += (int)roundf((float)adv * scale);
        }
    }
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            const uint8_t* s = img_px + ((y / 4) * 2 + (x / 4)) * 4;
            put(&want, 60 + x, 2 + y, (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3], 255);
        }
    CHECK(compare(&want, got, "whole frame") == 0, "every pixel of the frame matches the independently composed one");
    free(want.px);
    free(want.band);
    free(got);
    dyn_ui_free(ui);
}

static void test_rounded_and_clip_nesting(void)
{
    int font, x, y, bad = 0;
    dyn_ui_t* ui = fresh(&font);
    const int W = 40, H = 40;
    const uint32_t BG = 0x000000FFu, FG = 0xFFFFFFFFu;
    uint8_t* got = malloc((size_t)W * H * 4);
    frame want = frame_new(W, H, BG);
    dyn_ui_layout r = lay(FIXED(30), FIXED(30)), outer = lay(FIXED(40), FIXED(10)), inner = lay(FIXED(30), FIXED(30)),
                  deep = lay(FIXED(40), FIXED(40));
    dyn_ui_style s = fill(FG), none = fill(0), red = fill(0xFF0000FFu);
    s.radius = 8;
    r.floating = DYN_UI_FLOAT_ROOT;
    r.float_x = 5;
    r.float_y = 5;
    dyn_ui_begin(ui, (float)W, (float)H);
    dyn_ui_open(ui, 1, &r, &s);
    dyn_ui_close(ui);
    dyn_ui_end(ui, NULL);
    dyn_ui_raster(ui, got, W, H, (size_t)W * 4, BG);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float cx = px < 13 ? 13 : px > 27 ? 27
                                              : px,
                  cy = py < 13 ? 13 : py > 27 ? 27
                                              : py;
            float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - 8;
            int in_box = x >= 5 && x < 35 && y >= 5 && y < 35;
            if (!in_box || d >= 0.5f)
                continue;
            if (d <= -0.5f)
                put(&want, x, y, FG, 255);
            else
                band_between(&want, got, x, y, BG, FG, &bad);
        }
    CHECK(bad == 0 && compare(&want, got, "rounded rect") == 0, "rounded corners: inside exact, outside untouched, edge a blend");
    free(want.px);
    free(want.band);
    want = frame_new(W, H, BG);
    outer.clip_x = outer.clip_y = 1;
    inner.clip_x = inner.clip_y = 1;
    dyn_ui_begin(ui, (float)W, (float)H);
    dyn_ui_open(ui, 0, &outer, &none);
    dyn_ui_open(ui, 0, &inner, &none);
    dyn_ui_open(ui, 12, &deep, &red);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    dyn_ui_close(ui);
    dyn_ui_end(ui, NULL);
    dyn_ui_raster(ui, got, W, H, (size_t)W * 4, BG);
    rect(&want, 0, 0, 30, 10, 0xFF0000FFu);
    CHECK(compare(&want, got, "nested clip") == 0, "nested clips intersect: 40x40 red shows only 30x10");
    free(want.px);
    free(want.band);
    free(got);
    dyn_ui_free(ui);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void)
{
    uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static float rf(void)
{
    static const float pick[] = { 0, 1, 3, 7.5f, 20, 100, 1e5f, -5, NAN, INFINITY, -INFINITY, 0.5f, 1e9f };
    return pick[next() % (sizeof(pick) / sizeof(pick[0]))];
}
static dyn_ui_size rsize(void)
{
    dyn_ui_size s = sz((int)(next() % 5), rf());
    if (next() % 4 == 0)
        s.min = rf();
    if (next() % 4 == 0)
        s.max = rf();
    return s;
}

static void random_tree(dyn_ui_t* ui, int font, int depth, int* count)
{
    int n = (int)(next() % 5), i;
    for (i = 0; i < n && *count < 3000; i++) {
        uint64_t kind = next() % 10;
        (*count)++;
        if (kind < 6 && depth < 7) {
            dyn_ui_layout l = lay(rsize(), rsize());
            dyn_ui_style s = fill((uint32_t)next());
            l.dir = (uint8_t)(next() % 3);
            l.align_x = (uint8_t)(next() % 4);
            l.align_y = (uint8_t)(next() % 4);
            l.clip_x = (uint8_t)(next() % 2);
            l.clip_y = (uint8_t)(next() % 2);
            l.floating = (uint8_t)(next() % 6 == 0 ? next() % 5 : 0);
            l.pad_l = rf();
            l.pad_t = rf();
            l.gap = rf();
            l.float_x = rf();
            l.float_y = rf();
            l.z = (int32_t)(next() % 7);
            l.interactive = (uint8_t)(next() % 2);
            s.border = (uint32_t)next();
            s.border_w = next() % 3 ? 0 : rf();
            s.radius = next() % 3 ? 0 : rf();
            dyn_ui_open(ui, (dyn_ui_id)next(), &l, &s);
            random_tree(ui, font, depth + 1, count);
            dyn_ui_close(ui);
        } else if (kind < 8) {
            static const char* texts[] = { "", "a", "hello world", "a b c d e f g h i j k l m n o p", "line\nbreak\n\n", "\xff\xfe\xc3\x28 bad utf8",
                "\xe4\xb8\xad\xe6\x96\x87 no glyphs", "toolongwordwithoutanyspaceatallxxxxxxxxxxxxxxxxxxxxxx" };
            const char* t = texts[next() % 8];
            dyn_ui_text(ui, (dyn_ui_id)next(), t, strlen(t), font, (float)(1 + next() % 40), (uint32_t)next(), (int)(next() % 2),
                (uint8_t)(next() % 3));
        } else if (kind < 9 && depth > 0) {
            dyn_ui_vertex v[6];
            int k;
            for (k = 0; k < 6; k++) {
                v[k].x = rf();
                v[k].y = rf();
            }
            dyn_ui_triangles(ui, v, 6, (uint32_t)next());
        }
    }
}

static int finite_cmds(dyn_ui_t* ui)
{
    size_t n, i;
    const dyn_ui_cmd* c = dyn_ui_commands(ui, &n);
    for (i = 0; i < n; i++)
        if (!isfinite(c[i].rect.x) || !isfinite(c[i].rect.y) || !isfinite(c[i].rect.w) || !isfinite(c[i].rect.h) || c[i].rect.w < 0 || c[i].rect.h < 0)
            return 0;
    return 1;
}

static void test_random_trees(void)
{
    int font, round, nonfinite = 0, errors = 0;
    dyn_ui_t* ui = fresh(&font);
    uint8_t* px = malloc(96 * 64 * 4);
    for (round = 0; round < 1500; round++) {
        int count = 0;
        dyn_ui_pointer p;
        memset(&p, 0, sizeof(p));
        p.x = (float)(next() % 96);
        p.y = (float)(next() % 64);
        p.buttons = (uint32_t)(next() % 2);
        p.wheel_y = (float)(next() % 3) - 1;
        dyn_ui_input_pointer(ui, &p);
        dyn_ui_begin(ui, 96, 64);
        random_tree(ui, font, 0, &count);
        if (dyn_ui_end(ui, NULL))
            errors++;
        if (!finite_cmds(ui))
            nonfinite++;
        dyn_ui_raster(ui, px, 96, 64, 96 * 4, 0x000000FFu);
    }
    CHECK(nonfinite == 0, "%d frames emitted a non-finite or negative rectangle", nonfinite);
    CHECK(errors < 1500, "most random frames are valid (%d refused)", errors);
    free(px);
    dyn_ui_free(ui);
}

static void test_atlas_full_recovers(void)
{
    int font, px, err = 0;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_atlas_view v;
    const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    dyn_ui_begin(ui, 100, 100);
    for (px = 1; px <= 512; px++)
        dyn_ui_text(ui, 0, abc, strlen(abc), font, (float)px, 0xFFFFFFFFu, 0, 0);
    err = dyn_ui_end(ui, NULL);
    CHECK(err == DYN_UI_ERR_LIMIT && strstr(dyn_ui_error_text(ui), "atlas"), "a frame that overfills the atlas says so: %s", dyn_ui_error_text(ui));
    v = dyn_ui_atlas(ui);
    CHECK(v.height <= 8192, "the atlas never grows past 8192 rows (%d)", v.height);
    dyn_ui_begin(ui, 100, 100);
    dyn_ui_text(ui, 0, "ok", 2, font, 13, 0xFFFFFFFFu, 0, 0);
    CHECK(dyn_ui_end(ui, NULL) == 0, "the next frame rebuilds it: %s", dyn_ui_error_text(ui));
    v = dyn_ui_atlas(ui);
    CHECK(v.height <= 128, "and it is small again (%d rows)", v.height);
    dyn_ui_free(ui);
}

static void test_gaps(void)
{
    int font, changed = -1, k;
    dyn_ui_t* ui = fresh(&font);
    const dyn_ui_cmd* c;
    size_t n;
    dyn_ui_layout f1 = lay(FIXED(5), FIXED(5)), f2 = f1, f3 = f1;
    dyn_ui_style a = fill(0xA0A0A0FFu), b = fill(0xB0B0B0FFu), cc = fill(0xC0C0C0FFu);
    f1.floating = f2.floating = f3.floating = DYN_UI_FLOAT_ROOT;
    f1.z = 1;
    f2.z = 1;
    f3.z = 0;
    dyn_ui_begin(ui, 50, 50);
    dyn_ui_open(ui, 1, &f1, &a);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 2, &f2, &b);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 3, &f3, &cc);
    dyn_ui_close(ui);
    dyn_ui_end(ui, NULL);
    c = dyn_ui_commands(ui, &n);
    CHECK(n == 3 && c[0].color == 0xC0C0C0FFu && c[1].color == 0xA0A0A0FFu && c[2].color == 0xB0B0B0FFu,
        "floats of equal z keep declaration order");
    {
        dyn_ui_layout row = lay(FIXED(100), FIT), col = lay(FIXED(100), FIT), wb = lay(FIT, FIT);
        row.dir = DYN_UI_ROW;
        row.clip_x = 1;
        col.dir = DYN_UI_COLUMN;
        col.clip_x = 1;
        dyn_ui_begin(ui, 300, 300);
        dyn_ui_open(ui, 10, &row, NULL);
        dyn_ui_open(ui, 11, &wb, NULL);
        dyn_ui_text(ui, 0, "aaaa bbbb cccc dddd", 19, font, 13, 0xFFFFFFFFu, 1, 0);
        dyn_ui_close(ui);
        dyn_ui_close(ui);
        dyn_ui_open(ui, 20, &col, NULL);
        dyn_ui_open(ui, 21, &wb, NULL);
        dyn_ui_text(ui, 0, "aaaa bbbb cccc dddd", 19, font, 13, 0xFFFFFFFFu, 1, 0);
        dyn_ui_close(ui);
        dyn_ui_close(ui);
        dyn_ui_end(ui, NULL);
        CHECK(box_is(ui, 11, 0, 0, 133, 13), "a horizontal scroller does not shrink its text along x");
        CHECK(box_is(ui, 21, 0, 13, 133, 13), "a column that clips x does not cap its child's width");
    }
    for (k = 0; k < 2; k++) {
        dyn_ui_begin(ui, 50, 50);
        dyn_ui_text(ui, 0, k ? "ac" : "ab", 2, font, 13, 0xFFFFFFFFu, 0, 0);
        dyn_ui_end(ui, &changed);
    }
    CHECK(changed == 1, "same width, same command, different glyph is a change");
    dyn_ui_free(ui);
    ui = fresh(&font);
    for (k = 0; k < 3; k++) {
        dyn_ui_layout pan = lay(FIXED(100), FIXED(50)), pop = lay(FIXED(100), FIXED(50));
        pan.dir = DYN_UI_COLUMN;
        pan.clip_y = 1;
        pop.floating = DYN_UI_FLOAT_ROOT;
        pop.interactive = 0;
        if (k == 2)
            wheel(ui, 10, 10, 0, -1);
        dyn_ui_begin(ui, 200, 200);
        dyn_ui_open(ui, 30, &pan, NULL);
        leaf(ui, 31, lay(FIXED(100), FIXED(200)));
        dyn_ui_close(ui);
        leaf(ui, 0, pop);
        dyn_ui_end(ui, NULL);
    }
    {
        dyn_ui_scroll_view v;
        dyn_ui_scroll_info(ui, 30, &v);
        CHECK(v.y == 0, "a wheel over a popup does not scroll the panel under it (%g)", v.y);
    }
    dyn_ui_free(ui);
}

static void test_blend_sweep(void)
{
    int font, a, row, bad;
    dyn_ui_t* ui = fresh(&font);
    static const uint32_t fg[4] = { 0xFF000000u, 0x00FF7F00u, 0x12345600u, 0xFEFDFC00u };
    static const uint32_t bg = 0x7F80C0FFu;
    uint8_t* got = malloc(256 * 4 * 4);
    frame want = frame_new(256, 4, bg);
    dyn_ui_layout l = lay(FIXED(1), FIXED(1));
    l.floating = DYN_UI_FLOAT_ROOT;
    l.interactive = 0;
    dyn_ui_begin(ui, 256, 4);
    for (row = 0; row < 4; row++)
        for (a = 0; a < 256; a++) {
            dyn_ui_style s = fill(fg[row] | (uint32_t)a);
            l.float_x = (float)a;
            l.float_y = (float)row;
            dyn_ui_open(ui, 0, &l, &s);
            dyn_ui_close(ui);
            put(&want, a, row, fg[row] | (uint32_t)a, 255);
        }
    dyn_ui_end(ui, NULL);
    dyn_ui_raster(ui, got, 256, 4, 256 * 4, bg);
    bad = compare(&want, got, "blend sweep");
    CHECK(bad == 0, "every alpha 0..255 over four colour pairs blends by (s*a + d*(255-a) + 127) / 255");
    free(want.px);
    free(want.band);
    free(got);
    dyn_ui_free(ui);
}

static void test_clipped_hits_and_fonts(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout outer = lay(FIXED(40), FIXED(10)), inner = lay(FIXED(30), FIXED(30));
    uint32_t b = 0;
    int k;
    uint8_t* cut;
    outer.clip_x = outer.clip_y = 1;
    inner.clip_x = inner.clip_y = 1;
    outer.interactive = inner.interactive = 0;
    pointer(ui, 5, 20, 0, 0);
    for (k = 0; k < 2; k++) {
        dyn_ui_begin(ui, 100, 100);
        dyn_ui_open(ui, 1, &outer, NULL);
        dyn_ui_open(ui, 2, &inner, NULL);
        leaf(ui, 3, lay(FIXED(30), FIXED(30)));
        dyn_ui_close(ui);
        dyn_ui_close(ui);
        b = dyn_ui_interact(ui, 3, 0);
        dyn_ui_end(ui, NULL);
    }
    CHECK(b == 0, "a child is not hot where an outer clip hides it, inside its own parent's clip: %x", b);
    cut = malloc(sizeof(proggy) - 4096);
    memcpy(cut, proggy, sizeof(proggy) - 4096);
    CHECK(dyn_ui_font_add(ui, cut, sizeof(proggy) - 4096, 1) < 0, "a font cut short, its directory intact, is refused");
    free(cut);
    dyn_ui_free(ui);
}

static void touch(dyn_ui_t* ui, float x, float y, uint32_t buttons, double time)
{
    dyn_ui_pointer p;
    memset(&p, 0, sizeof(p));
    p.x = x;
    p.y = y;
    p.buttons = buttons;
    p.time = time;
    p.kind = DYN_UI_POINTER_TOUCH;
    dyn_ui_input_pointer(ui, &p);
}

static void test_wrap_and_ellipsis(void)
{
    int font, k;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_layout row = lay(FIXED(100), FIT), eb = lay(FIXED(40), FIT);
    const dyn_ui_cmd* c;
    const dyn_ui_quad* q;
    size_t n, nq, i, quads = 0, first = 0;
    row.dir = DYN_UI_ROW;
    row.wrap = 1;
    row.gap = 4;
    row.interactive = 1;
    dyn_ui_begin(ui, 200, 200);
    dyn_ui_open(ui, 1, &row, NULL);
    for (k = 0; k < 5; k++)
        leaf(ui, (dyn_ui_id)(10 + k), lay(FIXED(30), FIXED(10)));
    dyn_ui_close(ui);
    dyn_ui_open(ui, 2, &eb, NULL);
    dyn_ui_text(ui, 0, "abcdefghij", 10, font, 13, 0xFFFFFFFFu, DYN_UI_TEXT_ELLIPSIS, DYN_UI_START);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 3, &row, NULL);
    leaf(ui, 20, lay(FIXED(48), FIXED(10)));
    leaf(ui, 21, lay(FIXED(50), FIXED(10)));
    dyn_ui_close(ui);
    CHECK(dyn_ui_end(ui, NULL) == 0, "end: %s", dyn_ui_error_text(ui));
    CHECK(box_is(ui, 21, 0, box(ui, 3).y + 14, 50, 10), "48 + 50 fits 100 px only without the gap, so the second box wraps: the gap counts in the break");
    CHECK(box_is(ui, 10, 0, 0, 30, 10) && box_is(ui, 11, 34, 0, 30, 10) && box_is(ui, 12, 68, 0, 30, 10), "three 30 px boxes fit a 100 px row with 4 px gaps (98)");
    CHECK(box_is(ui, 13, 0, 14, 30, 10) && box_is(ui, 14, 34, 14, 30, 10), "the fourth starts a second line one gap below the first");
    CHECK(box_is(ui, 1, 0, 0, 100, 24), "the wrapping row is as tall as its two lines and the gap");
    c = dyn_ui_commands(ui, &n);
    q = dyn_ui_quads(ui, &nq);
    for (i = 0; i < n; i++)
        if (c[i].type == DYN_UI_CMD_GLYPHS) {
            first = c[i].first;
            quads += c[i].count;
        }
    CHECK(quads == 5 && first + 5 <= nq, "70 px of text in 40 px keeps 2 glyphs (14 px) and appends \"...\" (21 px; ProggyClean has no U+2026): %zu quads", quads);
    if (quads == 5 && first + 5 <= nq)
        CHECK(q[first].u0 != q[first + 1].u0 && q[first + 1].u0 != q[first + 2].u0 && q[first + 2].u0 == q[first + 3].u0 && q[first + 3].u0 == q[first + 4].u0
                && q[first + 2].x0 >= 14 && q[first + 2].x0 < 21 && q[first + 4].x0 - q[first + 2].x0 == 14,
            "the quads are a, b and then three identical dots in the cells from 14 px, 7 px apart");
    dyn_ui_free(ui);
}

static void test_scale_resolution(void)
{
    int font;
    dyn_ui_t* ui = fresh(&font);
    const dyn_ui_theme* r;
    dyn_ui_set_scale(ui, 2);
    dyn_ui_begin(ui, 100, 100);
    dyn_ui_end(ui, NULL);
    r = dyn_ui_resolved(ui);
    CHECK(r->pad_x == 12 && r->pad_y == 8 && r->border_w == 2 && r->font_px == 26 && r->spacing == 8, "scale 2 doubles every metric: pad %g/%g border %g font %g", r->pad_x, r->pad_y,
        r->border_w, r->font_px);
    dyn_ui_set_scale(ui, 1.5f);
    dyn_ui_begin(ui, 100, 100);
    dyn_ui_end(ui, NULL);
    r = dyn_ui_resolved(ui);
    CHECK(r->pad_x == 9 && r->pad_y == 6 && r->border_w == 2 && r->font_px == 19.5f, "scale 1.5 rounds lengths to whole pixels (pad 9/6, border round(1.5) = 2) and keeps text exact (19.5)");
    dyn_ui_set_scale(ui, 100);
    CHECK(dyn_ui_scale(ui) == 1.5f, "a scale outside 0.25..16 is ignored");
    dyn_ui_free(ui);
}

static uint32_t touch_scene(dyn_ui_t* ui, uint32_t* row)
{
    dyn_ui_layout col = lay(FIXED(100), FIXED(60));
    uint32_t b;
    int k;
    col.dir = DYN_UI_COLUMN;
    col.clip_y = 1;
    dyn_ui_begin(ui, 200, 200);
    dyn_ui_open(ui, 1, &col, NULL);
    b = dyn_ui_interact(ui, 2, 1);
    leaf(ui, 2, lay(FIXED(100), FIXED(20)));
    for (k = 0; k < 9; k++)
        leaf(ui, (dyn_ui_id)(3 + k), lay(FIXED(100), FIXED(20)));
    dyn_ui_close(ui);
    if (row)
        *row = b;
    dyn_ui_end(ui, NULL);
    return b;
}

static void test_touch_gestures(void)
{
    int font, k, clicked = 0, ctx_seen = 0;
    double t = 1;
    dyn_ui_t* ui = fresh(&font);
    dyn_ui_scroll_view v;
    uint32_t b;
    dyn_ui_set_time(ui, t);
    touch_scene(ui, NULL);
    touch(ui, 50, 10, 1, t);
    b = touch_scene(ui, NULL);
    CHECK(b & DYN_UI_PRESSED, "a touch on the first row presses it");
    for (k = 1; k <= 6; k++) {
        t += 0.016;
        dyn_ui_set_time(ui, t);
        touch(ui, 50, 10 - 4.0f * (float)k, 1, t);
        b = touch_scene(ui, NULL);
    }
    dyn_ui_scroll_info(ui, 1, &v);
    CHECK(v.y == 24 && !(b & DYN_UI_ACTIVE), "a 24 px vertical drag past the 8 px slop scrolls the container 24 px and takes the press from the row (y %g, bits %x)", v.y, b);
    t += 0.016;
    dyn_ui_set_time(ui, t);
    touch(ui, 50, -18, 0, t);
    b = touch_scene(ui, NULL);
    CHECK(!(b & DYN_UI_CLICKED), "releasing a drag that scrolled is not a click (%x)", b);
    for (k = 0; k < 10; k++) {
        float before;
        dyn_ui_scroll_info(ui, 1, &v);
        before = v.y;
        t += 0.016;
        dyn_ui_set_time(ui, t);
        touch_scene(ui, NULL);
        dyn_ui_scroll_info(ui, 1, &v);
        if (k == 0)
            CHECK(v.y > before, "the release velocity keeps the content moving (fling: %g -> %g)", before, v.y);
    }
    CHECK(v.y <= 140 && v.y >= 24, "the fling stops inside the scroll range (%g)", v.y);
    for (k = 0; k < 120; k++) {
        t += 0.016;
        dyn_ui_set_time(ui, t);
        touch_scene(ui, NULL);
    }
    CHECK(dyn_ui_next_wake(ui) == (double)INFINITY, "a fling that has stopped asks for no more frames");
    dyn_ui_free(ui);
    ui = fresh(&font);
    t = 1;
    dyn_ui_set_time(ui, t);
    touch_scene(ui, NULL);
    touch(ui, 50, 10, 1, t);
    touch_scene(ui, NULL);
    CHECK(dyn_ui_next_wake(ui) > t && dyn_ui_next_wake(ui) <= t + 0.5 + 1e-9, "a held touch asks for a frame when the long press would fire (%g)", dyn_ui_next_wake(ui) - t);
    for (k = 0; k < 40; k++) {
        t += 0.016;
        dyn_ui_set_time(ui, t);
        b = touch_scene(ui, NULL);
        if (b & DYN_UI_CONTEXT)
            ctx_seen++;
    }
    CHECK(ctx_seen == 1, "holding still for 0.5 s raises CONTEXT exactly once (%d)", ctx_seen);
    touch(ui, 50, 10, 0, t);
    b = touch_scene(ui, NULL);
    clicked = (b & DYN_UI_CLICKED) != 0;
    CHECK(!clicked, "a long press does not also click");
    b = touch_scene(ui, NULL);
    CHECK(!(b & DYN_UI_HOT) && dyn_ui_hot(ui) == 0, "after the finger lifts nothing stays hot (touch has no hover)");
    touch(ui, 50, 10, 1, t + 1);
    touch(ui, 50, 10, 0, t + 1.05);
    b = touch_scene(ui, NULL);
    CHECK(b & DYN_UI_CLICKED, "a short tap clicks");
    dyn_ui_free(ui);
}

static void scroll_panel(dyn_ui_t* ui, int salt)
{
    dyn_ui_layout col = lay(FIXED(120), FIXED(80));
    char s[16];
    int k;
    col.dir = DYN_UI_COLUMN;
    col.clip_y = 1;
    dyn_ui_open(ui, 1, &col, NULL);
    for (k = 0; k < 20; k++) {
        int n = snprintf(s, sizeof(s), "line %d", k);
        dyn_ui_text(ui, 0, s, (size_t)n, 0, 13, 0xFFFFFFFFu, 0, DYN_UI_START);
    }
    dyn_ui_close(ui);
    leaf(ui, 50, lay(FIXED(40), FIXED(20)));
    if (salt) {
        dyn_ui_layout z = lay(FIXED(0), FIXED(0));
        z.interactive = 0;
        dyn_ui_open(ui, (dyn_ui_id)salt, &z, NULL);
        dyn_ui_close(ui);
    }
}

static void test_reuse_equals_recompute(void)
{
    int font, k, bad = 0;
    dyn_ui_t* a = fresh(&font);
    dyn_ui_t* b = fresh(&font);
    uint8_t* pa = malloc(160 * 140 * 4);
    uint8_t* pb = malloc(160 * 140 * 4);
    dyn_ui_stats_t sa, sb;
    for (k = 0; k < 60; k++) {
        dyn_ui_pointer p;
        memset(&p, 0, sizeof(p));
        p.x = k % 2 ? 20 : 21;
        p.y = 30;
        p.wheel_y = (k >= 10 && k < 18) ? -1.0f : (k >= 30 && k < 33) ? 1.0f
                                                                      : 0.0f;
        p.buttons = (k == 40 || k == 50) ? 1 : 0;
        if (k == 45) {
            float y = 7;
            dyn_ui_scroll(a, 1, NULL, &y);
            dyn_ui_scroll(b, 1, NULL, &y);
        }
        dyn_ui_input_pointer(a, &p);
        dyn_ui_input_pointer(b, &p);
        dyn_ui_begin(a, 160, 140);
        dyn_ui_begin(b, 160, 140);
        scroll_panel(a, 0);
        scroll_panel(b, 1000 + k);
        dyn_ui_end(a, NULL);
        dyn_ui_end(b, NULL);
        dyn_ui_raster(a, pa, 160, 140, 160 * 4, 0x000000FFu);
        dyn_ui_raster(b, pb, 160, 140, 160 * 4, 0x000000FFu);
        if (memcmp(pa, pb, 160 * 140 * 4))
            bad++;
    }
    dyn_ui_stats(a, &sa);
    dyn_ui_stats(b, &sb);
    CHECK(sa.reused > 10 && sb.reused == 0, "the plain context reuses frames (%llu) and its salted twin never does (%llu)", (unsigned long long)sa.reused,
        (unsigned long long)sb.reused);
    CHECK(bad == 0, "every reused frame rasterises identically to a recomputed one, through wheel, hover, press and dyn_ui_scroll (%d differ)", bad);
    free(pa);
    free(pb);
    dyn_ui_free(a);
    dyn_ui_free(b);
}

static void test_tessellation_and_insets(void)
{
    int font, k, ok_idx = 1, ok_fin = 1;
    dyn_ui_t* ui = fresh(&font);
    const dyn_ui_gvertex* v;
    const uint32_t* idx;
    const dyn_ui_batch* bt;
    size_t nv, ni, nb, i, covered = 0;
    dyn_ui_layout f = lay(FIXED(30), FIXED(30)), r = lay(FIXED(40), FIXED(20));
    dyn_ui_style s = fill(0xFF0000FFu), rs = fill(0x00FF00FFu);
    rs.radius = 6;
    rs.border = 0x0000FFFFu;
    rs.border_w = 2;
    f.floating = DYN_UI_FLOAT_SAFE;
    f.float_x = 190;
    f.float_y = 190;
    dyn_ui_set_insets(ui, 5, 6, 7, 8);
    dyn_ui_begin(ui, 200, 200);
    dyn_ui_open(ui, 1, &f, &s);
    dyn_ui_close(ui);
    f.floating = DYN_UI_FLOAT_ROOT;
    f.float_x = 0;
    f.float_y = 0;
    f.w = FIXED(200);
    dyn_ui_open(ui, 3, &f, NULL);
    dyn_ui_close(ui);
    dyn_ui_open(ui, 2, &r, &rs);
    dyn_ui_text(ui, 0, "Hi", 2, font, 13, 0xFFFFFFFFu, 0, DYN_UI_START);
    dyn_ui_close(ui);
    dyn_ui_end(ui, NULL);
    CHECK(box_is(ui, 1, 163, 162, 30, 30), "a SAFE float is kept inside the safe area: 200 - 7 - 30, 200 - 8 - 30");
    CHECK(box_is(ui, 3, 0, 0, 200, 30), "a ROOT float stays where it was put, over the insets (edge-to-edge chrome)");
    CHECK(dyn_ui_tessellate(ui, &v, &nv, &idx, &ni, &bt, &nb) == 0 && nb >= 1, "tessellates");
    for (i = 0; i < ni; i++)
        if (idx[i] >= nv)
            ok_idx = 0;
    for (i = 0; i < nv; i++)
        if (!isfinite(v[i].x) || !isfinite(v[i].y) || !isfinite(v[i].u) || !isfinite(v[i].v))
            ok_fin = 0;
    for (k = 0; k < (int)nb; k++) {
        CHECK(bt[k].first == covered, "batches are contiguous (%u vs %zu)", bt[k].first, covered);
        covered += bt[k].count;
    }
    CHECK(ok_idx && ok_fin && covered == ni && ni % 3 == 0, "every index is in range, every vertex finite, the batches cover all %zu indices in triangles", ni);
    CHECK(bt[0].texture == DYN_UI_TEX_ATLAS, "solid shapes and text share the atlas batch");
    CHECK(dyn_ui_atlas(ui).pixels[0] == 255, "atlas texel (0,0) is the white texel solid geometry samples");
    dyn_ui_free(ui);
}

static void test_atlas_rebuild(void)
{
    int font, k, full = 0;
    dyn_ui_t* ui = fresh(&font);
    char s[96];
    dyn_ui_layout col = lay(FIT, FIT);
    dyn_ui_atlas_view a;
    for (k = 0; k < 95; k++)
        s[k] = (char)(32 + k);
    col.dir = DYN_UI_COLUMN;
    for (k = 0; k < 40 && !full; k++) {
        dyn_ui_begin(ui, 4000, 4000);
        dyn_ui_open(ui, 1, &col, NULL);
        dyn_ui_text(ui, 0, s, 95, font, 200.0f + 7.0f * (float)k, 0xFFFFFFFFu, 0, DYN_UI_START);
        dyn_ui_close(ui);
        full = dyn_ui_end(ui, NULL) == DYN_UI_ERR_LIMIT;
    }
    CHECK(full, "glyphs at 200 px and up fill the 1024 x 8192 atlas within %d sizes", k);
    dyn_ui_begin(ui, 100, 100);
    dyn_ui_end(ui, NULL);
    a = dyn_ui_atlas(ui);
    CHECK(a.height == 128 && a.pixels[0] == 255, "the begin() after a full atlas rebuilds it at 128 rows and keeps the white texel (%d, %u)", a.height, a.pixels[0]);
    dyn_ui_free(ui);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_grow_distribution();
    test_align_percent_max();
    test_text_and_wrap();
    test_scroll();
    test_nested_scroll();
    test_floating();
    test_interaction();
    test_command_order();
    test_errors();
    test_state();
    test_raster_whole_frame();
    test_rounded_and_clip_nesting();
    test_random_trees();
    test_atlas_full_recovers();
    test_gaps();
    test_blend_sweep();
    test_clipped_hits_and_fonts();
    test_wrap_and_ellipsis();
    test_scale_resolution();
    test_touch_gestures();
    test_reuse_equals_recompute();
    test_tessellation_and_insets();
    test_atlas_rebuild();
    printf("test_ui_core: %d pass, %d fail\n", passes, fails);
    return fails ? 1 : 0;
}
