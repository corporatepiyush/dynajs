#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

#define RL_MAX_POINTS 65536

static int rl_drawable(JSContext* ctx, JSValueConst v, const char* what, Texture2D* tex, int* flip)
{
    JSClassID cid = JS_GetClassID(v);
    rl_gpu* g;
    if (cid != rl_tex_cid && cid != rl_rt_cid) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a Texture or a RenderTexture", what);
        return -1;
    }
    g = rl_gpu_of(ctx, v, cid);
    if (!g)
        return -1;
    if (g == rl.active_rt) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s: a RenderTexture cannot be drawn inside its own draw()", what);
        return -1;
    }
    *tex = cid == rl_rt_cid ? g->rt.texture : g->tex;
    *flip = cid == rl_rt_cid;
    return 0;
}

JSValue rl_draw2d(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 3, 5, 4, 4, 5, 5, 5, 7, 7, 6 };
    static const char* const names[] = { "drawPixel(x, y, color)", "drawLine(x1, y1, x2, y2, color, thick?)",
        "drawCircle(cx, cy, radius, color)", "drawCircleLines(cx, cy, radius, color)", "drawEllipse(cx, cy, rx, ry, color)",
        "drawRectangle(x, y, width, height, color)", "drawRectangleLines(x, y, width, height, color, thick?)",
        "drawRectangleRounded(x, y, width, height, roundness, segments, color)", "drawTriangle(x1, y1, x2, y2, x3, y3, color)",
        "drawPoly(cx, cy, sides, radius, rotation, color)" };
    float f[6] = { 0, 0, 0, 0, 0, 0 }, extra = 1;
    int n = need[magic] - 1, seg = 0, sides = 3;
    Color c;
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    if (magic == 7) {
        if (rl_nums(ctx, argv, 4, names[magic], f) || rl_num_in(ctx, argv[4], "drawRectangleRounded roundness", 0, 1, &f[4])
            || rl_int_in(ctx, argv[5], "drawRectangleRounded segments", 0, 1024, &seg) || rl_color(ctx, argv[6], "drawRectangleRounded color", &c))
            return JS_EXCEPTION;
    } else if (magic == 9) {
        if (rl_nums(ctx, argv, 2, names[magic], f) || rl_int_in(ctx, argv[2], "drawPoly sides", 3, 4096, &sides)
            || rl_num(ctx, argv[3], names[magic], &f[2]) || rl_num(ctx, argv[4], names[magic], &f[3])
            || rl_color(ctx, argv[5], "drawPoly color", &c))
            return JS_EXCEPTION;
    } else {
        if (rl_nums(ctx, argv, n, names[magic], f) || rl_color(ctx, argv[n], "a draw colour", &c))
            return JS_EXCEPTION;
        if ((magic == 1 || magic == 6) && argc > n + 1 && !JS_IsUndefined(argv[n + 1])
            && rl_num_in(ctx, argv[n + 1], "line thickness", 0, 4096, &extra))
            return JS_EXCEPTION;
    }
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawPixelV((Vector2) { f[0], f[1] }, c);
        break;
    case 1:
        DrawLineEx((Vector2) { f[0], f[1] }, (Vector2) { f[2], f[3] }, extra, c);
        break;
    case 2:
        DrawCircleV((Vector2) { f[0], f[1] }, f[2], c);
        break;
    case 3:
        DrawCircleLinesV((Vector2) { f[0], f[1] }, f[2], c);
        break;
    case 4:
        DrawEllipse((int)f[0], (int)f[1], f[2], f[3], c);
        break;
    case 5:
        DrawRectangleRec((Rectangle) { f[0], f[1], f[2], f[3] }, c);
        break;
    case 6:
        DrawRectangleLinesEx((Rectangle) { f[0], f[1], f[2], f[3] }, extra, c);
        break;
    case 7:
        DrawRectangleRounded((Rectangle) { f[0], f[1], f[2], f[3] }, f[4], seg, c);
        break;
    case 8:
        DrawTriangle((Vector2) { f[0], f[1] }, (Vector2) { f[2], f[3] }, (Vector2) { f[4], f[5] }, c);
        break;
    case 9:
        DrawPoly((Vector2) { f[0], f[1] }, sides, f[2], f[3], c);
        break;
    }
    return JS_UNDEFINED;
}

JSValue rl_draw_grid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int slices;
    float spacing;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 2, "drawGrid(slices, spacing)") || rl_int_in(ctx, argv[0], "drawGrid slices", 1, 1024, &slices)
        || rl_num_in(ctx, argv[1], "drawGrid spacing", 0, 1e6, &spacing))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawGrid()"))
        return JS_EXCEPTION;
    DrawGrid(slices, spacing);
    return JS_UNDEFINED;
}

JSValue rl_draw_fps(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[2];
    (void)this_val;
    if (rl_args(ctx, argc, argv, 2, "drawFPS(x, y)") || rl_nums(ctx, argv, 2, "drawFPS(x, y)", f))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawFPS()"))
        return JS_EXCEPTION;
    DrawFPS((int)f[0], (int)f[1]);
    return JS_UNDEFINED;
}

static int rl_font_arg(JSContext* ctx, JSValueConst v, Font* f)
{
    rl_gpu* g;
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        *f = GetFontDefault();
        return 0;
    }
    if (JS_GetClassID(v) != rl_font_cid) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the font must be a Font or null for the default font");
        return -1;
    }
    g = rl_gpu_of(ctx, v, rl_font_cid);
    if (!g)
        return -1;
    *f = g->font;
    return 0;
}

JSValue rl_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 5, 2, 7, 4 };
    static const char* const names[] = { "drawText(text, x, y, fontSize, color)", "measureText(text, fontSize)",
        "drawTextEx(font, text, x, y, fontSize, spacing, color)", "measureTextEx(font, text, fontSize, spacing)" };
    const char* s;
    float x = 0, y = 0, size = 10, spacing = 0;
    int isize = 10, ti = magic >= 2 ? 1 : 0;
    Color c = WHITE;
    Font font;
    JSValue ret = JS_UNDEFINED;
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    if (!JS_IsString(argv[ti]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a text string", names[magic]);
    switch (magic) {
    case 0:
        if (rl_num(ctx, argv[1], names[0], &x) || rl_num(ctx, argv[2], names[0], &y)
            || rl_int_in(ctx, argv[3], "fontSize", 1, 4096, &isize) || rl_color(ctx, argv[4], "drawText color", &c))
            return JS_EXCEPTION;
        break;
    case 1:
        if (rl_int_in(ctx, argv[1], "fontSize", 1, 4096, &isize))
            return JS_EXCEPTION;
        break;
    case 2:
        if (rl_num(ctx, argv[2], names[2], &x) || rl_num(ctx, argv[3], names[2], &y)
            || rl_num_in(ctx, argv[4], "fontSize", 1, 4096, &size) || rl_num_in(ctx, argv[5], "spacing", -4096, 4096, &spacing)
            || rl_color(ctx, argv[6], "drawTextEx color", &c))
            return JS_EXCEPTION;
        break;
    case 3:
        if (rl_num_in(ctx, argv[2], "fontSize", 1, 4096, &size) || rl_num_in(ctx, argv[3], "spacing", -4096, 4096, &spacing))
            return JS_EXCEPTION;
        break;
    }
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    if (magic >= 2 && rl_font_arg(ctx, argv[0], &font))
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, argv[ti]);
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawText(s, (int)x, (int)y, isize, c);
        break;
    case 1:
        ret = JS_NewInt32(ctx, MeasureText(s, isize));
        break;
    case 2:
        DrawTextEx(font, s, (Vector2) { x, y }, size, spacing, c);
        break;
    case 3: {
        Vector2 m = MeasureTextEx(font, s, size, spacing);
        ret = rl_xy(ctx, (double)m.x, (double)m.y, "width", "height");
        break;
    }
    }
    JS_FreeCString(ctx, s);
    return ret;
}

JSValue rl_draw_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[2];
    Color tint;
    Texture2D tex;
    int flip;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 3, "drawTexture(texture, x, y, tint?)") || rl_nums(ctx, argv + 1, 2, "drawTexture(texture, x, y, tint?)", f)
        || rl_color_opt(ctx, argc, argv, 3, "drawTexture tint", &tint))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawTexture()") || rl_drawable(ctx, argv[0], "drawTexture()", &tex, &flip))
        return JS_EXCEPTION;
    DrawTexturePro(tex, (Rectangle) { 0, 0, (float)tex.width, flip ? -(float)tex.height : (float)tex.height },
        (Rectangle) { f[0], f[1], (float)tex.width, (float)tex.height }, (Vector2) { 0, 0 }, 0, tint);
    return JS_UNDEFINED;
}

JSValue rl_draw_texture_pro(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Rectangle src, dst;
    float ox = 0, oy = 0, rot = 0;
    Color tint;
    Texture2D tex;
    int flip;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 3, "drawTexturePro(texture, src, dst, originX?, originY?, rotation?, tint?)")
        || rl_rect(ctx, argv[1], "drawTexturePro src", &src) || rl_rect(ctx, argv[2], "drawTexturePro dst", &dst)
        || (argc > 3 && !JS_IsUndefined(argv[3]) && rl_num(ctx, argv[3], "drawTexturePro originX", &ox))
        || (argc > 4 && !JS_IsUndefined(argv[4]) && rl_num(ctx, argv[4], "drawTexturePro originY", &oy))
        || (argc > 5 && !JS_IsUndefined(argv[5]) && rl_num(ctx, argv[5], "drawTexturePro rotation", &rot))
        || rl_color_opt(ctx, argc, argv, 6, "drawTexturePro tint", &tint))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawTexturePro()") || rl_drawable(ctx, argv[0], "drawTexturePro()", &tex, &flip))
        return JS_EXCEPTION;
    if (flip) {
        src.y = (float)tex.height - src.y - src.height;
        src.height = -src.height;
    }
    DrawTexturePro(tex, src, dst, (Vector2) { ox, oy }, rot, tint);
    return JS_UNDEFINED;
}

static int rl_points(JSContext* ctx, JSValueConst v, const char* who, int min, const Vector2** pts, int* n)
{
    size_t off, blen, bpe, ab, i, cnt;
    const float* f;
    uint8_t* base;
    JSValue buf;
    if (JS_GetClassID(v) != rl_f32_cid) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s takes a Float32Array of x, y pairs", who);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || blen > ab - off || blen % (2 * sizeof(float))) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s needs an even number of coordinates (x, y pairs)", who);
        return -1;
    }
    cnt = blen / (2 * sizeof(float));
    if (cnt < (size_t)min || cnt > RL_MAX_POINTS) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s takes %d to %d points, got %zu", who, min, RL_MAX_POINTS, cnt);
        return -1;
    }
    f = (const float*)(base + off);
    for (i = 0; i < cnt * 2; i++)
        if (!isfinite(f[i]) || fabsf(f[i]) > (float)RL_COORD_MAX) {
            JS_ThrowRangeError(ctx, "dyna:raylib: %s coordinate %zu must be a finite number within +-1e7", who, i);
            return -1;
        }
    *pts = (const Vector2*)f;
    *n = (int)cnt;
    return 0;
}

int rl_text_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out)
{
    size_t n;
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a text string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    if (dyn_wtf8_has_surrogate(s, n)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs well-formed text (it holds a lone surrogate)", who);
        return -1;
    }
    *out = s;
    return 0;
}

JSValue rl_draw_shape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 8, 7, 8 };
    static const char* const names[] = { "drawRing(cx, cy, innerRadius, outerRadius, startAngle, endAngle, segments, color)",
        "drawCircleSector(cx, cy, radius, startAngle, endAngle, segments, color)",
        "drawRectangleGradient(x, y, width, height, topLeft, bottomLeft, topRight, bottomRight)" };
    float f[6];
    int seg = 0;
    Color c[4];
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (rl_nums(ctx, argv, 2, names[0], f) || rl_num_in(ctx, argv[2], "drawRing innerRadius", 0, RL_COORD_MAX, &f[2])
            || rl_num_in(ctx, argv[3], "drawRing outerRadius", 0, RL_COORD_MAX, &f[3]) || rl_num(ctx, argv[4], names[0], &f[4])
            || rl_num(ctx, argv[5], names[0], &f[5]) || rl_int_in(ctx, argv[6], "drawRing segments", 0, 4096, &seg)
            || rl_color(ctx, argv[7], "drawRing color", &c[0]))
            return JS_EXCEPTION;
        break;
    case 1:
        if (rl_nums(ctx, argv, 2, names[1], f) || rl_num_in(ctx, argv[2], "drawCircleSector radius", 0, RL_COORD_MAX, &f[2])
            || rl_num(ctx, argv[3], names[1], &f[3]) || rl_num(ctx, argv[4], names[1], &f[4])
            || rl_int_in(ctx, argv[5], "drawCircleSector segments", 0, 4096, &seg) || rl_color(ctx, argv[6], "drawCircleSector color", &c[0]))
            return JS_EXCEPTION;
        break;
    default:
        if (rl_nums(ctx, argv, 4, names[2], f) || rl_color(ctx, argv[4], "drawRectangleGradient topLeft", &c[0])
            || rl_color(ctx, argv[5], "drawRectangleGradient bottomLeft", &c[1]) || rl_color(ctx, argv[6], "drawRectangleGradient topRight", &c[2])
            || rl_color(ctx, argv[7], "drawRectangleGradient bottomRight", &c[3]))
            return JS_EXCEPTION;
        break;
    }
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawRing((Vector2) { f[0], f[1] }, f[2], f[3], f[4], f[5], seg, c[0]);
        break;
    case 1:
        DrawCircleSector((Vector2) { f[0], f[1] }, f[2], f[3], f[4], seg, c[0]);
        break;
    default:
        DrawRectangleGradientEx((Rectangle) { f[0], f[1], f[2], f[3] }, c[0], c[1], c[3], c[2]);
        break;
    }
    return JS_UNDEFINED;
}

JSValue rl_draw_points(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "drawLineStrip(points, color, thick?)", "drawTriangleFan(points, color)" };
    const Vector2* p;
    int n;
    float thick = 0;
    Color c;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 2, names[magic]) || rl_color(ctx, argv[1], "a draw colour", &c))
        return JS_EXCEPTION;
    if (!magic && argc > 2 && !JS_IsUndefined(argv[2]) && rl_num_in(ctx, argv[2], "line thickness", 0, 4096, &thick))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    if (rl_points(ctx, argv[0], names[magic], magic ? 3 : 2, &p, &n))
        return JS_EXCEPTION;
    if (magic)
        DrawTriangleFan(p, n, c);
    else if (thick > 0)
        DrawSplineLinear(p, n, thick, c);
    else
        DrawLineStrip(p, n, c);
    return JS_UNDEFINED;
}

#endif
