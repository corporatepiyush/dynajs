#include "cutils.h"
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI) && defined(CONFIG_SDL3)

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "dyna-ui-backend.h"

#define SDLB_MAX_QUEUE 8192
#define SDLB_REPAINT_NS 500000000u
#define SDLB_MAX_IMAGES 4096

typedef struct {
    SDL_Event e;
    char* text;
} sdlb_ev;

typedef struct {
    SDL_Mutex* mu;
    sdlb_ev* q;
    size_t n, cap;
    SDL_WindowID wid;
    SDL_Renderer* r;
    SDL_Window* w;
    SDL_Texture* atlas;
    int atlas_h;
    uint32_t atlas_ver;
    SDL_Texture* img[SDLB_MAX_IMAGES];
    uint32_t img_ver[SDLB_MAX_IMAGES];
    uint8_t* conv;
    size_t conv_cap;
    SDL_FColor* vc;
    size_t vcap;
    uint32_t buttons;
    int touch_down;
    SDL_FingerID finger;
    float last_x, last_y;
    int text_active;
    int dirty;
    Uint64 repaint_until;
    float density;
    char* clip;
    dyn_ui_t* ui;
} sdlb;

static bool SDLCALL sdlb_watch(void* ud, SDL_Event* e)
{
    sdlb* b = ud;
    SDL_WindowID wid = 0;
    char* text = NULL;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        wid = e->motion.windowID;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        wid = e->button.windowID;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        wid = e->wheel.windowID;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        wid = e->tfinger.windowID;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        wid = e->key.windowID;
        break;
    case SDL_EVENT_TEXT_INPUT:
        wid = e->text.windowID;
        text = e->text.text ? SDL_strdup(e->text.text) : NULL;
        break;
    case SDL_EVENT_TEXT_EDITING:
        wid = e->edit.windowID;
        text = SDL_strdup(e->edit.text ? e->edit.text : "");
        break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
        wid = e->render.windowID;
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
    case SDL_EVENT_SCREEN_KEYBOARD_SHOWN:
    case SDL_EVENT_SCREEN_KEYBOARD_HIDDEN:
        break;
    default:
        if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) {
            wid = e->window.windowID;
            break;
        }
        return true;
    }
    if (wid != b->wid && wid != 0) {
        SDL_free(text);
        return true;
    }
    SDL_LockMutex(b->mu);
    if (b->n == b->cap && b->cap < SDLB_MAX_QUEUE) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        sdlb_ev* q = SDL_realloc(b->q, nc * sizeof(*q));
        if (q) {
            b->q = q;
            b->cap = nc;
        }
    }
    if (b->n < b->cap) {
        b->q[b->n].e = *e;
        b->q[b->n].text = text;
        b->n++;
        text = NULL;
    }
    SDL_UnlockMutex(b->mu);
    SDL_free(text);
    return true;
}

static int sdlb_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_LEFT:
        return DYN_UI_KEY_LEFT;
    case SDLK_RIGHT:
        return DYN_UI_KEY_RIGHT;
    case SDLK_UP:
        return DYN_UI_KEY_UP;
    case SDLK_DOWN:
        return DYN_UI_KEY_DOWN;
    case SDLK_HOME:
        return DYN_UI_KEY_HOME;
    case SDLK_END:
        return DYN_UI_KEY_END;
    case SDLK_BACKSPACE:
        return DYN_UI_KEY_BACKSPACE;
    case SDLK_DELETE:
        return DYN_UI_KEY_DELETE;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return DYN_UI_KEY_RETURN;
    case SDLK_TAB:
        return DYN_UI_KEY_TAB;
    case SDLK_ESCAPE:
    case SDLK_AC_BACK:
        return DYN_UI_KEY_ESCAPE;
    case SDLK_PAGEUP:
        return DYN_UI_KEY_PAGEUP;
    case SDLK_PAGEDOWN:
        return DYN_UI_KEY_PAGEDOWN;
    case SDLK_A:
        return DYN_UI_KEY_A;
    case SDLK_C:
        return DYN_UI_KEY_C;
    case SDLK_V:
        return DYN_UI_KEY_V;
    case SDLK_X:
        return DYN_UI_KEY_X;
    case SDLK_Y:
        return DYN_UI_KEY_Y;
    case SDLK_Z:
        return DYN_UI_KEY_Z;
    case SDLK_SPACE:
        return DYN_UI_KEY_SPACE;
    default:
        return 0;
    }
}

static int sdlb_mods(SDL_Keymod m)
{
    return ((m & SDL_KMOD_SHIFT) ? DYN_UI_MOD_SHIFT : 0) | ((m & SDL_KMOD_CTRL) ? DYN_UI_MOD_CTRL : 0) | ((m & SDL_KMOD_ALT) ? DYN_UI_MOD_ALT : 0)
        | ((m & SDL_KMOD_GUI) ? DYN_UI_MOD_SUPER : 0);
}

static uint32_t sdlb_button_bit(Uint8 button)
{
    return button == SDL_BUTTON_LEFT ? 1u : button == SDL_BUTTON_RIGHT ? 2u
        : button == SDL_BUTTON_MIDDLE                                  ? 4u
                                                                       : 0u;
}

static void sdlb_pointer(sdlb* b, float x, float y, uint32_t buttons, uint8_t kind, Uint64 ts, float wx, float wy)
{
    dyn_ui_pointer p;
    memset(&p, 0, sizeof(p));
    p.x = x;
    p.y = y;
    p.buttons = buttons;
    p.kind = kind;
    p.wheel_x = wx;
    p.wheel_y = wy;
    p.time = (double)ts / 1e9;
    b->last_x = x;
    b->last_y = y;
    dyn_ui_input_pointer(b->ui, &p);
}

static void sdlb_drop_textures(sdlb* b)
{
    size_t i;
    if (b->atlas)
        SDL_DestroyTexture(b->atlas);
    b->atlas = NULL;
    b->atlas_h = 0;
    for (i = 0; i < SDLB_MAX_IMAGES; i++) {
        if (b->img[i])
            SDL_DestroyTexture(b->img[i]);
        b->img[i] = NULL;
        b->img_ver[i] = 0;
    }
}

static void sdlb_apply(sdlb* b, const sdlb_ev* ev, int ow, int oh)
{
    const SDL_Event* e = &ev->e;
    float d = b->density;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID)
            break;
        sdlb_pointer(b, e->motion.x * d, e->motion.y * d, b->buttons, e->motion.which == SDL_PEN_MOUSEID ? DYN_UI_POINTER_PEN : DYN_UI_POINTER_MOUSE,
            e->motion.timestamp, 0, 0);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.which == SDL_TOUCH_MOUSEID)
            break;
        if (e->button.down)
            b->buttons |= sdlb_button_bit(e->button.button);
        else
            b->buttons &= ~sdlb_button_bit(e->button.button);
        sdlb_pointer(b, e->button.x * d, e->button.y * d, b->buttons, e->button.which == SDL_PEN_MOUSEID ? DYN_UI_POINTER_PEN : DYN_UI_POINTER_MOUSE,
            e->button.timestamp, 0, 0);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float s = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        if (e->wheel.which == SDL_TOUCH_MOUSEID)
            break;
        sdlb_pointer(b, e->wheel.mouse_x * d, e->wheel.mouse_y * d, b->buttons, DYN_UI_POINTER_MOUSE, e->wheel.timestamp, e->wheel.x * s, e->wheel.y * s);
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
        if (b->touch_down)
            break;
        b->touch_down = 1;
        b->finger = e->tfinger.fingerID;
        sdlb_pointer(b, e->tfinger.x * (float)ow, e->tfinger.y * (float)oh, 1, DYN_UI_POINTER_TOUCH, e->tfinger.timestamp, 0, 0);
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (b->touch_down && e->tfinger.fingerID == b->finger)
            sdlb_pointer(b, e->tfinger.x * (float)ow, e->tfinger.y * (float)oh, 1, DYN_UI_POINTER_TOUCH, e->tfinger.timestamp, 0, 0);
        break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (b->touch_down && e->tfinger.fingerID == b->finger) {
            b->touch_down = 0;
            sdlb_pointer(b, e->tfinger.x * (float)ow, e->tfinger.y * (float)oh, 0, DYN_UI_POINTER_TOUCH, e->tfinger.timestamp, 0, 0);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int k = sdlb_key(e->key.key);
        if (k)
            dyn_ui_input_key(b->ui, k, e->key.down, sdlb_mods(e->key.mod));
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (ev->text)
            dyn_ui_input_text(b->ui, ev->text, strlen(ev->text));
        dyn_ui_input_composition(b->ui, "", 0, 0);
        break;
    case SDL_EVENT_TEXT_EDITING:
        if (ev->text)
            dyn_ui_input_composition(b->ui, ev->text, strlen(ev->text), e->edit.start);
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
        sdlb_drop_textures(b);
        b->dirty = 1;
        b->repaint_until = SDL_GetTicksNS() + SDLB_REPAINT_NS;
        break;
    case SDL_EVENT_SCREEN_KEYBOARD_SHOWN:
    case SDL_EVENT_SCREEN_KEYBOARD_HIDDEN:
        b->dirty = 1;
        break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_EXPOSED:
        b->dirty = 1;
        b->repaint_until = SDL_GetTicksNS() + SDLB_REPAINT_NS;
        break;
    default:
        if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST)
            b->dirty = 1;
        break;
    }
}

static const char* sdlb_clip_get(void* ud, size_t* n)
{
    sdlb* b = ud;
    SDL_free(b->clip);
    b->clip = SDL_GetClipboardText();
    *n = b->clip ? strlen(b->clip) : 0;
    return b->clip;
}

static void sdlb_clip_set(void* ud, const char* s, size_t n)
{
    char* c = SDL_malloc(n + 1);
    (void)ud;
    if (!c)
        return;
    memcpy(c, s, n);
    c[n] = 0;
    SDL_SetClipboardText(c);
    SDL_free(c);
}

static int sdlb_resolve(sdlb* b, JSContext* ctx, JSValueConst target)
{
    void *r = NULL, *w = NULL;
    int rc = dyn_sdl_renderer_parts(ctx, target, &r, &w);
    if (rc <= 0) {
        if (rc == 0)
            JS_ThrowTypeError(ctx, "dyna:ui: the Context target is no longer a dyna:sdl Renderer");
        return -1;
    }
    b->r = r;
    b->w = w;
    return 0;
}

static int sdlb_begin(void* be, JSContext* ctx, JSValueConst target, dyn_ui_t* ui, dyn_uib_view* view)
{
    sdlb* b = be;
    sdlb_ev* q;
    size_t n, i;
    int ow = 0, oh = 0, ww = 0, wh = 0;
    SDL_Rect safe;
    if (sdlb_resolve(b, ctx, target))
        return -1;
    if (!SDL_GetCurrentRenderOutputSize(b->r, &ow, &oh) || ow < 1 || oh < 1) {
        JS_ThrowInternalError(ctx, "dyna:ui: SDL_GetCurrentRenderOutputSize: %s", SDL_GetError());
        return -1;
    }
    b->density = SDL_GetWindowPixelDensity(b->w);
    if (!(b->density > 0))
        b->density = 1;
    SDL_PumpEvents();
    SDL_LockMutex(b->mu);
    q = b->q;
    n = b->n;
    b->q = NULL;
    b->n = b->cap = 0;
    SDL_UnlockMutex(b->mu);
    for (i = 0; i < n; i++) {
        sdlb_apply(b, &q[i], ow, oh);
        SDL_free(q[i].text);
    }
    SDL_free(q);
    view->w = (float)ow;
    view->h = (float)oh;
    view->density = b->density;
    view->scale = SDL_GetWindowDisplayScale(b->w);
    if (!(view->scale > 0))
        view->scale = b->density;
    memset(view->insets, 0, sizeof(view->insets));
    view->touch = !strcmp(SDL_GetPlatform(), "Android") || !strcmp(SDL_GetPlatform(), "iOS");
    if (SDL_GetWindowSize(b->w, &ww, &wh) && SDL_GetWindowSafeArea(b->w, &safe)) {
        view->insets[0] = (float)safe.x * b->density;
        view->insets[1] = (float)safe.y * b->density;
        view->insets[2] = (float)(ww - safe.x - safe.w) * b->density;
        view->insets[3] = (float)(wh - safe.y - safe.h) * b->density;
    }
    {
        float kb = SDL_GetFloatProperty(SDL_GetGlobalProperties(), DYN_UIB_KEYBOARD_PROP, 0);
        if (kb > view->insets[3])
            view->insets[3] = kb;
    }
    return 0;
}

static SDL_Texture* sdlb_texture(sdlb* b, int w, int h)
{
    SDL_Texture* t = SDL_CreateTexture(b->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    return t;
}

static int sdlb_sync_atlas(sdlb* b, dyn_ui_t* ui)
{
    dyn_ui_atlas_view a = dyn_ui_atlas(ui);
    size_t px = (size_t)a.width * (size_t)a.height, i;
    if (b->atlas && b->atlas_ver == a.version && b->atlas_h == a.height)
        return 0;
    if (!b->atlas || b->atlas_h != a.height) {
        if (b->atlas)
            SDL_DestroyTexture(b->atlas);
        b->atlas = sdlb_texture(b, a.width, a.height);
        b->atlas_h = a.height;
        if (!b->atlas)
            return -1;
    }
    if (px * 4 > b->conv_cap) {
        uint8_t* c = SDL_realloc(b->conv, px * 4);
        if (!c)
            return -1;
        b->conv = c;
        b->conv_cap = px * 4;
    }
    for (i = 0; i < px; i++) {
        b->conv[i * 4] = b->conv[i * 4 + 1] = b->conv[i * 4 + 2] = 255;
        b->conv[i * 4 + 3] = a.pixels[i];
    }
    if (!SDL_UpdateTexture(b->atlas, NULL, b->conv, a.width * 4))
        return -1;
    b->atlas_ver = a.version;
    return 0;
}

static SDL_Texture* sdlb_image(sdlb* b, dyn_ui_t* ui, uint32_t id)
{
    int w, h;
    const uint8_t* px;
    uint32_t ver = dyn_ui_image_version(ui, id);
    if (id == 0 || id >= SDLB_MAX_IMAGES || !ver)
        return NULL;
    if (b->img[id] && b->img_ver[id] == ver)
        return b->img[id];
    px = dyn_ui_image_pixels(ui, id, &w, &h);
    if (!px)
        return NULL;
    if (b->img[id]) {
        float tw = 0, th = 0;
        SDL_GetTextureSize(b->img[id], &tw, &th);
        if ((int)tw != w || (int)th != h) {
            SDL_DestroyTexture(b->img[id]);
            b->img[id] = NULL;
        }
    }
    if (!b->img[id])
        b->img[id] = sdlb_texture(b, w, h);
    if (!b->img[id] || !SDL_UpdateTexture(b->img[id], NULL, px, w * 4))
        return NULL;
    b->img_ver[id] = ver;
    return b->img[id];
}

static int sdlb_draw(void* be, JSContext* ctx, JSValueConst target, dyn_ui_t* ui, int changed, int present, uint32_t clear)
{
    sdlb* b = be;
    const dyn_ui_gvertex* v;
    const uint32_t* idx;
    const dyn_ui_batch* bt;
    size_t nv, ni, nb, i;
    dyn_ui_rect caret;
    float sx = 1, sy = 1;
    SDL_Rect vp, cl;
    bool had_clip;
    int ow = 0, oh = 0;
    if (sdlb_resolve(b, ctx, target))
        return -1;
    if (dyn_ui_text_input(ui, &caret)) {
        SDL_Rect r;
        r.x = (int)(caret.x / b->density);
        r.y = (int)(caret.y / b->density);
        r.w = (int)ceilf(caret.w / b->density) + 1;
        r.h = (int)ceilf(caret.h / b->density);
        if (!b->text_active) {
            SDL_StartTextInput(b->w);
            b->text_active = 1;
        }
        SDL_SetTextInputArea(b->w, &r, 0);
    } else if (b->text_active) {
        SDL_StopTextInput(b->w);
        b->text_active = 0;
    }
    if (present && !changed && !b->dirty && SDL_GetTicksNS() >= b->repaint_until)
        return 0;
    b->dirty = 0;
    if (dyn_ui_tessellate(ui, &v, &nv, &idx, &ni, &bt, &nb)) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (sdlb_sync_atlas(b, ui)) {
        JS_ThrowInternalError(ctx, "dyna:ui: the glyph atlas texture: %s", SDL_GetError());
        return -1;
    }
    if (nv > b->vcap) {
        SDL_FColor* nvc = SDL_realloc(b->vc, nv * sizeof(SDL_FColor));
        if (!nvc) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        b->vc = nvc;
        b->vcap = nv;
    }
    for (i = 0; i < nv; i++) {
        uint32_t c = v[i].color;
        b->vc[i].r = (float)(c >> 24) / 255.0f;
        b->vc[i].g = (float)((c >> 16) & 0xFFu) / 255.0f;
        b->vc[i].b = (float)((c >> 8) & 0xFFu) / 255.0f;
        b->vc[i].a = (float)(c & 0xFFu) / 255.0f;
    }
    SDL_GetRenderScale(b->r, &sx, &sy);
    SDL_GetRenderViewport(b->r, &vp);
    had_clip = SDL_RenderClipEnabled(b->r);
    if (had_clip)
        SDL_GetRenderClipRect(b->r, &cl);
    SDL_SetRenderScale(b->r, 1, 1);
    SDL_SetRenderViewport(b->r, NULL);
    SDL_GetCurrentRenderOutputSize(b->r, &ow, &oh);
    if (present) {
        SDL_SetRenderDrawColor(b->r, (Uint8)(clear >> 24), (Uint8)(clear >> 16), (Uint8)(clear >> 8), (Uint8)clear);
        SDL_SetRenderClipRect(b->r, NULL);
        SDL_RenderClear(b->r);
    }
    for (i = 0; i < nb; i++) {
        SDL_Rect r;
        SDL_Texture* t = bt[i].texture == DYN_UI_TEX_ATLAS ? b->atlas : sdlb_image(b, ui, bt[i].texture);
        r.x = (int)bt[i].clip.x;
        r.y = (int)bt[i].clip.y;
        r.w = (int)bt[i].clip.w;
        r.h = (int)bt[i].clip.h;
        if (!t || bt[i].count == 0)
            continue;
        if (r.x <= 0 && r.y <= 0 && r.w >= ow && r.h >= oh)
            SDL_SetRenderClipRect(b->r, NULL);
        else
            SDL_SetRenderClipRect(b->r, &r);
        SDL_RenderGeometryRaw(b->r, t, &v[0].x, (int)sizeof(*v), b->vc, (int)sizeof(*b->vc), &v[0].u, (int)sizeof(*v), (int)nv, idx + bt[i].first, (int)bt[i].count,
            (int)sizeof(*idx));
    }
    SDL_SetRenderScale(b->r, sx, sy);
    SDL_SetRenderViewport(b->r, &vp);
    SDL_SetRenderClipRect(b->r, had_clip ? &cl : NULL);
    if (present)
        SDL_RenderPresent(b->r);
    return 1;
}

static void sdlb_idle(void* be, double seconds)
{
    sdlb* b = be;
    int ms = seconds > 0 ? (seconds > 1e6 ? 1000000000 : (int)ceil(seconds * 1000)) : 0;
    size_t n;
    SDL_LockMutex(b->mu);
    n = b->n;
    SDL_UnlockMutex(b->mu);
    if (n || b->dirty || ms == 0)
        return;
    if (SDL_GetTicksNS() < b->repaint_until && ms > 16)
        ms = 16;
    SDL_WaitEventTimeout(NULL, ms);
}

static void sdlb_release(void* be, JSValueConst target)
{
    sdlb* b = be;
    size_t i;
    int alive = dyn_sdl_renderer_alive(target);
    SDL_RemoveEventWatch(sdlb_watch, b);
    if (alive) {
        if (b->text_active && b->w)
            SDL_StopTextInput(b->w);
        sdlb_drop_textures(b);
    }
    for (i = 0; i < b->n; i++)
        SDL_free(b->q[i].text);
    SDL_free(b->q);
    SDL_free(b->conv);
    SDL_free(b->vc);
    SDL_free(b->clip);
    if (b->mu)
        SDL_DestroyMutex(b->mu);
    dyn_nat_free(b);
}

static const dyn_uib_ops sdlb_ops = { sdlb_begin, sdlb_draw, sdlb_idle, sdlb_release, "sdl" };

void* dyn_uib_sdl_attach(JSContext* ctx, JSValueConst target, dyn_ui_t* ui, const dyn_uib_ops** ops)
{
    void *r = NULL, *w = NULL;
    sdlb* b;
    int rc = dyn_sdl_renderer_parts(ctx, target, &r, &w);
    if (rc <= 0)
        return NULL;
    b = dyn_nat_calloc(1, sizeof(*b));
    if (!b) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    b->ui = ui;
    b->r = r;
    b->w = w;
    b->wid = SDL_GetWindowID(b->w);
    b->density = 1;
    b->dirty = 1;
    b->mu = SDL_CreateMutex();
    if (!b->mu || !SDL_AddEventWatch(sdlb_watch, b)) {
        if (b->mu)
            SDL_DestroyMutex(b->mu);
        dyn_nat_free(b);
        JS_ThrowInternalError(ctx, "dyna:ui: SDL event watch: %s", SDL_GetError());
        return NULL;
    }
    dyn_ui_set_clipboard(ui, sdlb_clip_get, sdlb_clip_set, b);
    *ops = &sdlb_ops;
    return b;
}

#endif
