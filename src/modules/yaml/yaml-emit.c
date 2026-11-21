#include "yaml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

typedef struct {
    JSContext* ctx;
    yb_t b;
    int indent, depth;
    int width;
    int sort_keys;
    int flow;
} yml_w_t;

static int yml_docsep_at(const yml_t* p, uint32_t li)
{
    const yml_line_t* L = &p->ln[li];
    size_t k;
    if (L->end - L->raw < 3)
        return 0;
    if (memcmp(p->s + L->raw, "---", 3) != 0
        && memcmp(p->s + L->raw, "...", 3) != 0)
        return 0;
    k = L->raw + 3;
    return k >= L->end || p->s[k] == ' ' || p->s[k] == '\t';
}

JSValue yml_quoted(yml_t* p, size_t* pi, size_t end, int multiline)
{
    char q = p->s[*pi];
    size_t i = *pi + 1;
    yb_t b;
    size_t hard = 0;

    {
        const char* s = p->s;
        size_t j = i;
        if (q == '"')
            while (j < end && s[j] != '"' && s[j] != '\\')
                j++;
        else
            while (j < end && s[j] != '\'')
                j++;
        if (j < end && s[j] == q && !(q == '\'' && j + 1 < end && s[j + 1] == '\'')) {
            *pi = j + 1;
            return JS_NewStringLen(p->ctx, s + i, j - i);
        }
    }
    yb_init(&b);
    for (;;) {
        int esc_break = 0;
        while (i < end) {
            char c = p->s[i];
            if (c == q) {
                if (q == '\'' && i + 1 < end && p->s[i + 1] == '\'') {
                    yb_put(&b, (uint8_t)'\'');
                    hard = b.n;
                    i += 2;
                    continue;
                }
                i++;
                *pi = i;
                {
                    JSValue v = JS_NewStringLen(p->ctx, (const char*)b.p, b.n);
                    yb_free(&b);
                    return v;
                }
            }
            if (q == '"' && c == '\\') {
                i++;
                if (i >= end) {
                    esc_break = 1;
                    break;
                }
                switch (p->s[i]) {
                case 'n':
                    yb_put(&b, (uint8_t)'\n');
                    break;
                case 't':
                    yb_put(&b, (uint8_t)'\t');
                    break;
                case 'r':
                    yb_put(&b, (uint8_t)'\r');
                    break;
                case '0':
                    yb_put(&b, (uint8_t)'\0');
                    break;
                case 'a':
                    yb_put(&b, (uint8_t)'\a');
                    break;
                case 'b':
                    yb_put(&b, (uint8_t)'\b');
                    break;
                case 'v':
                    yb_put(&b, (uint8_t)'\v');
                    break;
                case 'f':
                    yb_put(&b, (uint8_t)'\f');
                    break;
                case 'e':
                    yb_put(&b, (uint8_t)0x1B);
                    break;
                case ' ':
                    yb_put(&b, (uint8_t)' ');
                    break;
                case '\\':
                    yb_put(&b, (uint8_t)'\\');
                    break;
                case '"':
                    yb_put(&b, (uint8_t)'"');
                    break;
                case '/':
                    yb_put(&b, (uint8_t)'/');
                    break;
                case 'N':
                    yb_puts(&b, "\xC2\x85");
                    break;
                case '_':
                    yb_puts(&b, "\xC2\xA0");
                    break;
                case 'L':
                    yb_puts(&b, "\xE2\x80\xA8");
                    break;
                case 'P':
                    yb_puts(&b, "\xE2\x80\xA9");
                    break;
                case 'x':
                case 'u':
                case 'U': {
                    int width = p->s[i] == 'x' ? 2 : p->s[i] == 'u' ? 4
                                                                    : 8;
                    uint32_t cp;
                    if (i + (size_t)width >= end
                        || yml_hex(p->s, i + 1, width, &cp) < 0) {
                        yb_free(&b);
                        return yml_fail(p, "bad \\u escape");
                    }
                    if (cp > 0x10FFFF) {
                        yb_free(&b);
                        return yml_fail(p, "code point past U+10FFFF");
                    }
                    i += (size_t)width;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        int ok = 0;
                        if (i + 7 < end && p->s[i + 1] == '\\' && p->s[i + 2] == 'u') {
                            ok = yml_hex(p->s, i + 3, 4, &lo) == 0
                                && lo >= 0xDC00 && lo <= 0xDFFF;
                        }
                        if (!ok) {
                            yb_free(&b);
                            return yml_fail(p, "lone surrogate \\u escape");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        yb_free(&b);
                        return yml_fail(p, "lone surrogate \\u escape");
                    }
                    yml_utf8_put(&b, cp);
                    break;
                }
                default:
                    yb_free(&b);
                    return yml_fail(p, "unknown escape in a double-quoted scalar");
                }
                hard = b.n;
                i++;
                continue;
            }
            if (c == ' ' || c == '\t') {
                yb_put(&b, (uint8_t)c);
                i++;
                continue;
            }
            yb_put(&b, (uint8_t)(uint8_t)c);
            hard = b.n;
            i++;
        }
        if (!multiline || p->li + 1 >= p->nln) {
            yb_free(&b);
            return yml_fail(p, "unterminated quoted scalar");
        }
        if (esc_break) {
            uint32_t li = p->li + 1;
            size_t k = p->ln[li].raw;
            for (;;) {
                if (yml_docsep_at(p, li)) {
                    yb_free(&b);
                    return yml_fail(p, "unexpected document separator in a "
                                       "quoted scalar");
                }
                k = p->ln[li].raw;
                while (k < p->ln[li].end && (p->s[k] == ' ' || p->s[k] == '\t'))
                    k++;
                if (k < p->ln[li].end)
                    break;
                if (li + 1 >= p->nln) {
                    yb_free(&b);
                    return yml_fail(p, "unterminated quoted scalar");
                }
                yb_put(&b, (uint8_t)'\n');
                li++;
            }
            p->li = li;
            i = k;
            end = p->ln[li].end;
            continue;
        }
        {
            uint32_t li = p->li + 1, extra = 0;
            size_t k;
            b.n = hard;
            for (;;) {
                if (yml_docsep_at(p, li)) {
                    yb_free(&b);
                    return yml_fail(p, "unexpected document separator in a "
                                       "quoted scalar");
                }
                k = p->ln[li].raw;
                while (k < p->ln[li].end && (p->s[k] == ' ' || p->s[k] == '\t'))
                    k++;
                if (k < p->ln[li].end)
                    break;
                if (li + 1 >= p->nln) {
                    yb_free(&b);
                    return yml_fail(p, "unterminated quoted scalar");
                }
                extra++;
                li++;
            }
            if (yml_tab_refuse(p, li)) {
                yb_free(&b);
                return JS_EXCEPTION;
            }
            if (extra) {
                while (extra--)
                    yb_put(&b, (uint8_t)'\n');
            } else {
                yb_put(&b, (uint8_t)' ');
            }
            p->li = li;
            i = k;
            end = p->ln[li].end;
            continue;
        }
    }
}

static const char* const yml_stringify_keys[] = { "indent", "width", "sortKeys", "flow" };

static int yml_emit_value(yml_w_t* w, JSValueConst v, int depth, int same_line);

static void yml_put_scalar_str(yml_w_t* w, const char* s, size_t n, int flow)
{
    if (!yml_needs_quote(s, n, flow)) {
        yb_write(&w->b, s, n);
        return;
    }
    yb_put(&w->b, (uint8_t)'"');
    yml_put_escaped_body(&w->b, s, n);
    yb_put(&w->b, (uint8_t)'"');
}

static void yml_put_scalar_folded(yml_w_t* w, const char* s, size_t n,
    int cont_col)
{
    yb_t e;
    size_t i = 0, line_start = 0, last_ok = 0, last_word = 0, br;
    int budget, col, line_has_output;

    yb_init(&e);
    yml_put_escaped_body(&e, s, n);
    budget = w->width;
    if (budget < 16)
        budget = 16;
    yb_put(&w->b, (uint8_t)'"');
    col = 0;
    line_has_output = 0;
    while (i < e.n) {
        int ul;
        if (e.p[i] == '\\')
            ul = (i + 1 < e.n && e.p[i + 1] == 'u') ? 6 : 2;
        else if ((unsigned char)e.p[i] >= 0x80)
            ul = yml_utf8_unit((const char*)e.p, i, e.n);
        else
            ul = 1;
        if (line_has_output && col + 1 > budget) {
            br = last_word > line_start ? last_word : last_ok;
            if (i < e.n && e.p[i] == ' ') {
                size_t run = i;
                while (run < e.n && e.p[run] == ' ')
                    run++;
                if (run < e.n) {
                    yb_write(&w->b, e.p + i, run - i);
                    col += (int)(run - i);
                    i = run;
                    br = i;
                } else {
                    br = 0;
                }
            }
            if (br > line_start) {
                col -= (int)(i - br);
                w->b.n -= i - br;
                i = br;
                last_ok = last_word = 0;
                yb_put(&w->b, (uint8_t)'\\');
                yb_put(&w->b, (uint8_t)'\n');
                {
                    int q;
                    for (q = 0; q < cont_col; q++)
                        yb_put(&w->b, (uint8_t)' ');
                }
                line_start = i;
                col = 0;
                budget = w->width - cont_col;
                if (budget < 16)
                    budget = 16;
                line_has_output = 0;
                continue;
            }
        }
        yb_write(&w->b, e.p + i, (size_t)ul);
        i += (size_t)ul;
        col++;
        line_has_output = 1;
        if (i < e.n && e.p[i] != ' ') {
            last_ok = i;
            if (e.p[i - 1] == ' ')
                last_word = i;
        }
    }
    yb_put(&w->b, (uint8_t)'"');
    yb_free(&e);
}

static int yml_emit_scalar(yml_w_t* w, JSValueConst v, int cont_col, int flow)
{
    const char* s;
    size_t n;
    JSValue sv;

    if (JS_IsNull(v) || JS_IsUndefined(v) || JS_IsFunction(w->ctx, v)) {
        yb_puts(&w->b, "null");
        return 0;
    }
    if (JS_IsBool(v)) {
        yb_puts(&w->b, JS_ToBool(w->ctx, v) ? "true" : "false");
        return 0;
    }
    sv = JS_ToString(w->ctx, v);
    if (JS_IsException(sv))
        return -1;
    s = JS_ToCStringLen(w->ctx, &n, sv);
    JS_FreeValue(w->ctx, sv);
    if (!s)
        return -1;
    if (JS_IsBigInt(w->ctx, v)) {
        yb_write(&w->b, s, n);
    } else if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(w->ctx, &d, v);
        if (isnan(d))
            yb_puts(&w->b, ".nan");
        else if (isinf(d))
            yb_puts(&w->b, d > 0 ? ".inf" : "-.inf");
        else if (d == 0 && signbit(d))
            yb_puts(&w->b, "-0.0");
        else {
            yb_write(&w->b, s, n);
            if ((d > 9007199254740991.0 || d < -9007199254740991.0)
                && !memchr(s, '.', n) && !memchr(s, 'e', n))
                yb_puts(&w->b, ".0");
        }
    } else if (JS_IsString(v) && cont_col >= 0 && w->width > 0
        && n > (size_t)w->width)
        yml_put_scalar_folded(w, s, n, cont_col);
    else
        yml_put_scalar_str(w, s, n, flow);
    JS_FreeCString(w->ctx, s);
    return 0;
}

static void yml_nl_indent(yml_w_t* w, int depth)
{
    int k;
    yb_put(&w->b, (uint8_t)'\n');
    for (k = 0; k < depth * w->indent; k++)
        yb_put(&w->b, (uint8_t)' ');
}

static int yml_emit_seq(yml_w_t* w, JSValueConst v, int depth, int same_line,
    int64_t len)
{
    int64_t i;

    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(w->ctx, v, (uint32_t)i);
        int64_t elen = 0;
        int block, rc;
        if (JS_IsException(e))
            return -1;
        if (i || !same_line)
            yml_nl_indent(w, depth);
        yb_puts(&w->b, "- ");
        block = yml_is_block(w->ctx, e, &elen);
        rc = block < 0 ? -1 : yml_emit_value(w, e, depth + 1, 1);
        JS_FreeValue(w->ctx, e);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int yml_emit_map(yml_w_t* w, JSValueConst v, int depth, int same_line)
{
    yml_prop_t* tab = NULL;
    uint32_t len = 0, k, emitted = 0;
    int rc = 0;

    if (yml_props(w->ctx, v, w->sort_keys, &tab, &len) < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        JSValue e = JS_GetProperty(w->ctx, v, tab[k].e.atom);
        int64_t elen = 0;
        int block;

        if (JS_IsException(e)) {
            rc = -1;
            break;
        }
        if (JS_IsFunction(w->ctx, e)) {
            JS_FreeValue(w->ctx, e);
            continue;
        }
        if (emitted++ || !same_line)
            yml_nl_indent(w, depth);
        yml_put_scalar_str(w, tab[k].name, strlen(tab[k].name), 0);
        yb_put(&w->b, (uint8_t)':');
        block = yml_is_block(w->ctx, e, &elen);
        if (block < 0) {
            rc = -1;
        } else if (block) {
            rc = yml_emit_value(w, e, depth + 1, 0);
        } else {
            yb_put(&w->b, (uint8_t)' ');
            rc = yml_emit_value(w, e, depth + 1, 1);
        }
        JS_FreeValue(w->ctx, e);
    }
    yml_props_free(w->ctx, tab, len);
    if (rc == 0 && emitted == 0)
        yb_puts(&w->b, same_line ? "{}" : " {}");
    return rc;
}

static int yml_emit_flow(yml_w_t* w, JSValueConst v, int depth)
{
    int64_t len = 0;
    int block;

    if (depth >= YML_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "Stringify: nesting exceeds %d", YML_MAX_DEPTH);
        return -1;
    }
    block = yml_is_block(w->ctx, v, &len);
    if (block < 0)
        return -1;
    if (block <= 0) {
        if (JS_IsObject(v) && !JS_IsFunction(w->ctx, v)) {
            yb_puts(&w->b, JS_IsArray(w->ctx, v) == 1 ? "[]" : "{}");
            return 0;
        }
        return yml_emit_scalar(w, v, -1, 1);
    }
    if (JS_IsArray(w->ctx, v) == 1) {
        int64_t i;
        yb_put(&w->b, (uint8_t)'[');
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(w->ctx, v, (uint32_t)i);
            int rc;
            if (JS_IsException(e))
                return -1;
            if (i)
                yb_puts(&w->b, ", ");
            rc = yml_emit_flow(w, e, depth + 1);
            JS_FreeValue(w->ctx, e);
            if (rc < 0)
                return -1;
        }
        yb_put(&w->b, (uint8_t)']');
        return 0;
    }
    {
        yml_prop_t* tab = NULL;
        uint32_t n = 0, k, emitted = 0;
        int rc = 0;
        if (yml_props(w->ctx, v, w->sort_keys, &tab, &n) < 0)
            return -1;
        yb_put(&w->b, (uint8_t)'{');
        for (k = 0; k < n; k++) {
            JSValue e = JS_GetProperty(w->ctx, v, tab[k].e.atom);
            int wrc;
            if (JS_IsException(e)) {
                rc = -1;
                break;
            }
            if (JS_IsFunction(w->ctx, e)) {
                JS_FreeValue(w->ctx, e);
                continue;
            }
            if (emitted++)
                yb_puts(&w->b, ", ");
            yml_put_scalar_str(w, tab[k].name, strlen(tab[k].name), 1);
            yb_puts(&w->b, ": ");
            wrc = (JS_IsObject(e) && !JS_IsFunction(w->ctx, e))
                ? yml_emit_flow(w, e, depth + 1)
                : yml_emit_scalar(w, e, -1, 1);
            JS_FreeValue(w->ctx, e);
            if (wrc < 0) {
                rc = wrc;
                break;
            }
        }
        yb_put(&w->b, (uint8_t)'}');
        yml_props_free(w->ctx, tab, n);
        return rc;
    }
}

static int yml_emit_value(yml_w_t* w, JSValueConst v, int depth, int same_line)
{
    int64_t len = 0;
    int block;

    if (depth >= YML_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "Stringify: nesting exceeds %d", YML_MAX_DEPTH);
        return -1;
    }
    block = yml_is_block(w->ctx, v, &len);
    if (block < 0)
        return -1;
    if (w->flow)
        return yml_emit_flow(w, v, depth);
    if (!block) {
        if (JS_IsObject(v) && !JS_IsFunction(w->ctx, v)) {
            yb_puts(&w->b, JS_IsArray(w->ctx, v) == 1 ? "[]" : "{}");
            return 0;
        }
        return yml_emit_scalar(w, v, depth * w->indent + 2, 0);
    }
    if (JS_IsArray(w->ctx, v) == 1)
        return yml_emit_seq(w, v, depth, same_line, len);
    return yml_emit_map(w, v, depth, same_line);
}

JSValue yml_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    yml_w_t w;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Stringify(value, options): a value is required");
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    w.indent = 2;
    yb_init(&w.b);
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], yml_stringify_keys,
                countof(yml_stringify_keys))
            < 0) {
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "indent");
        int32_t ind = 2;
        if (JS_IsException(v)) {
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &ind, v) < 0) {
            JS_FreeValue(ctx, v);
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (ind < 1 || ind > 10) {
            yb_free(&w.b);
            return JS_ThrowRangeError(ctx, "Stringify: indent is 1 to 10");
        }
        w.indent = ind;
        v = JS_GetPropertyStr(ctx, argv[1], "width");
        if (JS_IsException(v)) {
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) || !(d >= 0 && d <= 65536) || d != (double)(int64_t)d) {
                JS_FreeValue(ctx, v);
                yb_free(&w.b);
                return JS_ThrowTypeError(ctx,
                    "Stringify: width must be an integer 0..65536");
            }
            w.width = (int)d;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "sortKeys");
        if (JS_IsException(v)) {
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (!JS_IsBool(v)) {
                JS_FreeValue(ctx, v);
                yb_free(&w.b);
                return JS_ThrowTypeError(ctx,
                    "Stringify: sortKeys must be a boolean");
            }
            w.sort_keys = JS_ToBool(ctx, v);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "flow");
        if (JS_IsException(v)) {
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (!JS_IsBool(v)) {
                JS_FreeValue(ctx, v);
                yb_free(&w.b);
                return JS_ThrowTypeError(ctx,
                    "Stringify: flow must be a boolean");
            }
            w.flow = JS_ToBool(ctx, v);
        }
        JS_FreeValue(ctx, v);
    }
    if (yml_emit_value(&w, argv[0], 0, 1) < 0) {
        yb_free(&w.b);
        return JS_EXCEPTION;
    }
    yb_put(&w.b, (uint8_t)'\n');
    out = JS_NewStringLen(ctx, (const char*)w.b.p, w.b.n);
    yb_free(&w.b);
    return out;
}

#endif
