#include "cutils.h"
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI) && defined(CONFIG_RAYLIB)

#include <math.h>
#include <raylib.h>
#include <rlgl.h>
#include <string.h>

#include "dyna-ui-backend.h"

#define RLB_MAX_IMAGES 4096
#define RLB_CHUNK 3072

typedef struct {
    Texture2D atlas;
    int atlas_h;
    uint32_t atlas_ver;
    Texture2D img[RLB_MAX_IMAGES];
    uint32_t img_ver[RLB_MAX_IMAGES];
    uint8_t* conv;
    size_t conv_cap;
    int touch_down;
    float last_x, last_y;
    int polled;
    int dirty;
    float density;
    int fw, fh;
    dyn_ui_t* ui;
} rlb;

static const struct {
    int rl;
    int ui;
} rlb_keys[] = {
    { KEY_LEFT, DYN_UI_KEY_LEFT },
    { KEY_RIGHT, DYN_UI_KEY_RIGHT },
    { KEY_UP, DYN_UI_KEY_UP },
    { KEY_DOWN, DYN_UI_KEY_DOWN },
    { KEY_HOME, DYN_UI_KEY_HOME },
    { KEY_END, DYN_UI_KEY_END },
    { KEY_BACKSPACE, DYN_UI_KEY_BACKSPACE },
    { KEY_DELETE, DYN_UI_KEY_DELETE },
    { KEY_ENTER, DYN_UI_KEY_RETURN },
    { KEY_KP_ENTER, DYN_UI_KEY_RETURN },
    { KEY_TAB, DYN_UI_KEY_TAB },
    { KEY_ESCAPE, DYN_UI_KEY_ESCAPE },
    { KEY_PAGE_UP, DYN_UI_KEY_PAGEUP },
    { KEY_PAGE_DOWN, DYN_UI_KEY_PAGEDOWN },
    { KEY_A, DYN_UI_KEY_A },
    { KEY_C, DYN_UI_KEY_C },
    { KEY_V, DYN_UI_KEY_V },
    { KEY_X, DYN_UI_KEY_X },
    { KEY_Y, DYN_UI_KEY_Y },
    { KEY_Z, DYN_UI_KEY_Z },
    { KEY_SPACE, DYN_UI_KEY_SPACE },
};

static int rlb_mods(void)
{
    return ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? DYN_UI_MOD_SHIFT : 0)
        | ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) ? DYN_UI_MOD_CTRL : 0)
        | ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) ? DYN_UI_MOD_ALT : 0)
        | ((IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) ? DYN_UI_MOD_SUPER : 0);
}

static void rlb_input(rlb* b)
{
    dyn_ui_pointer p;
    size_t k;
    int mods = rlb_mods(), cp;
    char u8[8];
    memset(&p, 0, sizeof(p));
    p.time = GetTime();
#ifdef __ANDROID__
    if (GetTouchPointCount() > 0) {
        Vector2 t = GetTouchPosition(0);
        p.x = t.x * b->density;
        p.y = t.y * b->density;
        p.buttons = 1;
        b->touch_down = 1;
    } else {
        p.x = b->last_x;
        p.y = b->last_y;
        b->touch_down = 0;
    }
    p.kind = DYN_UI_POINTER_TOUCH;
#else
    {
        Vector2 m = GetMousePosition(), w = GetMouseWheelMoveV();
        p.x = m.x * b->density;
        p.y = m.y * b->density;
        p.buttons = (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 1u : 0u) | (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ? 2u : 0u)
            | (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ? 4u : 0u);
        p.wheel_x = w.x;
        p.wheel_y = w.y;
        p.kind = DYN_UI_POINTER_MOUSE;
    }
#endif
    b->last_x = p.x;
    b->last_y = p.y;
    dyn_ui_input_pointer(b->ui, &p);
    for (k = 0; k < countof(rlb_keys); k++) {
        if (IsKeyPressed(rlb_keys[k].rl) || IsKeyPressedRepeat(rlb_keys[k].rl))
            dyn_ui_input_key(b->ui, rlb_keys[k].ui, 1, mods);
        if (IsKeyReleased(rlb_keys[k].rl))
            dyn_ui_input_key(b->ui, rlb_keys[k].ui, 0, mods);
    }
    while ((cp = GetCharPressed()) > 0) {
        int n = 0;
        const char* s = CodepointToUTF8(cp, &n);
        if (n > 0 && n <= 4) {
            memcpy(u8, s, (size_t)n);
            dyn_ui_input_text(b->ui, u8, (size_t)n);
        }
    }
}

static const char* rlb_clip_get(void* ud, size_t* n)
{
    const char* s = GetClipboardText();
    (void)ud;
    *n = s ? strlen(s) : 0;
    return s;
}

static void rlb_clip_set(void* ud, const char* s, size_t n)
{
    char* c = dyn_nat_malloc(n + 1);
    (void)ud;
    if (!c)
        return;
    memcpy(c, s, n);
    c[n] = 0;
    SetClipboardText(c);
    dyn_nat_free(c);
}

static int rlb_check(JSContext* ctx, JSValueConst target, int* in_frame)
{
    int rc = dyn_raylib_target(ctx, target, in_frame);
    if (rc == 0)
        JS_ThrowTypeError(ctx, "dyna:ui: the Context target is no longer a dyna:raylib Window");
    return rc == 1 ? 0 : -1;
}

static int rlb_begin(void* be, JSContext* ctx, JSValueConst target, dyn_ui_t* ui, dyn_uib_view* view)
{
    rlb* b = be;
    int in_frame = 0, sw, rw;
    Vector2 dpi;
    (void)ui;
    if (rlb_check(ctx, target, &in_frame))
        return -1;
    if (!b->polled && !in_frame)
        PollInputEvents();
    b->polled = 0;
    sw = GetScreenWidth();
    rw = GetRenderWidth();
    b->density = sw > 0 && rw > 0 ? (float)rw / (float)sw : 1;
    rlb_input(b);
    b->fw = rw;
    b->fh = GetRenderHeight();
    if (b->fw < 1 || b->fh < 1) {
        JS_ThrowTypeError(ctx, "dyna:ui: the raylib window has no drawable area");
        return -1;
    }
    view->w = (float)b->fw;
    view->h = (float)b->fh;
    view->density = b->density;
    dpi = GetWindowScaleDPI();
    view->scale = dpi.x > 0 ? dpi.x : b->density;
    memset(view->insets, 0, sizeof(view->insets));
#ifdef __ANDROID__
    view->touch = 1;
#else
    view->touch = 0;
#endif
    return 0;
}

static Texture2D rlb_texture(const uint8_t* rgba, int w, int h)
{
    Image im;
    im.data = (void*)(uintptr_t)rgba;
    im.width = w;
    im.height = h;
    im.mipmaps = 1;
    im.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return LoadTextureFromImage(im);
}

static int rlb_sync_atlas(rlb* b, dyn_ui_t* ui)
{
    dyn_ui_atlas_view a = dyn_ui_atlas(ui);
    size_t px = (size_t)a.width * (size_t)a.height, i;
    if (b->atlas.id && b->atlas_ver == a.version && b->atlas_h == a.height)
        return 0;
    if (px * 4 > b->conv_cap) {
        uint8_t* c = dyn_nat_realloc(b->conv, px * 4);
        if (!c)
            return -1;
        b->conv = c;
        b->conv_cap = px * 4;
    }
    for (i = 0; i < px; i++) {
        b->conv[i * 4] = b->conv[i * 4 + 1] = b->conv[i * 4 + 2] = 255;
        b->conv[i * 4 + 3] = a.pixels[i];
    }
    if (b->atlas.id && b->atlas_h == a.height)
        UpdateTexture(b->atlas, b->conv);
    else {
        if (b->atlas.id)
            UnloadTexture(b->atlas);
        b->atlas = rlb_texture(b->conv, a.width, a.height);
        b->atlas_h = a.height;
        if (!b->atlas.id)
            return -1;
        SetTextureFilter(b->atlas, TEXTURE_FILTER_POINT);
    }
    b->atlas_ver = a.version;
    return 0;
}

static unsigned int rlb_image(rlb* b, dyn_ui_t* ui, uint32_t id)
{
    int w, h;
    const uint8_t* px;
    uint32_t ver = dyn_ui_image_version(ui, id);
    if (id == 0 || id >= RLB_MAX_IMAGES || !ver)
        return 0;
    if (b->img[id].id && b->img_ver[id] == ver)
        return b->img[id].id;
    px = dyn_ui_image_pixels(ui, id, &w, &h);
    if (!px)
        return 0;
    if (b->img[id].id && b->img[id].width == w && b->img[id].height == h)
        UpdateTexture(b->img[id], px);
    else {
        if (b->img[id].id)
            UnloadTexture(b->img[id]);
        b->img[id] = rlb_texture(px, w, h);
        if (!b->img[id].id)
            return 0;
        SetTextureFilter(b->img[id], TEXTURE_FILTER_POINT);
    }
    b->img_ver[id] = ver;
    return b->img[id].id;
}

static int rlb_draw(void* be, JSContext* ctx, JSValueConst target, dyn_ui_t* ui, int changed, int present, uint32_t clear)
{
    rlb* b = be;
    const dyn_ui_gvertex* v;
    const uint32_t* idx;
    const dyn_ui_batch* bt;
    size_t nv, ni, nb, i;
    int in_frame = 0, blend;
    float d = b->density > 0 ? b->density : 1;
    if (rlb_check(ctx, target, &in_frame))
        return -1;
    if (dyn_raylib_texture_height() > 0)
        d = 1;
    if (present && in_frame) {
        JS_ThrowTypeError(ctx, "dyna:ui: frame() on a presenting raylib Context runs outside rl.frame(); use { present: false } to draw inside it");
        return -1;
    }
    if (!present && !in_frame) {
        JS_ThrowTypeError(ctx, "dyna:ui: a { present: false } raylib Context draws inside rl.frame() or RenderTexture#draw()");
        return -1;
    }
    if (present && !changed && !b->dirty && !IsWindowResized())
        return 0;
    b->dirty = 0;
    if (dyn_ui_tessellate(ui, &v, &nv, &idx, &ni, &bt, &nb)) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (rlb_sync_atlas(b, ui)) {
        JS_ThrowInternalError(ctx, "dyna:ui: the glyph atlas texture could not be created");
        return -1;
    }
    if (present) {
        BeginDrawing();
        ClearBackground((Color) { (unsigned char)(clear >> 24), (unsigned char)(clear >> 16), (unsigned char)(clear >> 8), (unsigned char)clear });
    }
    rlDrawRenderBatchActive();
    blend = dyn_raylib_blend_mode();
    if (blend == BLEND_ALPHA) {
        rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD, RL_FUNC_ADD);
        rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
    }
    rlDisableBackfaceCulling();
    for (i = 0; i < nb; i++) {
        unsigned int tex = bt[i].texture == DYN_UI_TEX_ATLAS ? b->atlas.id : rlb_image(b, ui, bt[i].texture);
        uint32_t k;
        if (!tex || bt[i].count == 0)
            continue;
        rlDrawRenderBatchActive();
        rlEnableScissorTest();
        rlScissor((int)bt[i].clip.x, b->fh - (int)(bt[i].clip.y + bt[i].clip.h), (int)bt[i].clip.w, (int)bt[i].clip.h);
        rlSetTexture(tex);
        for (k = 0; k < bt[i].count; k += RLB_CHUNK) {
            uint32_t e = k + RLB_CHUNK < bt[i].count ? k + RLB_CHUNK : bt[i].count, j;
            rlCheckRenderBatchLimit((int)(e - k));
            rlBegin(RL_TRIANGLES);
            for (j = k; j < e; j++) {
                const dyn_ui_gvertex* q = &v[idx[bt[i].first + j]];
                rlColor4ub((unsigned char)(q->color >> 24), (unsigned char)(q->color >> 16), (unsigned char)(q->color >> 8), (unsigned char)q->color);
                rlTexCoord2f(q->u, q->v);
                rlVertex2f(q->x / d, q->y / d);
            }
            rlEnd();
        }
        rlDrawRenderBatchActive();
        rlSetTexture(0);
    }
    rlDisableScissorTest();
    rlEnableBackfaceCulling();
    if (blend == BLEND_ALPHA)
        rlSetBlendMode(RL_BLEND_ALPHA);
    if (present) {
        EndDrawing();
        b->polled = 1;
    }
    (void)ni;
    return 1;
}

static void rlb_idle(void* be, double seconds)
{
    (void)be;
    if (seconds > 0)
        WaitTime(seconds < 1.0 / 60 ? seconds : 1.0 / 60);
}

static void rlb_release(void* be, JSValueConst target)
{
    rlb* b = be;
    size_t i;
    if (dyn_raylib_window_alive(target)) {
        if (b->atlas.id)
            UnloadTexture(b->atlas);
        for (i = 0; i < RLB_MAX_IMAGES; i++)
            if (b->img[i].id)
                UnloadTexture(b->img[i]);
    }
    dyn_nat_free(b->conv);
    dyn_nat_free(b);
}

static const dyn_uib_ops rlb_ops = { rlb_begin, rlb_draw, rlb_idle, rlb_release, "raylib" };

void* dyn_uib_raylib_attach(JSContext* ctx, JSValueConst target, dyn_ui_t* ui, const dyn_uib_ops** ops)
{
    int in_frame = 0, rc = dyn_raylib_target(ctx, target, &in_frame);
    rlb* b;
    if (rc <= 0)
        return NULL;
    b = dyn_nat_calloc(1, sizeof(*b));
    if (!b) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    b->ui = ui;
    b->density = 1;
    b->dirty = 1;
    dyn_ui_set_clipboard(ui, rlb_clip_get, rlb_clip_set, b);
    *ops = &rlb_ops;
    return b;
}

#endif
