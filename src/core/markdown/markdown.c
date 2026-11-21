#include "markdown.h"

#define MD_MAX_DEPTH 64

typedef struct {
    const char* s;
    size_t n;
    hb_t out;
    int allow_html;
    int heading_ids;
    int depth;
    hb_t slugs;
    uint32_t* slug_tab;
    uint32_t slug_cap, slug_n;
    uint64_t work;
    uint64_t work_max;
    int exhausted;
} md_t;

typedef struct {
    size_t start, end;
    int indent;
} md_line_t;

static void md_text(md_t* m, const char* s, size_t n);

static void md_blocks(md_t* m, size_t from, size_t to);

#define MD_SIMD_MIN 64

static int md_charge(md_t* m, size_t k)
{
    m->work += k;
    if (m->work > m->work_max) {
        m->exhausted = 1;
        return -1;
    }
    return 0;
}

static size_t md_line_at(const char* s, size_t n, size_t i, md_line_t* L)
{
    size_t j, k;
    const char* nl = i < n ? (const char*)memchr(s + i, '\n', n - i) : NULL;

    j = nl ? (size_t)(nl - s) : (i < n ? n : i);
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

static int md_blank(const md_line_t* L) { return L->start >= L->end; }

static int md_punct(unsigned char c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
        || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static int md_link_tail_scan(const char* s, size_t n, size_t* i,
    size_t* ds, size_t* de, size_t* ts, size_t* te, size_t* scanned)
{
    size_t j = *i;

    j++;
    while (j < n && (s[j] == ' ' || s[j] == '\t'))
        j++;
    *ds = j;
    if (j < n && s[j] == '<') {
        j++;
        *ds = j;
        while (j < n && s[j] != '>' && s[j] != '\n')
            j++;
        if (j >= n) {
            *scanned = j;
            return 0;
        }
        *de = j;
        j++;
    } else {
        int par = 0;
        while (j < n && s[j] != '\n') {
            if (s[j] == '\\' && j + 1 < n) {
                j += 2;
                continue;
            }
            if (s[j] == '(')
                par++;
            else if (s[j] == ')') {
                if (!par)
                    break;
                par--;
            } else if (s[j] == ' ' || s[j] == '\t')
                break;
            j++;
        }
        *de = j;
    }
    *ts = *te = j;
    while (j < n && (s[j] == ' ' || s[j] == '\t'))
        j++;
    if (j < n && (s[j] == '"' || s[j] == '\'')) {
        char q = s[j++];
        *ts = j;
        while (j < n && s[j] != q)
            j++;
        if (j >= n) {
            *scanned = j;
            return 0;
        }
        *te = j;
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t'))
            j++;
    }
    *scanned = j;
    if (j >= n || s[j] != ')')
        return 0;
    *i = j + 1;
    return 1;
}

static int md_link_tail(md_t* m, const char* s, size_t n, size_t* i,
    size_t* ds, size_t* de, size_t* ts, size_t* te)
{
    size_t start = *i, scanned = *i;
    int found;

    if (start >= n || s[start] != '(')
        return 0;
    found = md_link_tail_scan(s, n, i, ds, de, ts, te, &scanned);
    if (md_charge(m, scanned - start + 1) < 0)
        return 0;
    return found;
}

static int md_hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int md_scheme_char(unsigned char c, int first)
{
    if (first)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

static int md_url_ok(const char* s, size_t n)
{
    size_t i = 0, k = 0;
    char scheme[16];
    int first = 1;

    while (i < n && (unsigned char)s[i] <= 0x20)
        i++;
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
            return 1;
        if (c == '\\')
            return 0;
        if (c <= 0x20)
            continue;
        if (!md_scheme_char(c, first))
            return 1;
        first = 0;
        if (k + 1 < sizeof scheme)
            scheme[k++] = (char)ht_lower(c);
    }
    if (i >= n || first)
        return 1;
    scheme[k] = 0;
    return strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0
        || strcmp(scheme, "mailto") == 0 || strcmp(scheme, "ftp") == 0;
}

static void md_href(md_t* m, const char* s, size_t n)
{
    if (!md_url_ok(s, n))
        return;
    ht_escape(&m->out, s, n, 1);
}

static int md_code_span(md_t* m, const char* s, size_t n, size_t* i)
{
    size_t open = *i, run = 0, j, st;

    while (open + run < n && s[open + run] == '`')
        run++;
    st = open + run;
    if (md_charge(m, run) < 0)
        return 0;
    for (j = st; j + run <= n; j++) {
        size_t k = 0;
        while (j + k < n && s[j + k] == '`')
            k++;
        if (md_charge(m, 1 + k) < 0)
            return 0;
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

static int md_emphasis(md_t* m, const char* s, size_t n, size_t* i)
{
    char c = s[*i];
    size_t run = 0, j, st;
    const char *open, *close;

    while (*i + run < n && s[*i + run] == c)
        run++;
    if (run > 3)
        return 0;
    if (c == '~' && run != 2)
        return 0;
    st = *i + run;
    if (st >= n || s[st] == ' ')
        return 0;
    for (j = st; j < n; j++) {
        size_t k = 0;
        if (md_charge(m, 1) < 0)
            return 0;
        if (s[j] == '\\') {
            j++;
            continue;
        }
        while (j + k < n && s[j + k] == c)
            k++;
        if (k >= run && j > st && s[j - 1] != ' ')
            break;
        if (k)
            j += k - 1;
    }
    if (j >= n)
        return 0;
    open = c == '~' ? "<del>" : run >= 2 ? "<strong>"
                                         : "<em>";
    close = c == '~' ? "</del>" : run >= 2 ? "</strong>"
                                           : "</em>";
    hb_puts(&m->out, open);
    md_text(m, s + st, j - st);
    hb_puts(&m->out, close);
    *i = j + run;
    return 1;
}

static const uint8_t MD_INL[256] = {
    ['`'] = 1,
    ['*'] = 1,
    ['_'] = 1,
    ['['] = 1,
    ['!'] = 1,
    ['\\'] = 1,
    ['<'] = 1,
    ['&'] = 1,
    ['~'] = 1,
    ['>'] = 1,
    ['"'] = 1,
};

static const uint8_t MD_INL_SET[] = { '`', '*', '_', '[', '!', '\\', '<', '&', '~', '>', '"' };

static void md_text(md_t* m, const char* s, size_t n)
{
    size_t i = 0, run;

    if (m->depth >= MD_MAX_DEPTH) {
        ht_escape(&m->out, s, n, 0);
        return;
    }
    m->depth++;
    while (i < n) {
        char c;
        if (m->exhausted) {
            ht_escape(&m->out, s + i, n - i, 0);
            break;
        }
        run = i;
        if (n - i >= MD_SIMD_MIN) {
            size_t t = simd.find_first_of((const uint8_t*)s + i, n - i,
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
        if (c == '*' || c == '_' || c == '~') {
            size_t delim_run = 1, keep = c == '~' ? 2 : 3;
            while (i + delim_run < n && s[i + delim_run] == c)
                delim_run++;
            if (delim_run > keep) {
                if (md_charge(m, delim_run) < 0)
                    keep = 0;
                ht_escape(&m->out, s + i, delim_run - keep, 0);
                i += delim_run - keep;
                continue;
            }
            if (md_emphasis(m, s, n, &i))
                continue;
        }
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            int img = c == '!';
            size_t j = i + (img ? 2 : 1), depth = 1, ls = j, le, ds, de, ts, te;
            while (j < n && depth) {
                if (md_charge(m, 1) < 0)
                    break;
                if (s[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (s[j] == '[')
                    depth++;
                else if (s[j] == ']')
                    depth--;
                if (depth)
                    j++;
            }
            le = j;
            if (m->exhausted) {
                ht_escape(&m->out, s + i, 1, 0);
                i++;
                continue;
            }
            if (j < n)
                j++;
            if (j < n && md_link_tail(m, s, n, &j, &ds, &de, &ts, &te)) {
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
        if (c == '&') {
            hb_t tmp;
            size_t j = i;
            hb_init(&tmp);
            if (ht_entity(s, n, &j, &tmp)) {
                ht_escape(&m->out, (const char*)tmp.p, tmp.n, 0);
                hb_free(&tmp);
                i = j;
                continue;
            }
            hb_free(&tmp);
        }
        if (c == '<') {
            size_t j = i + 1;
            while (j < n && s[j] != '>' && s[j] != ' ' && s[j] != '\n') {
                if (md_charge(m, 1) < 0)
                    break;
                j++;
            }
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
            hb_put(&m->out, '>');
            i++;
            continue;
        }
        ht_escape(&m->out, s + i, 1, 0);
        i++;
    }
    m->depth--;
}

static int md_fence(const md_line_t* L, const char* s, char* ch, size_t* len)
{
    size_t i = L->start, run = 0;
    char c;

    if (i >= L->end || (s[i] != '`' && s[i] != '~'))
        return 0;
    c = s[i];
    while (i + run < L->end && s[i + run] == c)
        run++;
    if (run < 3)
        return 0;
    *ch = c;
    *len = run;
    return 1;
}

static int md_thematic(const md_line_t* L, const char* s)
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

static size_t md_list_marker(const md_line_t* L, const char* s, int* ordered)
{
    size_t i = L->start;

    if (i < L->end && (s[i] == '-' || s[i] == '*' || s[i] == '+')
        && i + 1 < L->end && (s[i + 1] == ' ' || s[i + 1] == '\t')) {
        *ordered = 0;
        return i + 2;
    }
    while (i < L->end && s[i] >= '0' && s[i] <= '9')
        i++;
    if (i > L->start && i < L->end && (s[i] == '.' || s[i] == ')')
        && i + 1 < L->end && s[i + 1] == ' ') {
        *ordered = 1;
        return i + 2;
    }
    return 0;
}

static int md_setext(const md_line_t* L, const char* s)
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

static int md_table_delim(const md_line_t* L, const char* s)
{
    size_t i;
    int cells = 0, dash = 0;

    for (i = L->start; i < L->end; i++) {
        char c = s[i];
        if (c == '|') {
            if (dash)
                cells++;
            dash = 0;
            continue;
        }
        if (c == '-') {
            dash = 1;
            continue;
        }
        if (c == ':' || c == ' ' || c == '\t')
            continue;
        return 0;
    }
    if (dash)
        cells++;
    return cells >= 1;
}

static void md_table_row(md_t* m, const char* s, const md_line_t* L,
    const char* cell)
{
    size_t i = L->start, st;

    hb_puts(&m->out, "<tr>");
    if (i < L->end && s[i] == '|')
        i++;
    while (i <= L->end) {
        st = i;
        while (i < L->end && s[i] != '|')
            i++;
        {
            size_t a = st, b = i;
            while (a < b && (s[a] == ' ' || s[a] == '\t'))
                a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
                b--;
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
            break;
    }
    hb_puts(&m->out, "</tr>");
}

static uint32_t md_slug_hash(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t md_slug_find(md_t* m, const uint8_t* p, size_t n)
{
    uint32_t mask, i, off, len;
    if (!m->slug_cap)
        return 0;
    mask = m->slug_cap - 1;
    for (i = md_slug_hash(p, n) & mask; (off = m->slug_tab[i]) != 0; i = (i + 1) & mask) {
        memcpy(&len, m->slugs.p + off - 1, 4);
        if (len == n && !memcmp(m->slugs.p + off - 1 + 8, p, n))
            return off;
    }
    return 0;
}

static uint32_t md_slug_add(md_t* m, const uint8_t* p, size_t n)
{
    uint32_t off, len = (uint32_t)n, zero = 0, mask, i;
    if ((size_t)(m->slug_n + 1) * 2 > m->slug_cap) {
        uint32_t nc = m->slug_cap ? m->slug_cap * 2 : 64, j;
        uint32_t* nt = (uint32_t*)calloc(nc, sizeof *nt);
        if (!nt) {
            m->out.oom = 1;
            return 0;
        }
        for (j = 0; j < m->slug_cap; j++) {
            uint32_t o = m->slug_tab[j], l, k;
            if (!o)
                continue;
            memcpy(&l, m->slugs.p + o - 1, 4);
            for (k = md_slug_hash(m->slugs.p + o - 1 + 8, l) & (nc - 1); nt[k]; k = (k + 1) & (nc - 1))
                ;
            nt[k] = o;
        }
        free(m->slug_tab);
        m->slug_tab = nt;
        m->slug_cap = nc;
    }
    off = (uint32_t)m->slugs.n + 1;
    hb_write(&m->slugs, &len, 4);
    hb_write(&m->slugs, &zero, 4);
    hb_write(&m->slugs, p, n);
    if (m->slugs.oom) {
        m->out.oom = 1;
        return 0;
    }
    mask = m->slug_cap - 1;
    for (i = md_slug_hash(p, n) & mask; m->slug_tab[i]; i = (i + 1) & mask)
        ;
    m->slug_tab[i] = off;
    m->slug_n++;
    return off;
}

static void md_slug_of(hb_t* sl, const uint8_t* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c == '<') {
            while (i < n && p[i] != '>')
                i++;
            i++;
        } else if (c == '&') {
            size_t k = i + 1;
            while (k < n && k - i < 32 && p[k] != ';' && p[k] != '&')
                k++;
            i = (k < n && p[k] == ';') ? k + 1 : i + 1;
        } else {
            if (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-' || c == '_')
                hb_put(sl, c);
            else if (c >= 'A' && c <= 'Z')
                hb_put(sl, (uint8_t)(c + 32));
            else if (c == ' ')
                hb_put(sl, '-');
            i++;
        }
    }
}

static void md_heading_id(md_t* m, const hb_t* in)
{
    hb_t sl, cand;
    uint32_t base, cnt;
    char num[16];

    hb_init(&sl);
    md_slug_of(&sl, in->p, in->n);
    if (sl.oom) {
        m->out.oom = 1;
        hb_free(&sl);
        return;
    }
    if (!sl.n) {
        hb_free(&sl);
        return;
    }
    hb_init(&cand);
    hb_write(&cand, sl.p, sl.n);
    base = md_slug_find(m, sl.p, sl.n);
    while (!cand.oom && md_slug_find(m, cand.p, cand.n)) {
        memcpy(&cnt, m->slugs.p + base - 1 + 4, 4);
        cnt++;
        memcpy(m->slugs.p + base - 1 + 4, &cnt, 4);
        cand.n = 0;
        hb_write(&cand, sl.p, sl.n);
        snprintf(num, sizeof num, "-%u", (unsigned)cnt);
        hb_puts(&cand, num);
        md_charge(m, cand.n);
    }
    if (cand.oom)
        m->out.oom = 1;
    else if (md_slug_add(m, cand.p, cand.n)) {
        hb_puts(&m->out, " id=\"");
        hb_write(&m->out, cand.p, cand.n);
        hb_put(&m->out, '"');
    }
    hb_free(&sl);
    hb_free(&cand);
}

static void md_heading(md_t* m, unsigned lvl, const char* s, size_t n)
{
    char tag[8];
    hb_t saved, in;

    snprintf(tag, sizeof tag, "h%u", lvl);
    hb_put(&m->out, '<');
    hb_puts(&m->out, tag);
    if (!m->heading_ids) {
        hb_put(&m->out, '>');
        md_text(m, s, n);
    } else {
        saved = m->out;
        hb_init(&m->out);
        md_text(m, s, n);
        in = m->out;
        m->out = saved;
        if (in.oom)
            m->out.oom = 1;
        md_heading_id(m, &in);
        hb_put(&m->out, '>');
        hb_write(&m->out, in.p, in.n);
        hb_free(&in);
    }
    hb_puts(&m->out, "</");
    hb_puts(&m->out, tag);
    hb_puts(&m->out, ">\n");
}

static size_t md_one_block(md_t* m, size_t from, size_t to)
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
        while (h < L.end && m->s[h] == '#' && lvl < 7) {
            h++;
            lvl++;
        }
        if (lvl <= 6 && h < L.end && m->s[h] == ' ') {
            size_t te = L.end;
            ts = h + 1;
            while (te > ts && (m->s[te - 1] == ' ' || m->s[te - 1] == '#'))
                te--;
            md_heading(m, (unsigned)lvl, m->s + ts, te - ts);
            return next;
        }
    }
    if (md_fence(&L, m->s, &fc, &flen)) {
        size_t info_s = L.start + flen, info_e = L.end, k = next;
        while (info_s < info_e && m->s[info_s] == ' ')
            info_s++;
        hb_puts(&m->out, "<pre><code");
        if (info_e > info_s) {
            size_t ie = info_s;
            while (ie < info_e && m->s[ie] != ' ')
                ie++;
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
    if (L.indent >= 4) {
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
            if (cs < C.end && m->s[cs] == ' ')
                cs++;
            hb_write(&m->out, m->s + cs, C.end - cs);
            hb_put(&m->out, '\n');
            k = after;
        }
        inner = m->out;
        m->out = save;
        hb_puts(&m->out, "<blockquote>\n");
        if (m->depth < MD_MAX_DEPTH) {
            md_t sub = *m;
            sub.s = (const char*)inner.p;
            sub.n = inner.n;
            sub.out = m->out;
            sub.depth = m->depth + 1;
            md_blocks(&sub, 0, inner.n);
            m->out = sub.out;
            m->work = sub.work;
            m->exhausted = sub.exhausted;
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
            if (md_blank(&C)) {
                k = after;
                continue;
            }
            cs = md_list_marker(&C, m->s, &ord2);
            if (!cs || ord2 != ordered)
                break;
            hb_puts(&m->out, "<li>");
            md_text(m, m->s + cs, C.end - cs);
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
    {
        size_t k = from, first = 1;
        int heading = 0;
        hb_t para;
        hb_init(&para);
        while (k < to) {
            md_line_t C;
            size_t after = md_line_at(m->s, to, k, &C);
            int st;
            if (md_blank(&C)) {
                k = after;
                break;
            }
            st = md_setext(&C, m->s);
            if (st && !first) {
                heading = st;
                k = after;
                break;
            }
            if (!first && (md_thematic(&C, m->s) || m->s[C.start] == '>' || m->s[C.start] == '#'))
                break;
            if (!first)
                hb_put(&para, '\n');
            hb_write(&para, m->s + C.start, C.end - C.start);
            first = 0;
            k = after;
        }
        if (heading) {
            md_heading(m, (unsigned)heading, (const char*)para.p, para.n);
        } else if (para.n) {
            hb_puts(&m->out, "<p>");
            md_text(m, (const char*)para.p, para.n);
            hb_puts(&m->out, "</p>\n");
        }
        hb_free(&para);
        return k;
    }
}

static void md_blocks(md_t* m, size_t from, size_t to)
{
    size_t i = from;
    while (i < to) {
        size_t next = md_one_block(m, i, to);
        if (next <= i)
            break;
        i = next;
    }
}

int dyn_md_render_html(const char* s, size_t n, int allow_html, int heading_ids,
    hb_t* out)
{
    md_t m;
    memset(&m, 0, sizeof m);
    m.s = s;
    m.n = n;
    m.allow_html = allow_html;
    m.heading_ids = heading_ids;
    m.work_max = (uint64_t)n * 16 + 4096;
    hb_init(&m.out);
    if (n > 4096) {
        size_t est = n + (n >> 2);
        if (est > MD_MAX_INPUT)
            est = MD_MAX_INPUT;
        hb_reserve(&m.out, est);
    }
    md_blocks(&m, 0, n);
    hb_free(&m.slugs);
    free(m.slug_tab);
    *out = m.out;
    return m.out.oom ? -1 : 0;
}
