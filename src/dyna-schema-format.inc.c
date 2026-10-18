static int dyn_fm_digits(const char* s, size_t n)
{
    size_t i;
    if (!n)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

static int dyn_fm_num(const char* s, size_t n)
{
    int v = 0;
    size_t i;
    for (i = 0; i < n; i++)
        v = v * 10 + (s[i] - '0');
    return v;
}

static int dyn_fm_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int dyn_fm_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int dyn_fm_alnum(char c)
{
    return dyn_fm_alpha(c) || (c >= '0' && c <= '9');
}

static int dyn_fm_date(const char* s, size_t n)
{
    static const int mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y, m, d;
    if (n != 10 || s[4] != '-' || s[7] != '-' || !dyn_fm_digits(s, 4) || !dyn_fm_digits(s + 5, 2) || !dyn_fm_digits(s + 8, 2))
        return 0;
    y = dyn_fm_num(s, 4);
    m = dyn_fm_num(s + 5, 2);
    d = dyn_fm_num(s + 8, 2);
    if (m < 1 || m > 12 || d < 1 || d > mdays[m - 1])
        return 0;
    if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 0;
    return 1;
}

static int dyn_fm_time(const char* s, size_t n)
{
    int hh, mm, ss, oh = 0, om = 0, sign = 0, utc;
    size_t i = 8;
    if (n < 9 || s[2] != ':' || s[5] != ':' || !dyn_fm_digits(s, 2) || !dyn_fm_digits(s + 3, 2) || !dyn_fm_digits(s + 6, 2))
        return 0;
    hh = dyn_fm_num(s, 2);
    mm = dyn_fm_num(s + 3, 2);
    ss = dyn_fm_num(s + 6, 2);
    if (hh > 23 || mm > 59 || ss > 60)
        return 0;
    if (s[i] == '.') {
        size_t j = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        if (i == j)
            return 0;
    }
    if (i < n && (s[i] == 'Z' || s[i] == 'z')) {
        if (i + 1 != n)
            return 0;
    } else {
        if (n - i != 6 || (s[i] != '+' && s[i] != '-') || s[i + 3] != ':' || !dyn_fm_digits(s + i + 1, 2) || !dyn_fm_digits(s + i + 4, 2))
            return 0;
        sign = s[i] == '+' ? 1 : -1;
        oh = dyn_fm_num(s + i + 1, 2);
        om = dyn_fm_num(s + i + 4, 2);
        if (oh > 23 || om > 59)
            return 0;
    }
    if (ss == 60) {
        utc = ((hh * 60 + mm - sign * (oh * 60 + om)) % 1440 + 1440) % 1440;
        if (utc != 23 * 60 + 59)
            return 0;
    }
    return 1;
}

static int dyn_fm_date_time(const char* s, size_t n)
{
    return n > 11 && (s[10] == 'T' || s[10] == 't') && dyn_fm_date(s, 10) && dyn_fm_time(s + 11, n - 11);
}

static size_t dyn_fm_dur_part(const char* s, size_t n, size_t i, char unit)
{
    size_t j = i;
    while (j < n && s[j] >= '0' && s[j] <= '9')
        j++;
    if (j == i || j >= n || s[j] != unit)
        return 0;
    return j + 1 - i;
}

static int dyn_fm_duration(const char* s, size_t n)
{
    static const char date_units[3] = { 'Y', 'M', 'D' };
    static const char time_units[3] = { 'H', 'M', 'S' };
    size_t i = 1, k, adv;
    int seen = 0, last = -1;
    if (n < 3 || s[0] != 'P')
        return 0;
    adv = dyn_fm_dur_part(s, n, 1, 'W');
    if (adv)
        return 1 + adv == n;
    for (k = 0; k < 3 && i < n && s[i] != 'T'; k++) {
        adv = dyn_fm_dur_part(s, n, i, date_units[k]);
        if (adv) {
            if (last >= 0 && last != (int)k - 1)
                return 0;
            last = (int)k;
            i += adv;
            seen = 1;
        }
    }
    if (i < n && s[i] != 'T')
        return 0;
    if (i < n) {
        int tseen = 0;
        last = -1;
        i++;
        for (k = 0; k < 3 && i < n; k++) {
            adv = dyn_fm_dur_part(s, n, i, time_units[k]);
            if (adv) {
                if (last >= 0 && last != (int)k - 1)
                    return 0;
                last = (int)k;
                i += adv;
                tseen = 1;
            }
        }
        if (!tseen || i != n)
            return 0;
        seen = 1;
    }
    return seen && i == n;
}

static int dyn_fm_ipv4(const char* s, size_t n)
{
    size_t i = 0;
    int part;
    for (part = 0; part < 4; part++) {
        size_t j = i;
        while (j < n && s[j] >= '0' && s[j] <= '9' && j - i < 4)
            j++;
        if (j == i || j - i > 3 || (j - i > 1 && s[i] == '0') || dyn_fm_num(s + i, j - i) > 255)
            return 0;
        i = j;
        if (part < 3) {
            if (i >= n || s[i] != '.')
                return 0;
            i++;
        }
    }
    return i == n;
}

static int dyn_fm_ipv6(const char* s, size_t n)
{
    size_t i = 0;
    int groups = 0, dcolon = 0;
    if (n < 2)
        return 0;
    if (s[0] == ':') {
        if (s[1] != ':')
            return 0;
        dcolon = 1;
        i = 2;
        if (i == n)
            return 1;
    }
    while (i < n) {
        size_t j = i;
        while (j < n && dyn_fm_hex(s[j]))
            j++;
        if (j < n && s[j] == '.') {
            if (!dyn_fm_ipv4(s + i, n - i))
                return 0;
            groups += 2;
            i = n;
            break;
        }
        if (j == i || j - i > 4)
            return 0;
        groups++;
        i = j;
        if (i == n)
            break;
        if (s[i] != ':')
            return 0;
        i++;
        if (i < n && s[i] == ':') {
            if (dcolon)
                return 0;
            dcolon = 1;
            i++;
            if (i == n)
                break;
        } else if (i == n) {
            return 0;
        }
    }
    return dcolon ? groups < 8 : groups == 8;
}

static int dyn_fm_label_ok(const char* s, size_t n)
{
    size_t i;
    if (n < 1 || n > 63 || s[0] == '-' || s[n - 1] == '-')
        return 0;
    for (i = 0; i < n; i++)
        if (!dyn_fm_alnum(s[i]) && s[i] != '-')
            return 0;
    if (n >= 4 && s[2] == '-' && s[3] == '-' && !((s[0] == 'x' || s[0] == 'X') && (s[1] == 'n' || s[1] == 'N')))
        return 0;
    return 1;
}

static int dyn_fm_hostname(const char* s, size_t n)
{
    size_t i = 0;
    if (n == 0 || n > 253)
        return 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '.')
            j++;
        if (!dyn_fm_label_ok(s + i, j - i))
            return 0;
#if defined(CONFIG_NATIVE_MODULE_URL)
        if (j - i >= 4 && (s[i] == 'x' || s[i] == 'X') && (s[i + 1] == 'n' || s[i + 1] == 'N') && s[i + 2] == '-' && s[i + 3] == '-'
            && !dyn_idna_hostname_ok(s + i, j - i))
            return 0;
#endif
        i = j + 1;
        if (j + 1 == n)
            return 0;
    }
    return 1;
}

static int dyn_fm_idn_hostname(const char* s, size_t n)
{
    size_t i;
    int ascii = 1;
    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] >= 0x80)
            ascii = 0;
    if (ascii)
        return dyn_fm_hostname(s, n);
#if defined(CONFIG_NATIVE_MODULE_URL)
    return dyn_idna_hostname_ok(s, n);
#else
    return 1;
#endif
}

static int dyn_fm_utf8_next(const char* s, size_t n, size_t* i, uint32_t* cp)
{
    const unsigned char* u = (const unsigned char*)s;
    unsigned char c = u[*i];
    size_t need, k;
    uint32_t v;
    if (c < 0x80) {
        *cp = c;
        (*i)++;
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        need = 1;
        v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        v = c & 0x07;
    } else {
        return 0;
    }
    for (k = 1; k <= need; k++) {
        if (*i + k >= n || (u[*i + k] & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (u[*i + k] & 0x3F);
    }
    *i += need + 1;
    *cp = v;
    return 1;
}

static int dyn_fm_atext(uint32_t c, int intl)
{
    if (c >= 0x80)
        return intl;
    return dyn_fm_alnum((char)c) || (c && strchr("!#$%&'*+-/=?^_`{|}~", (int)c) != NULL);
}

static int dyn_fm_email(const char* s, size_t n, int intl)
{
    size_t i = 0, at;
    uint32_t cp;
    if (n == 0)
        return 0;
    if (s[0] == '"') {
        i = 1;
        while (i < n && s[i] != '"') {
            if (s[i] == '\\') {
                if (i + 1 >= n)
                    return 0;
                i += 2;
                continue;
            }
            if ((unsigned char)s[i] < 0x20 && s[i] != '\t')
                return 0;
            if ((unsigned char)s[i] >= 0x80 && !intl)
                return 0;
            i++;
        }
        if (i >= n)
            return 0;
        i++;
    } else {
        int prev_dot = 1;
        while (i < n && s[i] != '@') {
            if (s[i] == '.') {
                if (prev_dot)
                    return 0;
                prev_dot = 1;
                i++;
                continue;
            }
            if (!dyn_fm_utf8_next(s, n, &i, &cp) || !dyn_fm_atext(cp, intl))
                return 0;
            prev_dot = 0;
        }
        if (prev_dot)
            return 0;
    }
    if (i >= n || s[i] != '@')
        return 0;
    at = i + 1;
    if (at >= n)
        return 0;
    if (s[at] == '[') {
        if (s[n - 1] != ']')
            return 0;
        if (n - at > 7 && !strncasecmp(s + at + 1, "IPv6:", 5))
            return dyn_fm_ipv6(s + at + 6, n - at - 7);
        return dyn_fm_ipv4(s + at + 1, n - at - 2);
    }
    if (intl) {
        size_t j;
        for (j = at; j < n; j++)
            if ((unsigned char)s[j] >= 0x80)
                return 1;
    }
    return dyn_fm_hostname(s + at, n - at);
}

static int dyn_fm_uuid(const char* s, size_t n)
{
    size_t i;
    if (n != 36)
        return 0;
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return 0;
        } else if (!dyn_fm_hex(s[i])) {
            return 0;
        }
    }
    return 1;
}

static int dyn_fm_json_pointer_tail(const char* s, size_t n)
{
    size_t i;
    if (n && s[0] != '/')
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] == '~' && (i + 1 >= n || (s[i + 1] != '0' && s[i + 1] != '1')))
            return 0;
    return 1;
}

static int dyn_fm_rel_json_pointer(const char* s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9')
        i++;
    if (i == 0 || (i > 1 && s[0] == '0'))
        return 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        size_t j = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        if (i == j || (i - j > 1 && s[j] == '0') || (i - j == 1 && s[j] == '0'))
            return 0;
    }
    if (i < n && s[i] == '#')
        return i + 1 == n;
    return dyn_fm_json_pointer_tail(s + i, n - i);
}

static int dyn_fm_ucschar(uint32_t c)
{
    return (c >= 0xA0 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFEF)
        || (c >= 0x10000 && c <= 0xEFFFD && (c & 0xFFFE) != 0xFFFE);
}

static int dyn_fm_iprivate(uint32_t c)
{
    return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD);
}

static int dyn_fm_uri_char(const char* s, size_t n, size_t* i, const char* extra, int iri)
{
    unsigned char c = (unsigned char)s[*i];
    uint32_t cp;
    if (c == '%') {
        if (*i + 2 >= n || !dyn_fm_hex(s[*i + 1]) || !dyn_fm_hex(s[*i + 2]))
            return 0;
        *i += 3;
        return 1;
    }
    if (c >= 0x80) {
        if (!iri || !dyn_fm_utf8_next(s, n, i, &cp) || !(dyn_fm_ucschar(cp) || (iri == 2 && dyn_fm_iprivate(cp))))
            return 0;
        return 1;
    }
    if (dyn_fm_alnum((char)c) || strchr("-._~!$&'()*+,;=", c) || (extra && strchr(extra, c))) {
        if (!c)
            return 0;
        (*i)++;
        return 1;
    }
    return 0;
}

static int dyn_fm_uri_authority(const char* s, size_t n, int iri)
{
    size_t i = 0, at = n, host_end;
    const char* p = memchr(s, '@', n);
    if (p) {
        at = (size_t)(p - s);
        while (i < at)
            if (!dyn_fm_uri_char(s, at, &i, ":", iri))
                return 0;
        i = at + 1;
    }
    if (i < n && s[i] == '[') {
        const char* close = memchr(s + i, ']', n - i);
        if (!close)
            return 0;
        host_end = (size_t)(close - s);
        if (s[i + 1] == 'v' || s[i + 1] == 'V') {
            if (host_end - i < 4)
                return 0;
        } else if (!dyn_fm_ipv6(s + i + 1, host_end - i - 1)) {
            return 0;
        }
        i = host_end + 1;
    } else {
        while (i < n && s[i] != ':')
            if (!dyn_fm_uri_char(s, n, &i, NULL, iri))
                return 0;
    }
    if (i < n) {
        if (s[i] != ':')
            return 0;
        for (i++; i < n; i++)
            if (s[i] < '0' || s[i] > '9')
                return 0;
    }
    return 1;
}

static int dyn_fm_uri(const char* s, size_t n, int absolute, int iri)
{
    size_t i = 0, j;
    int has_scheme = 0;
    for (j = 0; j < n && s[j] != ':' && s[j] != '/' && s[j] != '?' && s[j] != '#'; j++)
        ;
    if (j < n && s[j] == ':' && j > 0 && dyn_fm_alpha(s[0])) {
        size_t k;
        for (k = 1; k < j; k++)
            if (!dyn_fm_alnum(s[k]) && s[k] != '+' && s[k] != '-' && s[k] != '.')
                return 0;
        has_scheme = 1;
        i = j + 1;
    }
    if (absolute && !has_scheme)
        return 0;
    if (!has_scheme && j < n && s[j] == ':' && j > 0)
        return 0;
    if (n - i >= 2 && s[i] == '/' && s[i + 1] == '/') {
        size_t a = i + 2, e = a;
        while (e < n && s[e] != '/' && s[e] != '?' && s[e] != '#')
            e++;
        if (!dyn_fm_uri_authority(s + a, e - a, iri))
            return 0;
        i = e;
    }
    while (i < n && s[i] != '?' && s[i] != '#')
        if (!dyn_fm_uri_char(s, n, &i, ":@/", iri))
            return 0;
    if (i < n && s[i] == '?')
        for (i++; i < n && s[i] != '#';)
            if (!dyn_fm_uri_char(s, n, &i, ":@/?", iri ? 2 : 0))
                return 0;
    if (i < n && s[i] == '#')
        for (i++; i < n;)
            if (!dyn_fm_uri_char(s, n, &i, ":@/?", iri))
                return 0;
    return 1;
}

static int dyn_fm_uri_template(const char* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '{') {
            size_t j = i + 1, k;
            int first = 1;
            if (j < n && strchr("+#./;?&=,!@|", s[j]) && s[j])
                j++;
            for (;;) {
                size_t v = j;
                while (j < n && (dyn_fm_alnum(s[j]) || s[j] == '_' || s[j] == '.' || s[j] == '%')) {
                    if (s[j] == '%' && (j + 2 >= n || !dyn_fm_hex(s[j + 1]) || !dyn_fm_hex(s[j + 2])))
                        return 0;
                    j += s[j] == '%' ? 3 : 1;
                }
                if (j == v || s[v] == '.' || s[j - 1] == '.')
                    return 0;
                for (k = v; k + 1 < j; k++)
                    if (s[k] == '.' && s[k + 1] == '.')
                        return 0;
                if (j < n && s[j] == ':') {
                    size_t d = ++j;
                    while (j < n && s[j] >= '0' && s[j] <= '9')
                        j++;
                    if (j == d || j - d > 4 || s[d] == '0')
                        return 0;
                } else if (j < n && s[j] == '*') {
                    j++;
                }
                first = 0;
                if (j < n && s[j] == ',') {
                    j++;
                    continue;
                }
                break;
            }
            (void)first;
            if (j >= n || s[j] != '}')
                return 0;
            i = j + 1;
            continue;
        }
        if (c == '}' || c < 0x21 || c == 0x7F || strchr("\"<>\\^`|", c))
            return 0;
        if (c == '%') {
            if (i + 2 >= n || !dyn_fm_hex(s[i + 1]) || !dyn_fm_hex(s[i + 2]))
                return 0;
            i += 3;
            continue;
        }
        i++;
    }
    return 1;
}

static int dyn_fm_regex(JSContext* ctx, const char* s, size_t n)
{
    char err[64];
    int len = 0;
    char* z = js_malloc(ctx, n + 1);
    uint8_t* bc;
    if (!z)
        return -1;
    memcpy(z, s, n);
    z[n] = 0;
    bc = lre_compile(&len, err, sizeof err, z, n, LRE_FLAG_UNICODE, ctx);
    js_free(ctx, z);
    if (!bc)
        return 0;
    js_free(ctx, bc);
    return 1;
}

static int dyn_fm_builtin(JSContext* ctx, const char* fmt, const char* s, size_t n)
{
    if (!strcmp(fmt, "date"))
        return dyn_fm_date(s, n);
    if (!strcmp(fmt, "time"))
        return dyn_fm_time(s, n);
    if (!strcmp(fmt, "date-time"))
        return dyn_fm_date_time(s, n);
    if (!strcmp(fmt, "duration"))
        return dyn_fm_duration(s, n);
    if (!strcmp(fmt, "email"))
        return dyn_fm_email(s, n, 0);
    if (!strcmp(fmt, "idn-email"))
        return dyn_fm_email(s, n, 1);
    if (!strcmp(fmt, "hostname"))
        return dyn_fm_hostname(s, n);
    if (!strcmp(fmt, "idn-hostname"))
        return dyn_fm_idn_hostname(s, n);
    if (!strcmp(fmt, "ipv4"))
        return dyn_fm_ipv4(s, n);
    if (!strcmp(fmt, "ipv6"))
        return dyn_fm_ipv6(s, n);
    if (!strcmp(fmt, "uuid"))
        return dyn_fm_uuid(s, n);
    if (!strcmp(fmt, "json-pointer"))
        return dyn_fm_json_pointer_tail(s, n);
    if (!strcmp(fmt, "relative-json-pointer"))
        return dyn_fm_rel_json_pointer(s, n);
    if (!strcmp(fmt, "uri"))
        return dyn_fm_uri(s, n, 1, 0);
    if (!strcmp(fmt, "uri-reference"))
        return dyn_fm_uri(s, n, 0, 0);
    if (!strcmp(fmt, "iri"))
        return dyn_fm_uri(s, n, 1, 1);
    if (!strcmp(fmt, "iri-reference"))
        return dyn_fm_uri(s, n, 0, 1);
    if (!strcmp(fmt, "uri-template"))
        return dyn_fm_uri_template(s, n);
    if (!strcmp(fmt, "regex"))
        return dyn_fm_regex(ctx, s, n);
    return 1;
}
