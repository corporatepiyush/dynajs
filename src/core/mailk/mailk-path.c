#include "../dyn-mailk.h"

#include <arpa/inet.h>
#include <string.h>

static int isdig(int c) { return c >= '0' && c <= '9'; }
static int isalpha_(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int isletdig(int c) { return isdig(c) || isalpha_(c); }

static int ieq(const char* a, const char* lit, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if ((a[i] | 32) != lit[i])
            return 0;
    return 1;
}

static int atext(int c)
{
    return isletdig(c) || (c && strchr("!#$%&'*+-/=?^_`{|}~", c) != NULL);
}

static size_t utf8_at(const char* s, size_t n, size_t i, unsigned flags)
{
    if ((uint8_t)s[i] < 0x80 || !(flags & MAILK_F_UTF8))
        return 0;
    return (size_t)mailk_utf8_len((const uint8_t*)s + i, n - i);
}

static int local_part(const char* s, size_t n, unsigned flags, size_t* used, mailk_path_t* out)
{
    size_t i = 0, k;
    if (n && s[0] == '"') {
        out->quoted = 1;
        for (i = 1; i < n && s[i] != '"'; i++) {
            uint8_t c = (uint8_t)s[i];
            if (c == '\\') {
                if (i + 1 >= n || (uint8_t)s[i + 1] < 32 || (uint8_t)s[i + 1] > 126)
                    return MAILK_EBAD;
                i++;
            } else if (c >= 0x80) {
                if (!(k = utf8_at(s, n, i, flags)))
                    return MAILK_EBAD;
                out->utf8 = 1;
                i += k - 1;
            } else if (c < 32 || c > 126)
                return MAILK_EBAD;
        }
        if (i >= n)
            return MAILK_EBAD;
        *used = i + 1;
        return MAILK_OK;
    }
    for (;;) {
        size_t a = i;
        while (i < n) {
            if (atext((uint8_t)s[i]))
                i++;
            else if ((k = utf8_at(s, n, i, flags)))
                i += k, out->utf8 = 1;
            else
                break;
        }
        if (i == a)
            return MAILK_EBAD;
        if (i < n && s[i] == '.') {
            i++;
            continue;
        }
        break;
    }
    *used = i;
    return MAILK_OK;
}

static int snum(const char* s, size_t n, size_t* i)
{
    int v = 0, d = 0;
    while (*i < n && isdig((uint8_t)s[*i]) && d < 3)
        v = v * 10 + (s[(*i)++] - '0'), d++;
    return d && v <= 255 && !(*i < n && isdig((uint8_t)s[*i]));
}

static int literal_ok(const char* s, size_t n)
{
    size_t i = 0, k;
    char buf[64];
    struct in6_addr a6;
    if (n && isdig((uint8_t)s[0])) {
        for (k = 0; k < 4; k++) {
            if (k && (i >= n || s[i++] != '.'))
                return 0;
            if (!snum(s, n, &i))
                return 0;
        }
        return i == n;
    }
    for (k = 0; k < n && s[k] != ':'; k++)
        if (!isletdig((uint8_t)s[k]) && !(s[k] == '-' && k + 1 < n && s[k + 1] != ':'))
            return 0;
    if (!k || k >= n || k + 1 >= n)
        return 0;
    if (k == 4 && (s[0] | 32) == 'i' && (s[1] | 32) == 'p' && (s[2] | 32) == 'v' && s[3] == '6') {
        if (n - 5 >= sizeof buf)
            return 0;
        memcpy(buf, s + 5, n - 5);
        buf[n - 5] = 0;
        for (i = 0; buf[i]; i++)
            if (!isdig((uint8_t)buf[i]) && !strchr("abcdefABCDEF:.", buf[i]))
                return 0;
        return inet_pton(AF_INET6, buf, &a6) == 1;
    }
    for (i = k + 1; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 33 || c > 126 || c == '[' || c == '\\' || c == ']')
            return 0;
    }
    return 1;
}

int mailk_domain_ok(const char* s, size_t n, unsigned flags)
{
    size_t i = 0, k;
    if (!n || n > MAILK_DOMAIN_MAX)
        return 0;
    for (;;) {
        size_t a = i;
        int last_hy = 0;
        while (i < n && s[i] != '.') {
            uint8_t c = (uint8_t)s[i];
            if (isletdig(c))
                i++, last_hy = 0;
            else if (c == '-' && i != a)
                i++, last_hy = 1;
            else if ((k = utf8_at(s, n, i, flags)))
                i += k, last_hy = 0;
            else
                return 0;
        }
        if (i == a || last_hy || i - a > MAILK_LABEL_MAX)
            return 0;
        if (i == n)
            return 1;
        i++;
    }
}

static int mailbox_at(const char* s, size_t n, unsigned flags, mailk_path_t* out, int allow_pm)
{
    size_t u;
    memset(out, 0, sizeof *out);
    if (local_part(s, n, flags, &u, out) != MAILK_OK)
        return MAILK_EBAD;
    out->lo = 0;
    out->ln = u;
    if (out->ln > MAILK_LOCAL_MAX)
        return MAILK_ELONG;
    if (u == n && allow_pm && u == 10 && !out->quoted && ieq(s, "postmaster", 10)) {
        out->postmaster = 1;
        return MAILK_OK;
    }
    if (u >= n || s[u] != '@')
        return MAILK_EBAD;
    out->dn_off = u + 1;
    out->dn = n - u - 1;
    if (out->dn && s[u + 1] == '[') {
        if (s[n - 1] != ']' || !literal_ok(s + u + 2, out->dn - 2))
            return MAILK_EBAD;
        out->literal = 1;
        return MAILK_OK;
    }
    if (!mailk_domain_ok(s + u + 1, out->dn, flags))
        return out->dn > MAILK_DOMAIN_MAX ? MAILK_ELONG : MAILK_EBAD;
    for (u = out->dn_off; u < n; u++)
        if ((uint8_t)s[u] >= 0x80)
            out->utf8 = 1;
    return MAILK_OK;
}

int mailk_mailbox_parse(const char* s, size_t n, unsigned flags, mailk_path_t* out)
{
    return mailbox_at(s, n, flags, out, 0);
}

static int adl_skip(const char* s, size_t n, unsigned flags, size_t* i)
{
    size_t a;
    if (*i >= n || s[*i] != '@')
        return MAILK_OK;
    for (;;) {
        if (*i >= n || s[*i] != '@')
            return MAILK_EBAD;
        a = ++*i;
        while (*i < n && s[*i] != ',' && s[*i] != ':')
            ++*i;
        if (*i >= n || !mailk_domain_ok(s + a, *i - a, flags))
            return MAILK_EBAD;
        if (s[(*i)++] == ':')
            return MAILK_OK;
    }
}

int mailk_path_parse(const char* s, size_t n, unsigned flags, int rcpt, mailk_path_t* out, size_t* used)
{
    size_t i = 1, e, q = 0;
    int rc;
    memset(out, 0, sizeof *out);
    if (!n || s[0] != '<')
        return MAILK_EBAD;
    if (!rcpt && n >= 2 && s[1] == '>') {
        out->null_path = 1;
        *used = 2;
        return MAILK_OK;
    }
    if (adl_skip(s, n, flags, &i) != MAILK_OK)
        return MAILK_EBAD;
    for (e = i; e < n; e++) {
        if (s[e] == '"' && (e == i || q))
            q = !q;
        else if (q && s[e] == '\\')
            e++;
        else if (!q && s[e] == '>')
            break;
    }
    if (e >= n)
        return MAILK_EBAD;
    if (e + 1 > MAILK_PATH_MAX)
        return MAILK_ELONG;
    rc = mailbox_at(s + i, e - i, flags, out, rcpt && i == 1);
    if (rc != MAILK_OK)
        return rc;
    out->lo += i;
    out->dn_off += i;
    *used = e + 1;
    return MAILK_OK;
}

static int esmtp_value_ok(const char* s, size_t n, unsigned flags)
{
    size_t i, k;
    if (!n)
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 33 && c <= 126 && c != '=')
            continue;
        if (!(k = utf8_at(s, n, i, flags)))
            return 0;
        i += k - 1;
    }
    return 1;
}

int mailk_param_next(const char* s, size_t n, size_t* pos, unsigned flags, mailk_param_t* out)
{
    size_t i = *pos, e, k;
    if (i >= n)
        return 0;
    for (e = i; e < n && s[e] != ' '; e++)
        ;
    if (e < n && e + 1 >= n)
        return MAILK_EBAD;
    *pos = e < n ? e + 1 : n;
    for (k = i; k < e && s[k] != '='; k++)
        ;
    if (!mailk_keyword_ok(s + i, k - i))
        return MAILK_EBAD;
    out->kw = s + i;
    out->kwn = k - i;
    out->val = k < e ? s + k + 1 : NULL;
    out->valn = k < e ? e - k - 1 : 0;
    if (k < e && !esmtp_value_ok(out->val, out->valn, flags))
        return MAILK_EBAD;
    return 1;
}

int mailk_mail_parse(const char* arg, size_t n, int rcpt, unsigned flags, mailk_path_t* path, size_t* params_off)
{
    const char* kw = rcpt ? "to:" : "from:";
    size_t kl = rcpt ? 3 : 5, i, u;
    int rc;
    if (n < kl || !ieq(arg, kw, kl - 1) || arg[kl - 1] != ':')
        return MAILK_EBAD;
    i = kl;
    if ((flags & MAILK_F_LENIENT) && i < n && arg[i] == ' ')
        i++;
    rc = mailk_path_parse(arg + i, n - i, flags, rcpt, path, &u);
    if (rc != MAILK_OK)
        return rc;
    path->lo += i;
    path->dn_off += i;
    i += u;
    if (i == n)
        return *params_off = n, MAILK_OK;
    if (arg[i] != ' ' || i + 1 >= n)
        return MAILK_EBAD;
    *params_off = i + 1;
    return MAILK_OK;
}

int mailk_cmd_path(dyn_sb_t* out, int rcpt, const char* mbox, size_t n, unsigned flags,
    const char* const* params, size_t np)
{
    mailk_path_t p;
    size_t j;
    int rc;
    if (n || rcpt) {
        rc = mailbox_at(mbox, n, flags, &p, 0);
        if (rc != MAILK_OK)
            return rc;
        if (n + 2 > MAILK_PATH_MAX)
            return MAILK_ELONG;
    }
    for (j = 0; j < np; j++) {
        mailk_param_t pr;
        size_t pos = 0, pl = strlen(params[j]);
        if (!pl || memchr(params[j], ' ', pl) || mailk_param_next(params[j], pl, &pos, flags, &pr) != 1)
            return MAILK_EBAD;
    }
    dyn_sb_puts(out, rcpt ? "RCPT TO:<" : "MAIL FROM:<");
    dyn_sb_put(out, mbox, n);
    dyn_sb_putc(out, '>');
    for (j = 0; j < np; j++) {
        dyn_sb_putc(out, ' ');
        dyn_sb_puts(out, params[j]);
    }
    dyn_sb_put(out, "\r\n", 2);
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

static const char hexu[] = "0123456789ABCDEF";

int mailk_xtext_encode(dyn_sb_t* out, const uint8_t* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c >= 33 && c <= 126 && c != '+' && c != '=')
            dyn_sb_putc(out, (char)c);
        else {
            char h[3] = { '+', hexu[c >> 4], hexu[c & 15] };
            dyn_sb_put(out, h, 3);
        }
    }
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

static int hexv(char c)
{
    return isdig((uint8_t)c) ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

int mailk_xtext_decode(dyn_sb_t* out, const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '+') {
            int h, l;
            if (i + 2 >= n)
                return MAILK_EBAD;
            h = hexv(s[i + 1]);
            l = hexv(s[i + 2]);
            if (h < 0 || l < 0)
                return MAILK_EBAD;
            dyn_sb_putc(out, (char)(h * 16 + l));
            i += 2;
        } else if (c >= 33 && c <= 126 && c != '=')
            dyn_sb_putc(out, (char)c);
        else
            return MAILK_EBAD;
    }
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}
