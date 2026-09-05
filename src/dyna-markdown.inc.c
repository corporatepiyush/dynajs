/* Markdown for dyna:html (design 07): CommonMark's core plus the GFM pieces
   people actually use. HTML output is ESCAPED BY DEFAULT and raw HTML in the
   source is escaped rather than passed through -- a renderer that emits its
   input verbatim is an XSS hole with a nice API. Full API: see the module header. */

#define MD_MAX_DEPTH 64
#define MD_MAX_INPUT (16u << 20)

typedef struct {
    JSContext  *ctx;
    const char *s;
    size_t      n;
    hb_t        out;
    int         allow_html;             /* opt-in, and it is never the default */
    int         depth;
} md_t;

/* ------------------------------------------------------------------ lines */

typedef struct { size_t start, end; int indent; } md_line_t;

static size_t md_line_at(const char *s, size_t n, size_t i, md_line_t *L)
{
    size_t j = i, k;

    while (j < n && s[j] != '\n') j++;
    L->start = i;
    L->end = j;
    if (L->end > L->start && s[L->end - 1] == '\r')
        L->end--;
    for (k = L->start; k < L->end && (s[k] == ' ' || s[k] == '\t'); k++)
        ;
    L->indent = (int)(k - L->start);
    L->start = k;
    return j < n ? j + 1 : n;
}

static int md_blank(const md_line_t *L) { return L->start >= L->end; }

/* ----------------------------------------------------------------- inline */

static int md_punct(unsigned char c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
        || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static void md_text(md_t *m, const char *s, size_t n);

/* A link destination and title: `](dest "title")`. Returns 0 if this is not a
   complete link, so the `[` stays literal text. */
static int md_link_tail(const char *s, size_t n, size_t *i, size_t *ds,
                        size_t *de, size_t *ts, size_t *te)
{
    size_t j = *i;

    if (j >= n || s[j] != '(')
        return 0;
    j++;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    *ds = j;
    if (j < n && s[j] == '<') {
        j++;
        *ds = j;
        while (j < n && s[j] != '>' && s[j] != '\n') j++;
        if (j >= n)
            return 0;
        *de = j;
        j++;
    } else {
        int par = 0;
        while (j < n && s[j] != '\n') {
            if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
            if (s[j] == '(') par++;
            else if (s[j] == ')') { if (!par) break; par--; }
            else if (s[j] == ' ' || s[j] == '\t') break;
            j++;
        }
        *de = j;
    }
    *ts = *te = j;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j < n && (s[j] == '"' || s[j] == '\'')) {
        char q = s[j++];
        *ts = j;
        while (j < n && s[j] != q) j++;
        if (j >= n)
            return 0;
        *te = j;
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    }
    if (j >= n || s[j] != ')')
        return 0;
    *i = j + 1;
    return 1;
}

/* A URL that is safe to put in an href. Anything with a scheme that is not
   http, https, mailto or a relative reference becomes empty -- `javascript:`
   in a markdown link is the same hole as in raw HTML.

   The part before the first ':' is a scheme only when it has the URL grammar
   shape (an ALPHA first, then alnum / + / - / .). A '.' no longer marks the
   value relative, so `a.b:x` is the unknown scheme `a.b` and is refused; a
   part holding anything a scheme cannot ('/', '?', '#', or any other
   non-scheme byte) is a relative path. The scan percent-decodes as it goes,
   so `java%73cript:` is checked as the scheme it really names, and a
   backslash before the ':' is refused outright (browsers read one as '/'),
   never mistaken for a path byte. */
static int md_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int md_scheme_char(unsigned char c, int first)
{
    if (first)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

static int md_url_ok(const char *s, size_t n)
{
    size_t i = 0, k = 0;
    char scheme[16];
    int first = 1;

    while (i < n && (unsigned char)s[i] <= 0x20) i++;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < n
            && md_hex(s[i + 1]) >= 0 && md_hex(s[i + 2]) >= 0) {
            c = (unsigned char)(md_hex(s[i + 1]) * 16 + md_hex(s[i + 2]));
            i += 3;
        } else {
            i++;
        }
        if (c == ':')
            break;
        if (c == '/' || c == '?' || c == '#')
            return 1;                   /* relative: no scheme to check */
        if (c == '\\')
            return 0;                   /* a backslash in scheme position */
        if (c <= 0x20)
            continue;                   /* browsers ignore control bytes */
        if (!md_scheme_char(c, first))
            return 1;                   /* cannot be a scheme: a relative path */
        first = 0;
        if (k + 1 < sizeof scheme)
            scheme[k++] = (char)ht_lower(c);
        /* else: too long for any real scheme name, so the truncated copy
           below matches nothing and the URL is refused */
    }
    if (i >= n || first)
        return 1;                       /* no colon at all, or nothing before one */
    scheme[k] = 0;
    return strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0
        || strcmp(scheme, "mailto") == 0 || strcmp(scheme, "ftp") == 0;
}

static void md_href(md_t *m, const char *s, size_t n)
{
    if (!md_url_ok(s, n))
        return;                         /* an unsafe scheme yields href="" */
    ht_escape(&m->out, s, n, 1);
}

/* A code span: the run of backticks that opens it also closes it. */
static int md_code_span(md_t *m, const char *s, size_t n, size_t *i)
{
    size_t open = *i, run = 0, j, st;

    while (open + run < n && s[open + run] == '`') run++;
    st = open + run;
    for (j = st; j + run <= n; j++) {
        size_t k = 0;
        while (j + k < n && s[j + k] == '`') k++;
        if (k == run) {
            hb_puts(&m->out, "<code>");
            ht_escape(&m->out, s + st, j - st, 0);
            hb_puts(&m->out, "</code>");
            *i = j + run;
            return 1;
        }
        if (k)
            j += k - 1;
    }
    return 0;
}

/* An emphasis run: `*`/`_` for em, doubled for strong, `~~` for strikethrough. */
static int md_emphasis(md_t *m, const char *s, size_t n, size_t *i)
{
    char c = s[*i];
    size_t run = 0, j, st;
    const char *open, *close;

    while (*i + run < n && s[*i + run] == c) run++;
    if (run > 3)
        return 0;
    if (c == '~' && run != 2)
        return 0;
    st = *i + run;
    if (st >= n || s[st] == ' ')
        return 0;                       /* an opener is not followed by space */
    for (j = st; j < n; j++) {
        size_t k = 0;
        if (s[j] == '\\') { j++; continue; }
        while (j + k < n && s[j + k] == c) k++;
        if (k >= run && j > st && s[j - 1] != ' ')
            break;
        if (k)
            j += k - 1;
    }
    if (j >= n)
        return 0;
    open = c == '~' ? "<del>" : run >= 2 ? "<strong>" : "<em>";
    close = c == '~' ? "</del>" : run >= 2 ? "</strong>" : "</em>";
    hb_puts(&m->out, open);
    md_text(m, s + st, j - st);
    hb_puts(&m->out, close);
    *i = j + run;
    return 1;
}

/* Inline markup, into escaped HTML. */
/* The bytes that can START an inline construct. The run scan tested eleven of
   them per ordinary character; one table load says the same thing. */
static const uint8_t MD_INL[256] = {
    ['`']=1, ['*']=1, ['_']=1, ['[']=1, ['!']=1, ['\\']=1,
    ['<']=1, ['&']=1, ['~']=1, ['>']=1, ['"']=1,
};
static const uint8_t MD_INL_SET[] = { '`','*','_','[','!','\\','<','&','~','>','"' };
#define MD_SIMD_MIN 64

static void md_text(md_t *m, const char *s, size_t n)
{
    size_t i = 0, run;

    if (m->depth >= MD_MAX_DEPTH) {
        ht_escape(&m->out, s, n, 0);
        return;
    }
    m->depth++;
    while (i < n) {
        char c;
        run = i;
        if (n - i >= MD_SIMD_MIN) {
            size_t t = simd.find_first_of((const uint8_t *)s + i, n - i,
                                          MD_INL_SET, sizeof MD_INL_SET);
            i = (t == (size_t)-1) ? n : i + t;
        } else {
            while (i < n && !MD_INL[(unsigned char)s[i]])
                i++;
        }
        ht_escape(&m->out, s + run, i - run, 0);
        if (i >= n)
            break;
        c = s[i];
        if (c == '\\' && i + 1 < n && md_punct((unsigned char)s[i + 1])) {
            ht_escape(&m->out, s + i + 1, 1, 0);
            i += 2;
            continue;
        }
        if (c == '`' && md_code_span(m, s, n, &i))
            continue;
        if ((c == '*' || c == '_' || c == '~') && md_emphasis(m, s, n, &i))
            continue;
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            int img = c == '!';
            size_t j = i + (img ? 2 : 1), depth = 1, ls = j, le, ds, de, ts, te;
            while (j < n && depth) {
                if (s[j] == '\\') { j += 2; continue; }
                if (s[j] == '[') depth++;
                else if (s[j] == ']') depth--;
                if (depth) j++;
            }
            le = j;
            if (j < n) j++;
            if (j < n && md_link_tail(s, n, &j, &ds, &de, &ts, &te)) {
                if (img) {
                    hb_puts(&m->out, "<img src=\"");
                    md_href(m, s + ds, de - ds);
                    hb_puts(&m->out, "\" alt=\"");
                    ht_escape(&m->out, s + ls, le - ls, 1);
                    if (te > ts) {
                        hb_puts(&m->out, "\" title=\"");
                        ht_escape(&m->out, s + ts, te - ts, 1);
                    }
                    hb_puts(&m->out, "\">");
                } else {
                    hb_puts(&m->out, "<a href=\"");
                    md_href(m, s + ds, de - ds);
                    if (te > ts) {
                        hb_puts(&m->out, "\" title=\"");
                        ht_escape(&m->out, s + ts, te - ts, 1);
                    }
                    hb_puts(&m->out, "\">");
                    md_text(m, s + ls, le - ls);
                    hb_puts(&m->out, "</a>");
                }
                i = j;
                continue;
            }
        }
        if (c == '<') {
            /* An autolink is a URL in angle brackets; anything else is text,
               because raw HTML is escaped unless the caller opted in. */
            size_t j = i + 1;
            while (j < n && s[j] != '>' && s[j] != ' ' && s[j] != '\n') j++;
            if (j < n && s[j] == '>' && j > i + 1
                && md_url_ok(s + i + 1, j - i - 1)
                && memchr(s + i + 1, ':', j - i - 1)) {
                hb_puts(&m->out, "<a href=\"");
                ht_escape(&m->out, s + i + 1, j - i - 1, 1);
                hb_puts(&m->out, "\">");
                ht_escape(&m->out, s + i + 1, j - i - 1, 0);
                hb_puts(&m->out, "</a>");
                i = j + 1;
                continue;
            }
            if (m->allow_html) {
                hb_put(&m->out, '<');
                i++;
                continue;
            }
        }
        if (c == '>' && m->allow_html) {
            hb_put(&m->out, '>');       /* the other half of the opt-in */
            i++;
            continue;
        }
        ht_escape(&m->out, s + i, 1, 0);
        i++;
    }
    m->depth--;
}

/* ------------------------------------------------------------------ blocks */

static int md_fence(const md_line_t *L, const char *s, char *ch, size_t *len)
{
    size_t i = L->start, run = 0;
    char c;

    if (i >= L->end || (s[i] != '`' && s[i] != '~'))
        return 0;
    c = s[i];
    while (i + run < L->end && s[i + run] == c) run++;
    if (run < 3)
        return 0;
    *ch = c;
    *len = run;
    return 1;
}

static int md_thematic(const md_line_t *L, const char *s)
{
    size_t i;
    int count = 0;
    char c = 0;

    for (i = L->start; i < L->end; i++) {
        if (s[i] == ' ' || s[i] == '\t')
            continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_')
            return 0;
        if (c && s[i] != c)
            return 0;
        c = s[i];
        count++;
    }
    return count >= 3;
}

/* `- `, `* `, `+ ` or `1. `. Returns the content offset, or 0. */
static size_t md_list_marker(const md_line_t *L, const char *s, int *ordered)
{
    size_t i = L->start;

    if (i < L->end && (s[i] == '-' || s[i] == '*' || s[i] == '+')
        && i + 1 < L->end && (s[i + 1] == ' ' || s[i + 1] == '\t')) {
        *ordered = 0;
        return i + 2;
    }
    while (i < L->end && s[i] >= '0' && s[i] <= '9') i++;
    if (i > L->start && i < L->end && (s[i] == '.' || s[i] == ')')
        && i + 1 < L->end && s[i + 1] == ' ') {
        *ordered = 1;
        return i + 2;
    }
    return 0;
}

static int md_setext(const md_line_t *L, const char *s)
{
    size_t i;
    char c;

    if (L->start >= L->end)
        return 0;
    c = s[L->start];
    if (c != '=' && c != '-')
        return 0;
    for (i = L->start; i < L->end; i++)
        if (s[i] != c)
            return 0;
    return c == '=' ? 1 : 2;
}

static void md_blocks(md_t *m, size_t from, size_t to);

/* A table needs a delimiter row; anything else is a paragraph. */
static int md_table_delim(const md_line_t *L, const char *s)
{
    size_t i;
    int cells = 0, dash = 0;

    for (i = L->start; i < L->end; i++) {
        char c = s[i];
        if (c == '|') { if (dash) cells++; dash = 0; continue; }
        if (c == '-') { dash = 1; continue; }
        if (c == ':' || c == ' ' || c == '\t') continue;
        return 0;
    }
    if (dash) cells++;
    return cells >= 1;
}

static void md_table_row(md_t *m, const char *s, const md_line_t *L,
                         const char *cell)
{
    size_t i = L->start, st;

    hb_puts(&m->out, "<tr>");
    if (i < L->end && s[i] == '|') i++;
    while (i <= L->end) {
        st = i;
        while (i < L->end && s[i] != '|') i++;
        {
            size_t a = st, b = i;
            while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
            if (b > a || st < L->end) {
                hb_put(&m->out, '<');
                hb_puts(&m->out, cell);
                hb_put(&m->out, '>');
                md_text(m, s + a, b - a);
                hb_puts(&m->out, "</");
                hb_puts(&m->out, cell);
                hb_put(&m->out, '>');
            }
        }
        if (i >= L->end)
            break;
        i++;
        if (i >= L->end)
            break;                      /* a trailing pipe closes the row */
    }
    hb_puts(&m->out, "</tr>");
}

/* One block, from line `from`; returns the offset after it. */
static size_t md_one_block(md_t *m, size_t from, size_t to)
{
    md_line_t L;
    size_t next = md_line_at(m->s, to, from, &L), i;
    char fc;
    size_t flen;
    int ordered;

    if (md_blank(&L))
        return next;
    if (md_thematic(&L, m->s)) {
        hb_puts(&m->out, "<hr>\n");
        return next;
    }
    if (m->s[L.start] == '#') {
        size_t h = L.start, lvl = 0, ts;
        while (h < L.end && m->s[h] == '#' && lvl < 7) { h++; lvl++; }
        if (lvl <= 6 && h < L.end && m->s[h] == ' ') {
            char tag[8];
            size_t te = L.end;
            ts = h + 1;
            while (te > ts && (m->s[te - 1] == ' ' || m->s[te - 1] == '#')) te--;
            snprintf(tag, sizeof tag, "h%u", (unsigned)lvl);
            hb_put(&m->out, '<'); hb_puts(&m->out, tag); hb_put(&m->out, '>');
            md_text(m, m->s + ts, te - ts);
            hb_puts(&m->out, "</"); hb_puts(&m->out, tag);
            hb_puts(&m->out, ">\n");
            return next;
        }
    }
    if (md_fence(&L, m->s, &fc, &flen)) {
        size_t info_s = L.start + flen, info_e = L.end, k = next;
        while (info_s < info_e && m->s[info_s] == ' ') info_s++;
        hb_puts(&m->out, "<pre><code");
        if (info_e > info_s) {
            size_t ie = info_s;
            while (ie < info_e && m->s[ie] != ' ') ie++;
            hb_puts(&m->out, " class=\"language-");
            ht_escape(&m->out, m->s + info_s, ie - info_s, 1);
            hb_put(&m->out, '"');
        }
        hb_put(&m->out, '>');
        while (k < to) {
            md_line_t C;
            size_t after = md_line_at(m->s, to, k, &C);
            char cc;
            size_t clen;
            if (md_fence(&C, m->s, &cc, &clen) && cc == fc && clen >= flen) {
                k = after;
                break;
            }
            /* The fence's own indent is stripped from every content line. */
            {
                size_t st = C.start;
                int drop = L.indent < C.indent ? L.indent : C.indent;
                st = C.start - (C.indent - drop);
                ht_escape(&m->out, m->s + st, C.end - st, 0);
            }
            hb_put(&m->out, '\n');
            k = after;
        }
        hb_puts(&m->out, "</code></pre>\n");
        return k;
    }
    if (L.indent >= 4) {                /* an indented code block */
        size_t k = from;
        hb_puts(&m->out, "<pre><code>");
        while (k < to) {
            md_line_t C;
            size_t after = md_line_at(m->s, to, k, &C);
            if (!md_blank(&C) && C.indent < 4)
                break;
            if (!md_blank(&C))
                ht_escape(&m->out, m->s + C.start - (C.indent - 4), C.end - C.start + (C.indent - 4), 0);
            hb_put(&m->out, '\n');
            k = after;
        }
        hb_puts(&m->out, "</code></pre>\n");
        return k;
    }
    if (m->s[L.start] == '>') {
        size_t k = from;
        hb_t save = m->out;
        hb_t inner;
        hb_init(&inner);
        m->out = inner;
        while (k < to) {
            md_line_t C;
            size_t after = md_line_at(m->s, to, k, &C);
            size_t cs;
            if (md_blank(&C) || m->s[C.start] != '>')
                break;
            cs = C.start + 1;
            if (cs < C.end && m->s[cs] == ' ') cs++;
            hb_write(&m->out, m->s + cs, C.end - cs);
            hb_put(&m->out, '\n');
            k = after;
        }
        inner = m->out;
        m->out = save;
        hb_puts(&m->out, "<blockquote>\n");
        if (m->depth < MD_MAX_DEPTH) {
            md_t sub = *m;
            sub.s = (const char *)inner.p;
            sub.n = inner.n;
            sub.out = m->out;
            sub.depth = m->depth + 1;
            md_blocks(&sub, 0, inner.n);
            m->out = sub.out;
        }
        hb_free(&inner);
        hb_puts(&m->out, "</blockquote>\n");
        return k;
    }
    if ((i = md_list_marker(&L, m->s, &ordered)) != 0) {
        size_t k = from;
        hb_puts(&m->out, ordered ? "<ol>\n" : "<ul>\n");
        while (k < to) {
            md_line_t C;
            size_t after = md_line_at(m->s, to, k, &C), cs;
            int ord2;
            if (md_blank(&C)) { k = after; continue; }
            cs = md_list_marker(&C, m->s, &ord2);
            if (!cs || ord2 != ordered)
                break;
            hb_puts(&m->out, "<li>");
            md_text(m, m->s + cs, C.end - cs);
            /* A continuation line is indented under the marker. */
            while (after < to) {
                md_line_t D;
                size_t a2 = md_line_at(m->s, to, after, &D);
                int od;
                if (md_blank(&D) || D.indent == 0 || md_list_marker(&D, m->s, &od))
                    break;
                hb_put(&m->out, '\n');
                md_text(m, m->s + D.start, D.end - D.start);
                after = a2;
            }
            hb_puts(&m->out, "</li>\n");
            k = after;
        }
        hb_puts(&m->out, ordered ? "</ol>\n" : "</ul>\n");
        return k;
    }
    /* A table needs its delimiter row on the second line. */
    if (memchr(m->s + L.start, '|', L.end - L.start) && next < to) {
        md_line_t D;
        size_t after = md_line_at(m->s, to, next, &D);
        if (md_table_delim(&D, m->s)) {
            size_t k = after;
            hb_puts(&m->out, "<table>\n<thead>");
            md_table_row(m, m->s, &L, "th");
            hb_puts(&m->out, "</thead>\n<tbody>");
            while (k < to) {
                md_line_t R;
                size_t a2 = md_line_at(m->s, to, k, &R);
                if (md_blank(&R) || !memchr(m->s + R.start, '|', R.end - R.start))
                    break;
                md_table_row(m, m->s, &R, "td");
                k = a2;
            }
            hb_puts(&m->out, "</tbody>\n</table>\n");
            return k;
        }
    }
    {   /* a paragraph, up to a blank line or a block start */
        size_t k = from, first = 1;
        int heading = 0;
        hb_t para;
        hb_init(&para);
        while (k < to) {
            md_line_t C;
            size_t after = md_line_at(m->s, to, k, &C);
            int st;
            if (md_blank(&C)) { k = after; break; }
            st = md_setext(&C, m->s);
            if (st && !first) { heading = st; k = after; break; }
            if (!first && (md_thematic(&C, m->s) || m->s[C.start] == '>'
                           || m->s[C.start] == '#'))
                break;
            if (!first)
                hb_put(&para, '\n');
            hb_write(&para, m->s + C.start, C.end - C.start);
            first = 0;
            k = after;
        }
        if (heading) {
            hb_puts(&m->out, heading == 1 ? "<h1>" : "<h2>");
            md_text(m, (const char *)para.p, para.n);
            hb_puts(&m->out, heading == 1 ? "</h1>\n" : "</h2>\n");
        } else if (para.n) {
            hb_puts(&m->out, "<p>");
            md_text(m, (const char *)para.p, para.n);
            hb_puts(&m->out, "</p>\n");
        }
        hb_free(&para);
        return k;
    }
}

static void md_blocks(md_t *m, size_t from, size_t to)
{
    size_t i = from;
    while (i < to) {
        size_t next = md_one_block(m, i, to);
        if (next <= i)
            break;                      /* no progress is a bug, not a loop */
        i = next;
    }
}

static JSValue dyn_md_render(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    md_t m;
    const char *s;
    size_t n;
    JSValue out;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "MarkdownToHTML(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > MD_MAX_INPUT) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "MarkdownToHTML: input exceeds %u bytes",
                                  MD_MAX_INPUT);
    }
    memset(&m, 0, sizeof m);
    m.ctx = ctx;
    m.s = s;
    m.n = n;
    hb_init(&m.out);
    if (n > 4096) {
        size_t est = n + (n >> 2);
        if (est > MD_MAX_INPUT) est = MD_MAX_INPUT;
        hb_reserve(&m.out, est);
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "allowRawHTML");
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, s);
            hb_free(&m.out);
            return JS_EXCEPTION;
        }
        m.allow_html = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    md_blocks(&m, 0, n);
    JS_FreeCString(ctx, s);
    if (m.out.oom) {
        hb_free(&m.out);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, (const char *)m.out.p, m.out.n);
    hb_free(&m.out);
    return out;
}
