#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

static void rl_img_dispose(void* p)
{
    Image* im = p;
    UnloadImage(*im);
    dyn_nat_free(im);
}

static Image* rl_img_of(JSContext* ctx, JSValueConst v)
{
    return dyn_res_native(ctx, v, rl_img_cid);
}

JSValue rl_image_wrap(JSContext* ctx, JSValueConst new_target, Image im)
{
    Image* p = dyn_nat_malloc(sizeof(*p));
    if (!p) {
        UnloadImage(im);
        return JS_ThrowOutOfMemory(ctx);
    }
    *p = im;
    return dyn_res_wrap(ctx, new_target, rl_img_cid, p, rl_img_dispose);
}

JSValue rl_image_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int w, h;
    Color c = BLANK;
    const uint8_t* px = NULL;
    size_t off = 0, blen = 0, bpe = 0, ab = 0;
    Image im;
    if (rl_args(ctx, argc, argv, 2, "new Image(width, height, fill?)") || rl_dims(ctx, argv[0], argv[1], "Image", &w, &h))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_IsNumber(argv[2])) {
            if (rl_color(ctx, argv[2], "Image fill", &c))
                return JS_EXCEPTION;
        } else {
            JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[2], &off, &blen, &bpe);
            uint8_t* base;
            if (JS_IsException(buf))
                return JS_EXCEPTION;
            base = JS_GetArrayBuffer(ctx, &ab, buf);
            JS_FreeValue(ctx, buf);
            if (!base)
                return JS_EXCEPTION;
            if (bpe != 1)
                return JS_ThrowTypeError(ctx, "dyna:raylib: Image fill is a colour or a Uint8Array of RGBA");
            if (off > ab || blen > ab - off || blen != (size_t)w * (size_t)h * 4)
                return JS_ThrowRangeError(ctx, "dyna:raylib: Image pixels must be exactly %zu bytes of RGBA for %dx%d, got %zu",
                    (size_t)w * (size_t)h * 4, w, h, blen);
            px = base + off;
        }
    }
    im = GenImageColor(w, h, c);
    if (!im.data)
        return JS_ThrowOutOfMemory(ctx);
    if (px)
        memcpy(im.data, px, (size_t)w * (size_t)h * 4);
    return rl_image_wrap(ctx, new_target, im);
}

JSValue rl_img_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    Image* im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, magic ? im->height : im->width);
}

JSValue rl_img_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Image* im = rl_img_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!im)
        return JS_EXCEPTION;
    return dyn_nat_u8_copy(ctx, im->data, (size_t)im->width * (size_t)im->height * 4);
}

JSValue rl_img_export(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    Image* im;
    int ok;
    if (rl_args(ctx, argc, argv, 1, "export(path)") || rl_path(ctx, argv[0], "export(path)", &path))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    ok = ExportImage(*im, path);
    JS_FreeCString(ctx, path);
    if (!ok)
        return JS_ThrowInternalError(ctx, "dyna:raylib: export failed (the extension picks the format: .png, .bmp, .qoi, .tga)");
    return JS_UNDEFINED;
}

JSValue rl_img_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    int w = 0, h = 0, x = 0, y = 0;
    Image* im;
    if (magic == 0) {
        if (rl_args(ctx, argc, argv, 2, "resize(width, height)") || rl_dims(ctx, argv[0], argv[1], "resize", &w, &h))
            return JS_EXCEPTION;
    } else if (magic == 1) {
        if (rl_args(ctx, argc, argv, 4, "crop(x, y, width, height)") || rl_int_in(ctx, argv[0], "crop x", 0, RL_MAX_DIM - 1, &x)
            || rl_int_in(ctx, argv[1], "crop y", 0, RL_MAX_DIM - 1, &y) || rl_dims(ctx, argv[2], argv[3], "crop", &w, &h))
            return JS_EXCEPTION;
    }
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ImageResize(im, w, h);
        if (!im->data || im->width != w || im->height != h)
            return JS_ThrowInternalError(ctx, "dyna:raylib: resize to %dx%d failed", w, h);
        break;
    case 1:
        if (x + w > im->width || y + h > im->height)
            return JS_ThrowRangeError(ctx, "dyna:raylib: crop must lie inside the %dx%d image", im->width, im->height);
        ImageCrop(im, (Rectangle) { (float)x, (float)y, (float)w, (float)h });
        break;
    case 2:
        ImageFlipVertical(im);
        break;
    case 3:
        ImageFlipHorizontal(im);
        break;
    }
    return JS_UNDEFINED;
}

JSValue rl_img_copy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Image* im = rl_img_of(ctx, this_val);
    Image c;
    (void)argc;
    (void)argv;
    if (!im)
        return JS_EXCEPTION;
    c = ImageCopy(*im);
    if (!c.data)
        return JS_ThrowOutOfMemory(ctx);
    return rl_image_wrap(ctx, JS_UNDEFINED, c);
}

JSValue rl_img_to_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Image* im;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, "Image#toTexture()"))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    return rl_texture_from_image(ctx, im);
}

JSValue rl_img_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 1, 3, 5, 5, 5, 4, 4, 7 };
    static const char* const names[] = { "clear(color)", "drawPixel(x, y, color)", "drawLine(x1, y1, x2, y2, color, thick?)",
        "drawRectangle(x, y, width, height, color)", "drawRectangleLines(x, y, width, height, color, thick?)",
        "drawCircle(cx, cy, radius, color)", "drawCircleLines(cx, cy, radius, color)", "drawTriangle(x1, y1, x2, y2, x3, y3, color)" };
    const int lim = RL_MAX_DIM * 4;
    int v[6] = { 0, 0, 0, 0, 0, 0 }, n = need[magic] - 1, thick = 1, i;
    Color c;
    Image* im;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        int is_size = ((magic == 3 || magic == 4) && i >= 2) || ((magic == 5 || magic == 6) && i == 2);
        if (rl_int_in(ctx, argv[i], is_size ? "a size" : "a coordinate", is_size ? 0 : -lim, lim, &v[i]))
            return JS_EXCEPTION;
    }
    if (rl_color(ctx, argv[n], "a draw colour", &c))
        return JS_EXCEPTION;
    if ((magic == 2 || magic == 4) && argc > n + 1 && !JS_IsUndefined(argv[n + 1])
        && rl_int_in(ctx, argv[n + 1], "line thickness", 1, 4096, &thick))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ImageClearBackground(im, c);
        break;
    case 1:
        ImageDrawPixel(im, v[0], v[1], c);
        break;
    case 2:
        if (thick > 1)
            ImageDrawLineEx(im, (Vector2) { (float)v[0], (float)v[1] }, (Vector2) { (float)v[2], (float)v[3] }, thick, c);
        else
            ImageDrawLine(im, v[0], v[1], v[2], v[3], c);
        break;
    case 3:
        ImageDrawRectangle(im, v[0], v[1], v[2], v[3], c);
        break;
    case 4:
        ImageDrawRectangleLinesEx(im, (Rectangle) { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }, thick, c);
        break;
    case 5:
        ImageDrawCircle(im, v[0], v[1], v[2], c);
        break;
    case 6:
        ImageDrawCircleLines(im, v[0], v[1], v[2], c);
        break;
    default:
        ImageDrawTriangle(im, (Vector2) { (float)v[0], (float)v[1] }, (Vector2) { (float)v[2], (float)v[3] },
            (Vector2) { (float)v[4], (float)v[5] }, c);
        break;
    }
    return JS_UNDEFINED;
}

JSValue rl_img_draw_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const int lim = RL_MAX_DIM * 4;
    int x, y;
    Color tint;
    Image *dst, *src, tmp;
    if (rl_args(ctx, argc, argv, 3, "drawImage(source, x, y, tint?)") || rl_int_in(ctx, argv[1], "drawImage x", -lim, lim, &x)
        || rl_int_in(ctx, argv[2], "drawImage y", -lim, lim, &y) || rl_color_opt(ctx, argc, argv, 3, "drawImage tint", &tint))
        return JS_EXCEPTION;
    if (JS_GetClassID(argv[0]) != rl_img_cid)
        return JS_ThrowTypeError(ctx, "dyna:raylib: drawImage source must be an Image");
    dst = rl_img_of(ctx, this_val);
    if (!dst)
        return JS_EXCEPTION;
    src = rl_img_of(ctx, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    if (src != dst) {
        ImageDrawImage(dst, *src, x, y, tint);
        return JS_UNDEFINED;
    }
    tmp = ImageCopy(*src);
    if (!tmp.data)
        return JS_ThrowOutOfMemory(ctx);
    ImageDrawImage(dst, tmp, x, y, tint);
    UnloadImage(tmp);
    return JS_UNDEFINED;
}

JSValue rl_img_draw_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const int lim = RL_MAX_DIM * 4;
    int x, y, size;
    const char* s;
    Color c;
    Image* im;
    if (rl_args(ctx, argc, argv, 5, "drawText(text, x, y, fontSize, color)"))
        return JS_EXCEPTION;
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: Image#drawText needs a text string");
    if (rl_int_in(ctx, argv[1], "drawText x", -lim, lim, &x) || rl_int_in(ctx, argv[2], "drawText y", -lim, lim, &y)
        || rl_int_in(ctx, argv[3], "fontSize", 1, 4096, &size) || rl_color(ctx, argv[4], "drawText color", &c))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "Image#drawText() (the default font)"))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    if (rl_text_arg(ctx, argv[0], "Image#drawText", &s))
        return JS_EXCEPTION;
    ImageDrawText(im, s, x, y, size, c);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

#endif
