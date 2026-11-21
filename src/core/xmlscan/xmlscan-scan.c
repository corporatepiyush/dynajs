#include "xmlscan.h"

#define XML_MAX_TOKEN (16u << 20)
#define XML_BULK_ON 32
#define XML_BULK_OFF 16

static void xb_putc(xb_t* b, uint32_t cp)
{
    uint8_t t[4];
    xb_write(b, t, (size_t)dyn_utf8_encode(cp, t));
}

void xml_scan_bom(xml_scan_t* s)
{
    if (s->bom_done || s->pos != 0)
        return;
    if (s->buf.n >= 3) {
        if (memcmp(s->buf.p, "\xEF\xBB\xBF", 3) == 0)
            s->pos = 3;
        s->bom_done = 1;
    } else if (s->final) {
        s->bom_done = 1;
    }
}

static int xml_prefix(const char* p, size_t avail, const char* lit)
{
    size_t l = strlen(lit), m = avail < l ? avail : l;
    return memcmp(p, lit, m) == 0;
}

static int xml_fail(xml_scan_t* s, const char* what)
{
    if (!s->err[0])
        snprintf(s->err, sizeof s->err, "%s at byte %u", what, (unsigned)s->pos);
    return XS_ERR;
}

static int xml_char_ok(uint32_t cp)
{
    return cp == 0x9 || cp == 0xA || cp == 0xD
        || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD)
        || (cp >= 0x10000 && cp <= 0x10FFFF);
}

static int xml_charref(xml_scan_t* s, const char* p, size_t n, size_t* i,
    uint32_t* out)
{
    size_t j = *i + 2, k;
    uint32_t cp = 0;
    int hex = (j < n && (p[j] == 'x' || p[j] == 'X')), any = 0;

    for (k = hex ? j + 1 : j; k < n && p[k] != ';'; k++) {
        int d;
        if (p[k] >= '0' && p[k] <= '9')
            d = p[k] - '0';
        else if (hex && p[k] >= 'a' && p[k] <= 'f')
            d = p[k] - 'a' + 10;
        else if (hex && p[k] >= 'A' && p[k] <= 'F')
            d = p[k] - 'A' + 10;
        else
            return xml_fail(s, "bad character reference");
        cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
        any = 1;
        if (cp > 0x10FFFF)
            return xml_fail(s, "character reference out of range");
    }
    if (k >= n)
        return XS_NEED;
    if (!any)
        return xml_fail(s, "empty character reference");
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return xml_fail(s, "character reference is a surrogate");
    if (cp == 0)
        return xml_fail(s, "character reference is NUL");
    if (!xml_char_ok(cp))
        return xml_fail(s, "character reference is not a legal XML character");
    *i = k + 1;
    *out = cp;
    return XS_EVENT;
}

static char xml_predefined(const char* e, size_t len)
{
    if (len == 2 && memcmp(e, "lt", 2) == 0)
        return '<';
    if (len == 2 && memcmp(e, "gt", 2) == 0)
        return '>';
    if (len == 3 && memcmp(e, "amp", 3) == 0)
        return '&';
    if (len == 4 && memcmp(e, "apos", 4) == 0)
        return '\'';
    if (len == 4 && memcmp(e, "quot", 4) == 0)
        return '"';
    return 0;
}

static int xml_entity(xml_scan_t* s, const char* p, size_t n, size_t* i, xb_t* out)
{
    size_t j = *i + 1, k;
    char c;

    if (j < n && p[j] == '#') {
        uint32_t cp;
        int r = xml_charref(s, p, n, i, &cp);
        if (r != XS_EVENT)
            return r;
        xb_putc(out, cp);
        return XS_EVENT;
    }
    for (k = j; k < n && k < j + 8 && p[k] != ';'; k++)
        ;
    if (k >= n || k >= j + 8) {
        if (k >= n && !s->final)
            return XS_NEED;
        if (s->keep_entities) {
            xb_put(out, '&');
            *i = *i + 1;
            return XS_EVENT;
        }
        return xml_fail(s, "unterminated entity");
    }
    c = xml_predefined(p + j, k - j);
    if (c) {
        xb_put(out, (uint8_t)c);
        *i = k + 1;
        return XS_EVENT;
    }
    if (s->keep_entities) {
        xb_write(out, p + *i, k + 1 - *i);
        *i = k + 1;
        return XS_EVENT;
    }
    return xml_fail(s, "unknown entity (no DTD is processed, so none can exist)");
}

static int xml_attr_value(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xb_t* out)
{
    char quote = p[*i];
    size_t j = *i + 1;
    if (quote != '"' && quote != '\'')
        return xml_fail(s, "an attribute value must be quoted");
    while (j < n) {
        if (p[j] == quote) {
            *i = j + 1;
            return XS_EVENT;
        }
        if (p[j] == '<')
            return xml_fail(s, "< in an attribute value");
        if (p[j] == '&') {
            int r = xml_entity(s, p, n, &j, out);
            if (r != XS_EVENT)
                return r;
            continue;
        }
        {
            size_t run = j;
            while (j < n && p[j] != quote && p[j] != '<' && p[j] != '&')
                j++;
            if (j > run && !xml_ok_chars(p + run, j - run))
                return xml_fail(s, "illegal XML character in an attribute value");
            if (j > run) {
                size_t k = run;
                int need_space = 0;
                while (k < j && !xml_space((unsigned char)p[k]))
                    k++;
                if (k == j) {
                    xb_write(out, p + run, j - run);
                } else {
                    for (k = run; k < j; k++)
                        xb_put(out, (uint8_t)(xml_space((unsigned char)p[k]) ? ' ' : p[k]));
                }
                (void)need_space;
                if (out->n > XML_MAX_TOKEN)
                    return xml_fail(s, "attribute value exceeds the token limit");
                continue;
            }
        }
        xb_put(out, (uint8_t)(xml_space((unsigned char)p[j]) ? ' ' : p[j]));
        j++;
        if (out->n > XML_MAX_TOKEN)
            return xml_fail(s, "attribute value exceeds the token limit");
    }
    return XS_NEED;
}

static int xml_tag_need(xml_scan_t* s, size_t n)
{
    if (n - s->pos > XML_MAX_TOKEN)
        return xml_fail(s, "open tag exceeds the token limit");
    return XS_NEED;
}

static int xml_scan_attr(xml_scan_t* s, const char* p, size_t n, size_t* j,
    xml_ev_t* ev)
{
    size_t k = *j, an, av;
    int r;

    while (k < n && xml_space((unsigned char)p[k]))
        k++;
    if (k >= n)
        return xml_tag_need(s, n);
    if (p[k] == '>' || p[k] == '/') {
        *j = k;
        return XS_END;
    }
    if (!xml_name_start((unsigned char)p[k]))
        return xml_fail(s, "expected an attribute name");
    if (ev->nattr >= XML_MAX_ATTRS)
        return xml_fail(s, "too many attributes on one element");
    an = k;
    while (k < n && xml_name_char((unsigned char)p[k]))
        k++;
    if (k >= n)
        return xml_tag_need(s, n);
    if (k - an > XML_MAX_TOKEN)
        return xml_fail(s, "attribute name exceeds the token limit");
    ev->attr[ev->nattr].n = p + an;
    ev->attr[ev->nattr].nlen = k - an;
    {
        uint32_t a;
        for (a = 0; a < ev->nattr; a++)
            if (ev->attr[a].nlen == k - an
                && memcmp(ev->attr[a].n, p + an, k - an) == 0)
                return xml_fail(s, "duplicate attribute name");
    }
    while (k < n && xml_space((unsigned char)p[k]))
        k++;
    if (k >= n)
        return xml_tag_need(s, n);
    if (p[k] != '=')
        return xml_fail(s, "an attribute needs a value");
    k++;
    while (k < n && xml_space((unsigned char)p[k]))
        k++;
    if (k >= n)
        return xml_tag_need(s, n);
    av = ev->abuf.n;
    r = xml_attr_value(s, p, n, &k, &ev->abuf);
    if (r != XS_EVENT)
        return r;
    ev->attr[ev->nattr].voff = av;
    ev->attr[ev->nattr].vlen = ev->abuf.n - av;
    ev->nattr++;
    *j = k;
    return XS_EVENT;
}

int xml_scan_open(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev)
{
    size_t j = *i, st;

    if (!xml_name_start((unsigned char)p[j]))
        return xml_fail(s, "expected an element name");
    st = j;
    while (j < n && xml_name_char((unsigned char)p[j]))
        j++;
    if (j >= n)
        return xml_tag_need(s, n);
    if (j - st > XML_MAX_TOKEN)
        return xml_fail(s, "element name exceeds the token limit");
    ev->name = p + st;
    ev->nlen = j - st;
    ev->nattr = 0;
    ev->selfclose = 0;
    ev->abuf.n = 0;
    for (;;) {
        int r = xml_scan_attr(s, p, n, &j, ev);
        if (r == XS_EVENT)
            continue;
        if (r != XS_END)
            return r;
        break;
    }
    if (p[j] == '>') {
        *i = j + 1;
        s->seen_root = 1;
        return XS_EVENT;
    }
    if (j + 1 >= n)
        return xml_tag_need(s, n);
    if (p[j + 1] != '>')
        return xml_fail(s, "expected /> to close an empty element");
    ev->selfclose = 1;
    *i = j + 2;
    s->seen_root = 1;
    return XS_EVENT;
}

static const uint8_t XML_TXT_SET[3] = { '<', '&', ']' };

int xml_scan_text(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev)
{
    size_t k = *i + s->tscan;
    size_t clean = 0;
    int bulk = 0;

    ev->kind = XEV_TEXT;
    s->tok = 1;
    if (ev->text.cap == 0 && n - k > 4096) {
        xb_reserve(&ev->text, 8192);
    }
    while (k < n && p[k] != '<') {
        if (bulk) {
            size_t run = k;
            size_t t = simd.find_first_of((const uint8_t*)p + k, n - k,
                XML_TXT_SET, sizeof XML_TXT_SET);
            k = (t == (size_t)-1) ? n : k + t;
            if (k > run) {
                if (ev->text.n + (k - run) > XML_MAX_TOKEN)
                    return xml_fail(s, "text run exceeds the token limit");
                if (!xml_ok_chars(p + run, k - run))
                    return xml_fail(s, "illegal XML character");
                xb_write(&ev->text, p + run, k - run);
            }
            if (k - run < XML_BULK_OFF)
                bulk = 0;
            clean = 0;
            continue;
        }
        if (p[k] == '&') {
            clean = 0;
            size_t at = k;
            int r = xml_entity(s, p, n, &k, &ev->text);
            if (r == XS_NEED) {
                s->tscan = at - *i;
                return XS_NEED;
            }
            if (r != XS_EVENT)
                return r;
            continue;
        }
        if (p[k] == ']') {
            if (k + 2 < n) {
                if (p[k + 1] == ']' && p[k + 2] == '>')
                    return xml_fail(s, "]]> in text must be escaped");
            } else if (!s->final) {
                s->tscan = k - *i;
                return XS_NEED;
            }
        }
        if (!xml_ok_chars(p + k, 1))
            return xml_fail(s, "illegal XML character");
        xb_put(&ev->text, (uint8_t)p[k]);
        k++;
        if (++clean >= XML_BULK_ON)
            bulk = 1;
        if (ev->text.n > XML_MAX_TOKEN)
            return xml_fail(s, "text run exceeds the token limit");
    }
    if (k >= n && !s->final) {
        s->tscan = k - *i;
        return XS_NEED;
    }
    *i = k;
    s->tok = 0;
    s->tscan = 0;
    return ev->text.oom ? xml_fail(s, "out of memory") : XS_EVENT;
}

int xml_scan_close(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev)
{
    size_t j = *i + 2, st;

    if (j >= n)
        return XS_NEED;
    if (!xml_name_start((unsigned char)p[j]))
        return xml_fail(s, "expected a close-tag name");
    st = j;
    while (j < n && xml_name_char((unsigned char)p[j]))
        j++;
    while (j < n && xml_space((unsigned char)p[j]))
        j++;
    if (j >= n) {
        if (n - *i > XML_MAX_TOKEN)
            return xml_fail(s, "close tag exceeds the token limit");
        return XS_NEED;
    }
    if (p[j] != '>')
        return xml_fail(s, "expected > to close a close tag");
    ev->kind = XEV_CLOSE;
    ev->name = p + st;
    ev->nlen = j - st;
    while (ev->nlen && xml_space((unsigned char)ev->name[ev->nlen - 1]))
        ev->nlen--;
    if (ev->nlen > XML_MAX_TOKEN)
        return xml_fail(s, "close tag exceeds the token limit");
    *i = j + 1;
    return XS_EVENT;
}

static int xml_scan_until(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev, size_t open_len, const char* term,
    int kind, int tok)
{
    size_t st = *i + open_len, j = st + s->tscan;

    s->tok = tok;
    while (j + 2 < n && !(p[j] == term[0] && p[j + 1] == term[1] && p[j + 2] == term[2]))
        j++;
    if (j + 2 >= n) {
        if (n - st > XML_MAX_TOKEN)
            return xml_fail(s, kind == XEV_CDATA ? "CDATA section exceeds the token limit" : "comment exceeds the token limit");
        if (s->final)
            return xml_fail(s, kind == XEV_CDATA ? "unterminated CDATA section" : "unterminated comment");
        s->tscan = j - st;
        return XS_NEED;
    }
    if (j - st > XML_MAX_TOKEN)
        return xml_fail(s, kind == XEV_CDATA ? "CDATA section exceeds the token limit" : "comment exceeds the token limit");
    if (kind == XEV_COMMENT) {
        size_t c;
        for (c = st; c + 1 < j; c++)
            if (p[c] == '-' && p[c + 1] == '-')
                return xml_fail(s, "-- in a comment");
        if (j > st && p[j - 1] == '-')
            return xml_fail(s, "-- in a comment");
    }
    ev->kind = (uint8_t)kind;
    ev->text.n = 0;
    xb_write(&ev->text, p + st, j - st);
    *i = j + 3;
    s->tok = 0;
    s->tscan = 0;
    return XS_EVENT;
}

int xml_scan_bang(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev)
{
    size_t j;

    if (n - *i >= 4 && memcmp(p + *i, "<!--", 4) == 0)
        return xml_scan_until(s, p, n, i, ev, 4, "-->", XEV_COMMENT, 2);
    if (n - *i >= 9 && memcmp(p + *i, "<![CDATA[", 9) == 0)
        return xml_scan_until(s, p, n, i, ev, 9, "]]>", XEV_CDATA, 3);
    if (n - *i >= 9 && memcmp(p + *i, "<!DOCTYPE", 9) == 0) {
        int depth = 0, q = 0, in_comment = 0;
        for (j = *i + 9; j < n; j++) {
            if (in_comment) {
                if (p[j] == '-' && n - j >= 3
                    && p[j + 1] == '-' && p[j + 2] == '>') {
                    j += 2;
                    in_comment = 0;
                }
            } else if (q) {
                if (p[j] == q)
                    q = 0;
            } else if (p[j] == '\'' || p[j] == '"') {
                q = p[j];
            } else if (p[j] == '[') {
                depth++;
            } else if (p[j] == ']') {
                depth--;
            } else if (p[j] == '>' && depth <= 0) {
                break;
            } else if (p[j] == '<' && n - j >= 4
                && memcmp(p + j, "<!--", 4) == 0) {
                in_comment = 1;
                j += 3;
            }
        }
        if (j >= n) {
            if (n - *i > XML_MAX_TOKEN)
                return xml_fail(s, "DOCTYPE exceeds the token limit");
            if (s->final)
                return xml_fail(s, "unterminated DOCTYPE");
            return XS_NEED;
        }
        if (s->seen_doctype || s->seen_root)
            return xml_fail(s, "DOCTYPE must appear once before the root element");
        s->seen_doctype = 1;
        *i = j + 1;
        ev->kind = XEV_SKIP;
        return XS_EVENT;
    }
    if (n - *i < 9 && !s->final
        && (xml_prefix(p + *i, n - *i, "<!--")
            || xml_prefix(p + *i, n - *i, "<![CDATA[")
            || xml_prefix(p + *i, n - *i, "<!DOCTYPE")))
        return XS_NEED;
    return xml_fail(s, "unsupported <! declaration");
}

int xml_scan_pi(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev)
{
    size_t j = *i + 2, st = j, te;

    if (j >= n)
        return XS_NEED;
    if (!xml_name_start((unsigned char)p[j]))
        return xml_fail(s, "expected a processing-instruction target");
    while (j < n && xml_name_char((unsigned char)p[j]))
        j++;
    te = j;
    while (j + 1 < n && !(p[j] == '?' && p[j + 1] == '>'))
        j++;
    if (j - st > XML_MAX_TOKEN)
        return xml_fail(s, "processing instruction exceeds the token limit");
    if (j + 1 >= n)
        return XS_NEED;
    ev->kind = XEV_PI;
    ev->name = p + st;
    ev->nlen = te - st;
    ev->val = p + te;
    ev->vlen = j - te;
    while (ev->vlen && xml_space((unsigned char)ev->val[0])) {
        ev->val++;
        ev->vlen--;
    }
    *i = j + 2;
    return XS_EVENT;
}
