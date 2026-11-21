#include "../dyn-mailk.h"

#include <string.h>

void mailk_line_init(mailk_line_t* l, size_t max)
{
    dyn_sb_init(&l->buf);
    l->max = max ? max : MAILK_LINE_MAX_DEFAULT;
    l->cr = 0;
}

void mailk_line_free(mailk_line_t* l)
{
    dyn_sb_free(&l->buf);
    l->cr = 0;
}

void mailk_line_reset(mailk_line_t* l)
{
    l->buf.n = 0;
    l->cr = 0;
}

int mailk_line_feed(mailk_line_t* l, const uint8_t* p, size_t n, size_t* used)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (l->cr) {
            if (c != '\n')
                return *used = i, MAILK_EBARE;
            l->cr = 0;
            *used = i + 1;
            return MAILK_OK;
        }
        if (c == '\r') {
            l->cr = 1;
            i++;
            continue;
        }
        if (c == '\n')
            return *used = i, MAILK_EBARE;
        {
            size_t j = i + 1;
            while (j < n && p[j] != '\r' && p[j] != '\n')
                j++;
            if (l->buf.n + (j - i) > l->max)
                return *used = i, MAILK_ELONG;
            dyn_sb_put(&l->buf, (const char*)p + i, j - i);
            if (l->buf.oom)
                return *used = i, MAILK_ENOMEM;
            i = j;
        }
    }
    *used = n;
    return MAILK_NEED;
}

int mailk_utf8_len(const uint8_t* p, size_t n)
{
    uint32_t cp;
    int k, i;
    if (!n)
        return 0;
    if (p[0] < 0x80)
        return 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF)
        k = 2, cp = p[0] & 0x1F;
    else if (p[0] >= 0xE0 && p[0] <= 0xEF)
        k = 3, cp = p[0] & 0x0F;
    else if (p[0] >= 0xF0 && p[0] <= 0xF4)
        k = 4, cp = p[0] & 0x07;
    else
        return 0;
    if ((size_t)k > n)
        return 0;
    for (i = 1; i < k; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((k == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
        return 0;
    return k;
}

static int dig(char c) { return c >= '0' && c <= '9'; }

int mailk_esc_parse(const char* s, size_t n, int* cls, int* subject, int* detail, size_t* used)
{
    size_t i = 0;
    int v[3], k, d;
    for (k = 0; k < 3; k++) {
        if (k && (i >= n || s[i++] != '.'))
            return MAILK_EBAD;
        v[k] = 0;
        for (d = 0; i < n && dig(s[i]); d++, i++) {
            if (d == (k ? 3 : 1))
                return MAILK_EBAD;
            v[k] = v[k] * 10 + (s[i] - '0');
        }
        if (!d)
            return MAILK_EBAD;
    }
    if (i < n && s[i] != ' ')
        return MAILK_EBAD;
    if (v[0] != 2 && v[0] != 4 && v[0] != 5)
        return MAILK_EBAD;
    *cls = v[0];
    *subject = v[1];
    *detail = v[2];
    *used = i;
    return MAILK_OK;
}

void mailk_reply_init(mailk_reply_t* r)
{
    dyn_sb_init(&r->text);
    mailk_line_init(&r->line, MAILK_REPLY_LINE_MAX);
    mailk_reply_reset(r);
}

void mailk_reply_free(mailk_reply_t* r)
{
    dyn_sb_free(&r->text);
    mailk_line_free(&r->line);
}

void mailk_reply_reset(mailk_reply_t* r)
{
    r->code = 0;
    r->esc_class = r->esc_subject = r->esc_detail = -1;
    r->nlines = 0;
    r->done = 0;
    r->text.n = 0;
    mailk_line_reset(&r->line);
}

static int reply_line(mailk_reply_t* r, const char* s, size_t n)
{
    int code, last;
    size_t i;
    if (n < 3 || !dig(s[0]) || !dig(s[1]) || !dig(s[2]) || s[0] < '2' || s[0] > '5' || s[1] > '5')
        return MAILK_EBAD;
    code = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
    if (n > 3 && s[3] != ' ' && s[3] != '-')
        return MAILK_EBAD;
    last = n == 3 || s[3] == ' ';
    for (i = 3; i < n; i++)
        if (!s[i])
            return MAILK_EBAD;
    if (r->nlines && code != r->code)
        return MAILK_EBAD;
    if (r->nlines >= MAILK_REPLY_LINES_MAX)
        return MAILK_ELONG;
    if (!r->nlines) {
        int c, sj, d;
        size_t u;
        r->code = code;
        if (n > 4 && mailk_esc_parse(s + 4, n - 4, &c, &sj, &d, &u) == MAILK_OK && c == code / 100) {
            r->esc_class = c;
            r->esc_subject = sj;
            r->esc_detail = d;
        }
    } else
        dyn_sb_putc(&r->text, '\n');
    if (n > 4)
        dyn_sb_put(&r->text, s + 4, n - 4);
    if (r->text.oom)
        return MAILK_ENOMEM;
    r->nlines++;
    return last ? MAILK_OK : MAILK_NEED;
}

int mailk_reply_feed(mailk_reply_t* r, const uint8_t* p, size_t n, size_t* used)
{
    size_t off = 0;
    if (r->done)
        return *used = 0, MAILK_OK;
    while (off < n) {
        size_t u;
        int rc = mailk_line_feed(&r->line, p + off, n - off, &u);
        off += u;
        if (rc == MAILK_NEED)
            break;
        if (rc != MAILK_OK)
            return *used = off, rc;
        rc = reply_line(r, r->line.buf.p ? r->line.buf.p : "", r->line.buf.n);
        mailk_line_reset(&r->line);
        if (rc == MAILK_OK) {
            r->done = 1;
            return *used = off, MAILK_OK;
        }
        if (rc != MAILK_NEED)
            return *used = off, rc;
    }
    *used = off;
    return MAILK_NEED;
}

static int alnum(char c) { return dig(c) || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

int mailk_keyword_ok(const char* s, size_t n)
{
    size_t i;
    if (!n || !alnum(s[0]))
        return 0;
    for (i = 1; i < n; i++)
        if (!alnum(s[i]) && s[i] != '-')
            return 0;
    return 1;
}

int mailk_ehlo_next(const char* text, size_t n, size_t* pos, mailk_ehlo_ext_t* out)
{
    size_t i = *pos, e, k;
    if (i == 0) {
        while (i < n && text[i] != '\n')
            i++;
        if (i >= n)
            return *pos = n, 0;
        i++;
    }
    if (i >= n)
        return *pos = n, 0;
    for (e = i; e < n && text[e] != '\n'; e++)
        ;
    *pos = e < n ? e + 1 : n;
    for (k = i; k < e && text[k] != ' '; k++)
        ;
    if (!mailk_keyword_ok(text + i, k - i))
        return MAILK_EBAD;
    out->kw = text + i;
    out->kwn = k - i;
    out->params = k < e ? text + k + 1 : text + e;
    out->paramsn = k < e ? e - k - 1 : 0;
    return 1;
}

static const struct {
    const char* name;
    int verb;
} verbs[] = {
    { "HELO", MAILK_VERB_HELO }, { "EHLO", MAILK_VERB_EHLO }, { "MAIL", MAILK_VERB_MAIL },
    { "RCPT", MAILK_VERB_RCPT }, { "DATA", MAILK_VERB_DATA }, { "BDAT", MAILK_VERB_BDAT },
    { "RSET", MAILK_VERB_RSET }, { "NOOP", MAILK_VERB_NOOP }, { "QUIT", MAILK_VERB_QUIT },
    { "VRFY", MAILK_VERB_VRFY }, { "EXPN", MAILK_VERB_EXPN }, { "HELP", MAILK_VERB_HELP },
    { "STARTTLS", MAILK_VERB_STARTTLS }, { "AUTH", MAILK_VERB_AUTH },
};

int mailk_cmd_parse(const char* s, size_t n, mailk_cmd_t* out)
{
    size_t k, v, j;
    for (k = 0; k < n && s[k] != ' '; k++)
        if (!alnum(s[k]))
            return MAILK_EBAD;
    if (!k)
        return MAILK_EBAD;
    out->verb = MAILK_VERB_UNKNOWN;
    for (v = 0; v < sizeof verbs / sizeof verbs[0]; v++) {
        if (strlen(verbs[v].name) != k)
            continue;
        for (j = 0; j < k && (s[j] & ~32) == verbs[v].name[j]; j++)
            ;
        if (j == k) {
            out->verb = verbs[v].verb;
            break;
        }
    }
    out->arg = k < n ? s + k + 1 : s + n;
    out->argn = k < n ? n - k - 1 : 0;
    return MAILK_OK;
}
