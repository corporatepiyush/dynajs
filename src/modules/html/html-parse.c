#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

static int ht_attr_dup(const ht_t* p, const char* n, size_t nl)
{
    uint32_t k;
    size_t i;
    for (k = 0; k < p->nattr; k++) {
        if (p->attr[k].nlen != nl)
            continue;
        for (i = 0; i < nl; i++)
            if (ht_lower((unsigned char)p->attr[k].n[i])
                != ht_lower((unsigned char)n[i]))
                break;
        if (i == nl)
            return 1;
    }
    return 0;
}

static int ht_scan_attr(ht_t* p)
{
    size_t st, name_len;
    int store;

    while (p->i < p->n && ht_space((unsigned char)p->s[p->i]))
        p->i++;
    if (p->i >= p->n || p->s[p->i] == '>')
        return 0;
    if (p->s[p->i] == '/') {
        p->i++;
        if (p->i >= p->n || p->s[p->i] == '>') {
            p->selfclose = 1;
            return 0;
        }
        return 1;
    }
    st = p->i;
    while (p->i < p->n && !ht_space((unsigned char)p->s[p->i])
        && p->s[p->i] != '=' && p->s[p->i] != '>' && p->s[p->i] != '/')
        p->i++;
    if (p->i == st) {
        p->i++;
        return 1;
    }
    name_len = p->i - st;
    store = p->nattr < HT_MAX_ATTRS;
    if (store) {
        p->attr[p->nattr].n = p->s + st;
        p->attr[p->nattr].nlen = name_len;
        p->attr[p->nattr].voff = p->abuf.n;
        p->attr[p->nattr].vlen = 0;
    }
    while (p->i < p->n && ht_space((unsigned char)p->s[p->i]))
        p->i++;
    if (p->i < p->n && p->s[p->i] == '=') {
        size_t vs;
        p->i++;
        while (p->i < p->n && ht_space((unsigned char)p->s[p->i]))
            p->i++;
        if (p->i < p->n && (p->s[p->i] == '"' || p->s[p->i] == '\'')) {
            char q = p->s[p->i++];
            vs = p->i;
            while (p->i < p->n && p->s[p->i] != q)
                p->i++;
            if (store)
                ht_decode(p->s + vs, p->i - vs, &p->abuf);
            if (p->i < p->n)
                p->i++;
        } else {
            vs = p->i;
            while (p->i < p->n && !ht_space((unsigned char)p->s[p->i])
                && p->s[p->i] != '>')
                p->i++;
            if (store)
                ht_decode(p->s + vs, p->i - vs, &p->abuf);
        }
        if (store)
            p->attr[p->nattr].vlen = p->abuf.n - p->attr[p->nattr].voff;
    }
    if (!store)
        return 1;
    if (ht_attr_dup(p, p->attr[p->nattr].n, p->attr[p->nattr].nlen)) {
        p->abuf.n = p->attr[p->nattr].voff;
        return 1;
    }
    p->nattr++;
    return 1;
}

static int ht_ieqn(const char* a, size_t an, const char* b, size_t bn)
{
    size_t i;
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (ht_lower((unsigned char)a[i]) != ht_lower((unsigned char)b[i]))
            return 0;
    return 1;
}

static JSValue ht_new_element(ht_t* p, JSValue* children)
{
    JSAtom atoms[3];
    JSValue vals[3];
    char small[64];
    char* lower = small;
    size_t ln = p->taglen, m;
    int k;

    if (ln > sizeof small) {
        lower = (char*)js_malloc(p->ctx, ln);
        if (!lower)
            return JS_EXCEPTION;
    }
    for (m = 0; m < ln; m++)
        lower[m] = (char)ht_lower((unsigned char)p->tag[m]);
    vals[1] = ht_new_attrs(p);
    if (JS_IsException(vals[1])) {
        if (lower != small)
            js_free(p->ctx, lower);
        return JS_EXCEPTION;
    }
    vals[0] = JS_NewStringLen(p->ctx, lower, ln);
    if (lower != small)
        js_free(p->ctx, lower);
    if (JS_IsException(vals[0])) {
        JS_FreeValue(p->ctx, vals[1]);
        return JS_EXCEPTION;
    }
    *children = JS_NewArray(p->ctx);
    if (JS_IsException(*children)) {
        JS_FreeValue(p->ctx, vals[0]);
        JS_FreeValue(p->ctx, vals[1]);
        return JS_EXCEPTION;
    }
    vals[2] = JS_DupValue(p->ctx, *children);
    for (k = 0; k < 3; k++)
        atoms[k] = JS_DupAtom(p->ctx, p->el_atoms[k]);
    vals[0] = JS_NewObjectFromProps(p->ctx, &p->el_shape, 3, atoms, vals);
    if (JS_IsException(vals[0])) {
        JS_FreeValue(p->ctx, *children);
        return JS_EXCEPTION;
    }
    return vals[0];
}

static int ht_push_text(ht_t* p)
{
    JSValue sv;

    if (p->text.n == 0)
        return 0;
    sv = JS_NewStringLen(p->ctx, (const char*)p->text.p, p->text.n);
    p->text.n = 0;
    if (JS_IsException(sv))
        return -1;
    return JS_DefinePropertyValueUint32(p->ctx, p->stack[p->depth],
               p->counts[p->depth]++, sv,
               JS_PROP_C_W_E)
            < 0
        ? -1
        : 0;
}

static void ht_close(ht_t* p, const char* name, size_t nlen)
{
    int d;

    for (d = p->depth; d > 0; d--)
        if (ht_ieqn(name, nlen, p->nptr[d], p->nlen[d]))
            break;
    if (d == 0)
        return;
    while (p->depth >= d) {
        JS_FreeValue(p->ctx, p->stack[p->depth]);
        p->depth--;
    }
}

static int ht_open(ht_t* p)
{
    JSValue kids, el;

    if (p->depth > 0 && (ht_ieq(p->nptr[p->depth], p->nlen[p->depth], "p") && HT_CLOSES(p->tag, p->taglen)))
        ht_close(p, "p", 1);
    if (p->depth > 0 && ht_ieq(p->nptr[p->depth], p->nlen[p->depth], "li")
        && ht_ieq(p->tag, p->taglen, "li"))
        ht_close(p, "li", 2);
    if (p->depth + 1 >= HT_MAX_DEPTH) {
        snprintf(p->err, sizeof p->err, "nesting exceeds %d elements", HT_MAX_DEPTH);
        return -1;
    }
    el = ht_new_element(p, &kids);
    if (JS_IsException(el))
        return -1;
    if (JS_DefinePropertyValueUint32(p->ctx, p->stack[p->depth],
            p->counts[p->depth]++, el,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(p->ctx, kids);
        return -1;
    }
    if (p->selfclose || HT_IS_VOID(p->tag, p->taglen)) {
        JS_FreeValue(p->ctx, kids);
        return 0;
    }
    p->depth++;
    p->stack[p->depth] = kids;
    p->nptr[p->depth] = p->tag;
    p->nlen[p->depth] = p->taglen;
    p->counts[p->depth] = 0;
    return 0;
}

static void ht_raw_text(ht_t* p, const char* tag, size_t tlen)
{
    size_t st = p->i;
    const size_t n = p->n;
    const char* s = p->s;

    if (n - p->i >= 256) {
        while (p->i < n) {
            size_t t = simd.find_first_of((const uint8_t*)s + p->i, n - p->i,
                (const uint8_t*)"<", 1);
            if (t == (size_t)-1) {
                p->i = n;
                break;
            }
            p->i += t;
            if (p->i + 1 < n && s[p->i + 1] == '/'
                && p->i + 2 + tlen <= n
                && ht_ieqn(s + p->i + 2, tlen, p->nptr[p->depth], p->nlen[p->depth])
                && ht_raw_end(s, n, p->i, tlen)) {
                break;
            }
            p->i++;
        }
    } else {
        while (p->i < n) {
            if (s[p->i] == '<' && p->i + 1 < n && s[p->i + 1] == '/'
                && p->i + 2 + tlen <= n
                && ht_ieqn(s + p->i + 2, tlen, p->nptr[p->depth], p->nlen[p->depth])
                && ht_raw_end(s, n, p->i, tlen)) {
                break;
            }
            p->i++;
        }
    }
    (void)tag;
    hb_write(&p->text, s + st, p->i - st);
}

static void ht_skip_bang(ht_t* p)
{
    if (p->n - p->i >= 4 && memcmp(p->s + p->i, "<!--", 4) == 0) {
        size_t j = p->i + 4;
        if (p->n - j >= 256) {
            while (j + 2 < p->n) {
                size_t t = simd.find_first_of((const uint8_t*)p->s + j, p->n - j,
                    (const uint8_t*)"-", 1);
                if (t == (size_t)-1) {
                    j = p->n;
                    break;
                }
                j += t;
                if (j + 2 < p->n && p->s[j] == '-' && p->s[j + 1] == '-' && p->s[j + 2] == '>')
                    break;
                j++;
            }
        } else {
            while (j + 2 < p->n && !(p->s[j] == '-' && p->s[j + 1] == '-' && p->s[j + 2] == '>'))
                j++;
        }
        p->i = j + 2 < p->n ? j + 3 : p->n;
        return;
    }
    if (p->n - p->i >= 256) {
        size_t t = simd.find_first_of((const uint8_t*)p->s + p->i, p->n - p->i,
            (const uint8_t*)">", 1);
        p->i = (t == (size_t)-1) ? p->n : p->i + t + 1;
        return;
    }
    while (p->i < p->n && p->s[p->i] != '>')
        p->i++;
    if (p->i < p->n)
        p->i++;
}

static int ht_step_close(ht_t* p)
{
    size_t j = p->i + 2, st = j;

    if (j < p->n && ht_tag_start((unsigned char)p->s[j]))
        while (j < p->n && ht_name_char((unsigned char)p->s[j]))
            j++;
    if (j == st) {
        hb_put(&p->text, '<');
        p->i++;
        return 0;
    }
    if (ht_push_text(p) < 0)
        return -1;
    ht_close(p, p->s + st, j - st);
    while (j < p->n && p->s[j] != '>')
        j++;
    p->i = j < p->n ? j + 1 : p->n;
    return 0;
}

static int ht_step_open(ht_t* p)
{
    size_t st = p->i + 1;
    int raw;
    const char* tag;
    size_t tlen;

    p->i = st;
    while (p->i < p->n && ht_name_char((unsigned char)p->s[p->i]))
        p->i++;
    p->tag = p->s + st;
    p->taglen = p->i - st;
    p->nattr = 0;
    p->abuf.n = 0;
    p->selfclose = 0;
    while (ht_scan_attr(p))
        ;
    if (p->i < p->n && p->s[p->i] == '>')
        p->i++;
    if (ht_push_text(p) < 0)
        return -1;
    raw = HT_IS_RAW(p->tag, p->taglen);
    tag = p->tag;
    tlen = p->taglen;
    if (ht_open(p) < 0)
        return -1;
    if (raw && !p->selfclose && p->depth > 0) {
        ht_raw_text(p, tag, tlen);
        if (ht_push_text(p) < 0)
            return -1;
    }
    return 0;
}

static int ht_step(ht_t* p)
{
    size_t st;

    if (p->s[p->i] != '<') {
        st = p->i;
        while (p->i < p->n && p->s[p->i] != '<')
            p->i++;
        ht_decode(p->s + st, p->i - st, &p->text);
        return 0;
    }
    if (p->i + 1 >= p->n) {
        hb_put(&p->text, '<');
        p->i++;
        return 0;
    }
    if (p->s[p->i + 1] == '!') {
        ht_skip_bang(p);
        return 0;
    }
    if (p->s[p->i + 1] == '/')
        return ht_step_close(p);
    if (!ht_tag_start((unsigned char)p->s[p->i + 1])) {
        hb_put(&p->text, '<');
        p->i++;
        return 0;
    }
    return ht_step_open(p);
}

static void ht_parse_release(ht_t* p)
{
    int k;
    for (k = 0; k < 3; k++)
        if (p->el_atoms[k] != JS_ATOM_NULL)
            JS_FreeAtom(p->ctx, p->el_atoms[k]);
    JS_FreeShapeCache(JS_GetRuntime(p->ctx), &p->el_shape);
    JS_FreeShapeCache(JS_GetRuntime(p->ctx), &p->at_shape);
    hb_free(&p->text);
    hb_free(&p->abuf);
}

JSValue ht_parse_into(JSContext* ctx, const char* s, size_t n)
{
    ht_t p;
    JSValue root;
    int rc = 0;

    memset(&p, 0, sizeof p);
    p.ctx = ctx;
    p.s = s;
    p.n = n;
    hb_init(&p.text);
    hb_init(&p.abuf);
    if (n > 4096) {
        size_t ec = n > HT_MAX_INPUT ? HT_MAX_INPUT : n;
        hb_reserve(&p.text, ec < 8192 ? ec : 8192);
        hb_reserve(&p.abuf, 4096);
    }
    p.el_atoms[0] = JS_NewAtom(ctx, "name");
    p.el_atoms[1] = JS_NewAtom(ctx, "attrs");
    p.el_atoms[2] = JS_NewAtom(ctx, "children");
    root = p.el_atoms[0] && p.el_atoms[1] && p.el_atoms[2] ? JS_NewArray(ctx) : JS_EXCEPTION;
    if (JS_IsException(root)) {
        ht_parse_release(&p);
        return root;
    }
    p.stack[0] = root;
    p.nptr[0] = "";
    p.nlen[0] = 0;
    while (p.i < p.n && rc == 0)
        rc = ht_step(&p);
    if (rc == 0)
        rc = ht_push_text(&p);
    while (p.depth > 0) {
        JS_FreeValue(ctx, p.stack[p.depth]);
        p.depth--;
    }
    ht_parse_release(&p);
    if (rc >= 0 && (p.text.oom || p.abuf.oom)) {
        JS_FreeValue(ctx, root);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (rc < 0) {
        JS_FreeValue(ctx, root);
        return p.err[0] ? JS_ThrowRangeError(ctx, "HTMLParse: %s", p.err)
                        : JS_EXCEPTION;
    }
    return root;
}

JSValue dyn_html_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    JSValue out;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "HTMLParse(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > HT_MAX_INPUT) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "HTMLParse: input exceeds %u bytes",
            HT_MAX_INPUT);
    }
    out = ht_parse_into(ctx, s, n);
    JS_FreeCString(ctx, s);
    return out;
}

#endif
