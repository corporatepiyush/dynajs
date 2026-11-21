#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

static const char* const md_keys[] = { "allowRawHTML", "headingIds" };

JSValue dyn_md_render(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    hb_t html;
    int allow_html = 0, heading_ids = 0, rc;
    const char* s;
    size_t n;
    JSValue out;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "MarkdownToHTML(text): text must be a string");
    if (argc > 1 && dyn_opts_strict(ctx, argv[1], md_keys, countof(md_keys)) < 0)
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > MD_MAX_INPUT) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "MarkdownToHTML: input exceeds %u bytes",
            MD_MAX_INPUT);
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "allowRawHTML");
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        allow_html = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "headingIds");
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        heading_ids = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    rc = dyn_md_render_html(s, n, allow_html, heading_ids, &html);
    JS_FreeCString(ctx, s);
    if (rc < 0) {
        hb_free(&html);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, (const char*)html.p, html.n);
    hb_free(&html);
    return out;
}

#endif
