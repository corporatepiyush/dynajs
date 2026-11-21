#include "urlrec.h"

static int dyn_set_forbidden(dyn_parse_set_t set, unsigned char c)
{
    return dyn_url_cls_has((unsigned)set, c);
}

void dyn_parse_encode(dyn_sb_t* b, const char* s, size_t n,
    dyn_parse_set_t set)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_set_forbidden(set, c)) {
            size_t j = i + 1;
            while (j < n && !dyn_set_forbidden(set, (unsigned char)s[j]))
                j++;
            dyn_sb_put(b, s + i, j - i);
            i = j - 1;
        } else {
            dyn_sb_putc(b, '%');
            dyn_sb_putc(b, HEX[c >> 4]);
            dyn_sb_putc(b, HEX[c & 0xF]);
        }
    }
}

const char* dyn_url_default_port(const char* scheme)
{
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws"))
        return "80";
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss"))
        return "443";
    if (!strcmp(scheme, "ftp"))
        return "21";
    return NULL;
}

static int dyn_authority_end(char c, int special)
{
    return c == '/' || (special && c == '\\');
}

static int dyn_is_dot_seg(const char* p, size_t len, int dbl)
{
    if (dbl) {
        if (len == 2 && p[0] == '.' && p[1] == '.')
            return 1;
        if (len == 4 && p[0] == '.' && (p[1] | 0x20) == '%' && (p[2] | 0x20) == '2' && (p[3] | 0x20) == 'e')
            return 1;
        if (len == 4 && (p[0] | 0x20) == '%' && (p[1] | 0x20) == '2' && (p[2] | 0x20) == 'e' && p[3] == '.')
            return 1;
        if (len == 6 && (p[0] | 0x20) == '%' && (p[1] | 0x20) == '2' && (p[2] | 0x20) == 'e' && (p[3] | 0x20) == '%' && (p[4] | 0x20) == '2' && (p[5] | 0x20) == 'e')
            return 1;
        return 0;
    }
    if (len == 1 && p[0] == '.')
        return 1;
    if (len == 3 && (p[0] | 0x20) == '%' && (p[1] | 0x20) == '2' && (p[2] | 0x20) == 'e')
        return 1;
    return 0;
}

static void dyn_remove_dots(dyn_sb_t* out, const char* p, size_t n,
    int file_drive, int special)
{
    dyn_sb_t seg;
    size_t i = 0;
    int direct = out->n == 0 && !out->oom;
    if (direct)
        seg = *out;
    else
        dyn_sb_init(&seg);
    while (i <= n) {
        size_t b = i;
        while (i < n && p[i] != '/')
            i++;
        {
            size_t len = i - b;
            if (dyn_is_dot_seg(p + b, len, 0)) {
                if (i >= n)
                    dyn_sb_putc(&seg, '/');
            } else if (dyn_is_dot_seg(p + b, len, 1)) {
                size_t before = seg.n;
                if (file_drive && before == 3 && ((seg.p[1] | 0x20) >= 'a' && (seg.p[1] | 0x20) <= 'z') && seg.p[2] == ':') {
                } else {
                    size_t k = seg.n;
                    while (k > 0 && seg.p[k - 1] != '/')
                        k--;
                    seg.n = k > 0 ? k - 1 : 0;
                }
                if (i >= n)
                    dyn_sb_putc(&seg, '/');
            } else {
                dyn_sb_putc(&seg, '/');
                dyn_sb_put(&seg, p + b, len);
            }
        }
        if (i >= n)
            break;
        i++;
    }
    if (direct) {
        *out = seg;
        if (out->n == 0 && special)
            dyn_sb_putc(out, '/');
        return;
    }
    if (seg.n == 0) {
        if (!special)
            goto done;
        dyn_sb_putc(out, '/');
    } else
        dyn_sb_put(out, seg.p, seg.n);
done:
    dyn_sb_free(&seg);
}

static int dyn_parse_authority(dyn_url_t* u, const char* s, size_t n,
    int* port_colon)
{
    size_t at = (size_t)-1, i;
    size_t hb, he;
    int special = u->scheme ? dyn_scheme_is_special(u->scheme) : 0;
    for (i = 0; i < n; i++)
        if (s[i] == '@')
            at = i;
    if (at != (size_t)-1) {
        size_t c = (size_t)-1;
        for (i = 0; i < at; i++)
            if (s[i] == ':') {
                c = i;
                break;
            }
        if (c != (size_t)-1) {
            if (dyn_url_set_enc(&u->username, s, c, DYN_SET_USERINFO) < 0
                || dyn_url_set_enc(&u->password, s + c + 1, at - c - 1,
                       DYN_SET_USERINFO)
                    < 0)
                return -1;
        } else if (dyn_url_set_enc(&u->username, s, at, DYN_SET_USERINFO) < 0) {
            return -1;
        }
        hb = at + 1;
    } else {
        hb = 0;
    }
    he = n;
    if (hb < n && s[hb] == '[') {
        size_t close = hb;
        while (close < n && s[close] != ']')
            close++;
        if (close >= n)
            return -1;
        if (close + 1 < n && s[close + 1] == ':') {
            if (dyn_url_set_str(&u->port, s + close + 2, n - close - 2) < 0)
                return -1;
            he = close + 1;
        } else {
            if (close + 1 < n)
                return -1;
            he = close + 1;
        }
    } else if (!(u->scheme && !strcmp(u->scheme, "file"))) {
        size_t c = (size_t)-1;
        for (i = hb; i < n; i++)
            if (s[i] == ':')
                c = i;
        if (c != (size_t)-1) {
            if (port_colon)
                *port_colon = 1;
            if (dyn_url_set_str(&u->port, s + c + 1, n - c - 1) < 0)
                return -1;
            he = c;
        }
    }
    if (dyn_url_set_str(&u->host, s + hb, he - hb) < 0)
        return -1;
    if (u->host[0] != '[') {
        const char* h = u->host;
        size_t hl = he - hb, k2;
        int file_drive_host = 0;
        if (u->scheme && !strcmp(u->scheme, "file") && hl >= 1 && hl <= 2 && ((u->host[0] | 0x20) >= 'a' && (u->host[0] | 0x20) <= 'z') && (hl == 1 || u->host[1] == '|' || u->host[1] == ':'))
            file_drive_host = 1;
        for (k2 = 0; k2 < hl && !file_drive_host; k2++) {
            unsigned char c = (unsigned char)h[k2];
            if (c == 0x00 || c == 0x20 || c == '<' || c == '>' || c == '\\' || c == '^' || c == '|' || c == '[' || c == ']' || c == ':')
                return -1;
            if (special && ((c < 0x20) || c == 0x7F))
                return -1;
        }
    }
    if (u->port) {
        const char* d;
        size_t k, pl = strlen(u->port);
        uint64_t pv = 0;
        for (k = 0; k < pl; k++) {
            if (u->port[k] < '0' || u->port[k] > '9')
                return -1;
            pv = pv * 10 + (uint64_t)(u->port[k] - '0');
            if (pv > 65535)
                return -1;
        }
        if (pl == 0) {
            free(u->port);
            u->port = NULL;
        } else if (pl > 1 && u->port[0] == '0') {
            char canon[6];
            snprintf(canon, sizeof canon, "%llu", (unsigned long long)pv);
            free(u->port);
            u->port = dyn_strndup(canon, strlen(canon));
            if (!u->port)
                return -1;
        }
        if (u->port && u->scheme && !strcmp(u->scheme, "file"))
            return -1;
        d = u->scheme ? dyn_url_default_port(u->scheme) : NULL;
        if (u->port && d && strcmp(u->port, d) == 0) {
            free(u->port);
            u->port = NULL;
        }
    }
    return u->host ? 0 : -1;
}

static int dyn_scheme_char(char c, int first)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    if (first)
        return 0;
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

int dyn_url_parse(dyn_url_t* u, const char* s, size_t n,
    const dyn_url_t* base)
{
    size_t i = 0, scheme_end = 0;
    int has_scheme = 0, special = 0;

    while (n && (unsigned char)s[n - 1] <= 0x20)
        n--;
    while (n && (unsigned char)*s <= 0x20) {
        s++;
        n--;
    }

    if (n && dyn_scheme_char(s[0], 1)) {
        for (i = 1; i < n && dyn_scheme_char(s[i], 0); i++)
            ;
        if (i < n && s[i] == ':') {
            has_scheme = 1;
            scheme_end = i;
        }
    }
    if (has_scheme) {
        u->scheme = dyn_strndup(s, scheme_end);
        if (!u->scheme)
            return -1;
        dyn_lower(u->scheme);
        i = scheme_end + 1;
    } else {
        if (!base)
            return -1;
        if (dyn_url_set_str(&u->scheme, base->scheme,
                strlen(base->scheme))
            < 0)
            return -1;
        if (!base->has_authority && !(n && s[0] == '#') && !(base->path && base->path[0] == '/'))
            return -1;
        i = 0;
    }

    special = dyn_scheme_is_special(u->scheme);
    {
        size_t hash = (size_t)-1, q = (size_t)-1, k;
        size_t end = n;
        for (k = i; k < n; k++) {
            if (s[k] == '#') {
                hash = k;
                break;
            }
        }
        if (hash != (size_t)-1) {
            if (dyn_url_set_enc(&u->fragment, s + hash + 1, n - hash - 1,
                    DYN_SET_FRAGMENT)
                < 0)
                return -1;
            end = hash;
        }
        for (k = i; k < end; k++)
            if (s[k] == '?') {
                q = k;
                break;
            }
        if (q != (size_t)-1) {
            if (dyn_url_set_enc(&u->query, s + q + 1, end - q - 1,
                    special ? DYN_SET_QUERY_SPECIAL : DYN_SET_QUERY)
                < 0)
                return -1;
            end = q;
        }
        n = end;
    }

    int special_same_base = has_scheme && special && base && base->scheme && strcmp(base->scheme, u->scheme) == 0;
    int file_drive_ref = 0;
    int file_slash_opened = 0;
    int file_noslash_drive = 0;
    if (has_scheme && special && !strcmp(u->scheme, "file") && n - i >= 2) {
        size_t fj = (s[i] == '/' || s[i] == '\\') ? i + 1 : i;
        if (n - fj >= 2 && ((s[fj] | 0x20) >= 'a' && (s[fj] | 0x20) <= 'z') && (s[fj + 1] == '|' || s[fj + 1] == ':') && (n - fj == 2 || s[fj + 2] == '/' || s[fj + 2] == '\\' || s[fj + 2] == '?' || s[fj + 2] == '#')) {
            file_drive_ref = 1;
            if (fj == i)
                file_noslash_drive = 1;
        }
    } else if (base && base->scheme && !strcmp(base->scheme, "file")) {
        size_t fi = has_scheme ? i : 0;
        if (n - fi >= 2 && ((s[fi] | 0x20) >= 'a' && (s[fi] | 0x20) <= 'z') && (s[fi + 1] == '|' || s[fi + 1] == ':') && (n - fi == 2 || s[fi + 2] == '/' || s[fi + 2] == '\\' || s[fi + 2] == '?' || s[fi + 2] == '#'))
            file_drive_ref = 1;
    }
    {
        size_t ab = n + 1;
        if (has_scheme && special && strcmp(u->scheme, "file") != 0 && !special_same_base) {
            ab = i;
            while (ab < n && (s[ab] == '/' || s[ab] == '\\'))
                ab++;
        } else if (i + 1 < n && (s[i] == '/' || (special && s[i] == '\\')) && (s[i + 1] == '/' || (special && s[i + 1] == '\\'))) {
            ab = i + 2;
            if (special && strcmp(u->scheme, "file") != 0)
                while (ab < n && (s[ab] == '/' || s[ab] == '\\'))
                    ab++;

        } else if (has_scheme && !strcmp(u->scheme, "file") && i >= n) {
            u->has_authority = 1;
            if (dyn_url_set_str(&u->host, "", 0) < 0)
                return -1;
            file_slash_opened = 1;
        } else if (has_scheme && !strcmp(u->scheme, "file") && i < n && (s[i] == '/' || s[i] == '\\')) {
            u->has_authority = 1;
            if (base && base->scheme && !strcmp(base->scheme, "file")) {
                u->has_authority = base->has_authority;
                if (base->host) {
                    if (dyn_url_set_str(&u->host, base->host,
                            strlen(base->host))
                        < 0)
                        return -1;
                }
            } else {
                if (dyn_url_set_str(&u->host, "", 0) < 0)
                    return -1;
                file_slash_opened = 1;
            }
            i = i + 1;
        } else if (file_noslash_drive) {
            u->has_authority = 1;
            if (base && base->scheme && !strcmp(base->scheme, "file")) {
                u->has_authority = base->has_authority;
                if (base->host
                    && dyn_url_set_str(&u->host, base->host,
                           strlen(base->host))
                        < 0)
                    return -1;
            } else {
                if (dyn_url_set_str(&u->host, "", 0) < 0)
                    return -1;
                file_slash_opened = 1;
            }
        }
        if (ab <= n) {
            size_t ae = ab;
            int port_colon = 0;
            while (ae < n && !dyn_authority_end(s[ae], special))
                ae++;
            if (dyn_parse_authority(u, s + ab, ae - ab, &port_colon) < 0)
                return -1;
            if (dyn_url_normalize_host(u) < 0)
                return -1;
            if (special && strcmp(u->scheme, "file") != 0 && u->host && u->host[0] == '\0')
                return -1;
            if (!special && u->host && u->host[0] == '\0' && (port_colon || u->port || u->username || u->password))
                return -1;
            u->has_authority = 1;
            i = ae;
            base = NULL;
            if (i >= n) {
                if (u->drive) {
                    u->path = dyn_strndup(u->drive, strlen(u->drive));
                } else if (special)
                    u->path = dyn_strndup("/", 1);
                else
                    u->path = dyn_strndup("", 0);
                return u->path ? 0 : -1;
            }
        } else if (base && !file_slash_opened && (!has_scheme || special_same_base || file_drive_ref)) {
            u->has_authority = base->has_authority;
            if (base->host
                && dyn_url_set_str(&u->host, base->host,
                       strlen(base->host))
                    < 0)
                return -1;
            if (base->port
                && dyn_url_set_str(&u->port, base->port,
                       strlen(base->port))
                    < 0)
                return -1;
            if (base->username
                && dyn_url_set_str(&u->username, base->username,
                       strlen(base->username))
                    < 0)
                return -1;
            if (base->password
                && dyn_url_set_str(&u->password, base->password,
                       strlen(base->password))
                    < 0)
                return -1;
        } else if (has_scheme && !file_slash_opened) {
            if (u->scheme && !strcmp(u->scheme, "file")) {
                u->has_authority = 1;
                free(u->host);
                u->host = dyn_strndup("", 0);
                if (!u->host)
                    return -1;
            } else {
                u->has_authority = 0;
            }
        }
    }

    {
        dyn_sb_t out;
        char* conv = NULL;
        const char* ps = s;
        int rel = !has_scheme || special_same_base;
        int is_file = u->scheme && !strcmp(u->scheme, "file");
        int opaque = !u->has_authority && has_scheme && !is_file && !(i < n && s[i] == '/');
        if (special && i < n) {
            size_t k;
            int has = 0;
            for (k = i; k < n; k++)
                if (s[k] == '\\') {
                    has = 1;
                    break;
                }
            if (has) {
                conv = (char*)malloc(n - i);
                if (!conv)
                    return -1;
                for (k = i; k < n; k++)
                    conv[k - i] = (s[k] == '\\') ? '/' : s[k];
                ps = conv - i;
            }
        }
        dyn_sb_init(&out);
        if (i < n && dyn_authority_end(ps[i], special)) {
            if (is_file && rel && base && base->path && base->path[0] == '/' && ((base->path[1] | 0x20) >= 'a' && (base->path[1] | 0x20) <= 'z') && base->path[2] == ':' && (base->path[3] == '\0' || base->path[3] == '/')) {
                size_t rl = n - i - 1;
                int ref_is_drive = rl >= 2 && ((ps[i + 1] | 0x20) >= 'a' && (ps[i + 1] | 0x20) <= 'z') && (ps[i + 2] == ':' || ps[i + 2] == '|') && (rl == 2 || ps[i + 3] == '/' || ps[i + 3] == '\\' || ps[i + 3] == '?' || ps[i + 3] == '#');
                if (!ref_is_drive) {
                    dyn_sb_t t;
                    dyn_sb_init(&t);
                    dyn_sb_put(&t, base->path + 1, 2);
                    dyn_sb_putc(&t, '/');
                    dyn_sb_put(&t, ps + i + 1, n - i - 1);
                    dyn_remove_dots(&out, t.p, t.n, 1, special);
                    dyn_sb_free(&t);
                } else {
                    dyn_remove_dots(&out, ps + i + 1, n - i - 1, is_file, special);
                }
            } else {
                dyn_remove_dots(&out, ps + i + 1, n - i - 1, is_file, special);
            }
        } else if (opaque) {
            static const char HEX[] = "0123456789ABCDEF";
            size_t k;
            for (k = i; k < n; k++) {
                unsigned char c = (unsigned char)ps[k];
                if (c == ' ') {
                    unsigned char nx = (k + 1 < n)  ? (unsigned char)ps[k + 1]
                        : (u->query || u->fragment) ? (unsigned char)s[n]
                                                    : 0;
                    if (nx == '?' || nx == '#')
                        dyn_sb_puts(&out, "%20");
                    else
                        dyn_sb_putc(&out, ' ');
                } else if (!dyn_set_forbidden(DYN_SET_OPAQUE, c)) {
                    dyn_sb_putc(&out, (char)c);
                } else {
                    dyn_sb_putc(&out, '%');
                    dyn_sb_putc(&out, HEX[c >> 4]);
                    dyn_sb_putc(&out, HEX[c & 0xF]);
                }
            }
        } else if (i >= n) {
            if (base && base->path && rel) {
                dyn_sb_puts(&out, base->path);
                if (!u->query && base->query
                    && dyn_url_set_str(&u->query, base->query,
                           strlen(base->query))
                        < 0)
                    return -1;
            } else {
                dyn_sb_putc(&out, '/');
            }
        } else if (file_drive_ref) {
            size_t first = has_scheme ? i : 0;
            dyn_sb_putc(&out, '/');
            dyn_sb_put(&out, ps + first, n - first);
            if (out.n > 2 && out.p[2] == '|')
                out.p[2] = ':';
            {
                dyn_sb_t t;
                dyn_sb_init(&t);
                dyn_remove_dots(&t, out.p + 1, out.n - 1, 1, special);
                dyn_sb_free(&out);
                out = t;
            }
        } else if (base && base->path && rel) {
            dyn_sb_t merged;
            size_t bl = strlen(base->path), cut = 0, k;
            int skip = 1;
            dyn_sb_init(&merged);
            for (k = 0; k < bl; k++)
                if (base->path[k] == '/')
                    cut = k + 1;
            if (cut == 0 && base->path[0] != '/') {
                skip = 0;
            }
            dyn_sb_put(&merged, base->path, cut);
            dyn_sb_put(&merged, ps + i, n - i);
            dyn_remove_dots(&out, merged.p + skip,
                merged.n ? merged.n - skip : 0, is_file, special);
            dyn_sb_free(&merged);
        } else {
            dyn_remove_dots(&out, ps + i, n - i, is_file, special);
        }
        if (is_file && out.n >= 3 && out.p[0] == '/' && ((out.p[1] | 0x20) >= 'a' && (out.p[1] | 0x20) <= 'z') && ((out.p[2] == ':') || (out.p[2] == '|' && (out.n == 3 || out.p[3] == '/')))) {
            if (out.p[2] == '|')
                out.p[2] = ':';
        }
        if (u->drive) {
            dyn_sb_t t;
            dyn_sb_init(&t);
            dyn_sb_puts(&t, u->drive);
            dyn_sb_put(&t, out.p ? out.p : "/", out.n ? out.n : 1);
            dyn_sb_free(&out);
            out = t;
        }
        free(conv);
        if (out.oom) {
            dyn_sb_free(&out);
            return -1;
        }
        if (dyn_url_take_enc(&u->path, &out, DYN_SET_PATH, opaque) < 0) {
            dyn_sb_free(&out);
            return -1;
        }
        dyn_sb_free(&out);
    }
    return u->path ? 0 : -1;
}
