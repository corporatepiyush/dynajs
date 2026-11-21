#include "dyn-textdec.h"
#include "dyn-textdec-index.h"

#include <string.h>

_Static_assert(DYN_TD_ENC_UTF8 == DYN_TD_UTF_8 && DYN_TD_ENC_REPLACEMENT == DYN_TD_REPLACEMENT
        && DYN_TD_ENC_UTF16BE == DYN_TD_UTF_16BE && DYN_TD_ENC_UTF16LE == DYN_TD_UTF_16LE,
    "dyn-textdec.h ids follow dyn-textdec-index.h");

#define TD_EOQ (-1)
#define TD_CONT (-1)
#define TD_ERR (-2)
#define TD_FIN (-3)

enum { ISO_ASCII, ISO_ROMAN, ISO_KATAKANA, ISO_LEAD, ISO_TRAIL, ISO_ESC_START, ISO_ESC };

typedef struct {
    const uint8_t* in;
    size_t n, i;
    uint8_t pre[4];
    int npre;
} td_queue;

static inline int td_next(td_queue* q)
{
    if (q->npre)
        return q->pre[--q->npre];
    if (q->i < q->n)
        return q->in[q->i++];
    return TD_EOQ;
}

static inline void td_prepend1(td_queue* q, int b)
{
    if (b >= 0)
        q->pre[q->npre++] = (uint8_t)b;
}

static inline void td_prepend2(td_queue* q, int a, int b)
{
    td_prepend1(q, b);
    td_prepend1(q, a);
}

static inline uint16_t* td_put(uint16_t* o, uint32_t c)
{
    if (c < 0x10000) {
        *o++ = (uint16_t)c;
    } else {
        c -= 0x10000;
        *o++ = (uint16_t)(0xD800 | (c >> 10));
        *o++ = (uint16_t)(0xDC00 | (c & 0x3FF));
    }
    return o;
}

int dyn_textdec_label(const char* s, size_t n)
{
    char buf[32];
    size_t a = 0, b = n, i;
    int lo = 0, hi = DYN_TD_LABEL_COUNT - 1;
    while (a < b && (s[a] == 0x09 || s[a] == 0x0A || s[a] == 0x0C || s[a] == 0x0D || s[a] == 0x20))
        a++;
    while (b > a && (s[b - 1] == 0x09 || s[b - 1] == 0x0A || s[b - 1] == 0x0C || s[b - 1] == 0x0D || s[b - 1] == 0x20))
        b--;
    if (b - a >= sizeof(buf) || b == a)
        return -1;
    for (i = a; i < b; i++) {
        char c = s[i];
        if (!c)
            return -1;
        buf[i - a] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    buf[b - a] = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(buf, dyn_td_labels[mid].label);
        if (!c)
            return dyn_td_labels[mid].enc;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}

const char* dyn_textdec_name(int enc)
{
    return enc >= 0 && enc < DYN_TD_COUNT ? dyn_td_names[enc] : "utf-8";
}

void dyn_textdec_init(dyn_textdec_t* d, int enc)
{
    memset(d, 0, sizeof(*d));
    d->enc = (uint8_t)enc;
    d->lead_byte = -1;
    d->iso_state = ISO_ASCII;
    d->iso_out_state = ISO_ASCII;
}

size_t dyn_textdec_max_out(size_t n)
{
    return n + 16;
}

static uint32_t td_gb_range(uint32_t p)
{
    int lo = 0, hi = DYN_TD_GB_RANGES_N - 1;
    if ((p > 39419 && p < 189000) || p > 1237575)
        return 0;
    if (p == 7457)
        return 0xE7C7;
    if (p >= 189000)
        return 0x10000 + p - 189000;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (dyn_td_gb_ranges[mid][0] <= p)
            lo = mid;
        else
            hi = mid - 1;
    }
    return dyn_td_gb_ranges[lo][1] + p - dyn_td_gb_ranges[lo][0];
}

static int td_gb18030(dyn_textdec_t* d, int b, td_queue* q)
{
    if (b == TD_EOQ) {
        if (!d->first && !d->second && !d->third)
            return TD_FIN;
        d->first = d->second = d->third = 0;
        return TD_ERR;
    }
    if (d->third) {
        uint32_t p, c;
        if (b < 0x30 || b > 0x39) {
            q->pre[q->npre++] = (uint8_t)b;
            q->pre[q->npre++] = d->third;
            q->pre[q->npre++] = d->second;
            d->first = d->second = d->third = 0;
            return TD_ERR;
        }
        p = ((((uint32_t)(d->first - 0x81) * 10 + (d->second - 0x30)) * 126 + (d->third - 0x81)) * 10) + (uint32_t)(b - 0x30);
        d->first = d->second = d->third = 0;
        c = td_gb_range(p);
        return c ? (int)c : TD_ERR;
    }
    if (d->second) {
        if (b >= 0x81 && b <= 0xFE) {
            d->third = (uint8_t)b;
            return TD_CONT;
        }
        td_prepend2(q, d->second, b);
        d->first = d->second = 0;
        return TD_ERR;
    }
    if (d->first) {
        int lead = d->first, off = b < 0x7F ? 0x40 : 0x41;
        uint32_t c = 0;
        if (b >= 0x30 && b <= 0x39) {
            d->second = (uint8_t)b;
            return TD_CONT;
        }
        d->first = 0;
        if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFE))
            c = dyn_td_gb18030[(lead - 0x81) * 190 + (b - off)];
        if (c)
            return (int)c;
        if (b < 0x80)
            td_prepend1(q, b);
        return TD_ERR;
    }
    if (b < 0x80)
        return b;
    if (b == 0x80)
        return 0x20AC;
    if (b <= 0xFE) {
        d->first = (uint8_t)b;
        return TD_CONT;
    }
    return TD_ERR;
}

static int td_big5(dyn_textdec_t* d, int b, td_queue* q, uint32_t* second)
{
    if (b == TD_EOQ) {
        if (!d->lead)
            return TD_FIN;
        d->lead = 0;
        return TD_ERR;
    }
    if (d->lead) {
        int l = d->lead, off = b < 0x7F ? 0x40 : 0x62;
        long p = -1;
        uint32_t c = 0;
        d->lead = 0;
        if ((b >= 0x40 && b <= 0x7E) || (b >= 0xA1 && b <= 0xFE))
            p = (long)(l - 0x81) * 157 + (b - off);
        if (p == 1133 || p == 1135 || p == 1164 || p == 1166) {
            *second = (p == 1133 || p == 1164) ? 0x0304 : 0x030C;
            return p < 1164 ? 0x00CA : 0x00EA;
        }
        if (p >= 0 && p < DYN_TD_BIG5_N)
            c = dyn_td_big5[p];
        if (c)
            return (int)c;
        if (b < 0x80)
            td_prepend1(q, b);
        return TD_ERR;
    }
    if (b < 0x80)
        return b;
    if (b >= 0x81 && b <= 0xFE) {
        d->lead = (uint8_t)b;
        return TD_CONT;
    }
    return TD_ERR;
}

static int td_eucjp(dyn_textdec_t* d, int b, td_queue* q)
{
    if (b == TD_EOQ) {
        if (!d->lead)
            return TD_FIN;
        d->lead = 0;
        return TD_ERR;
    }
    if (d->lead == 0x8E && b >= 0xA1 && b <= 0xDF) {
        d->lead = 0;
        return 0xFF61 - 0xA1 + b;
    }
    if (d->lead == 0x8F && b >= 0xA1 && b <= 0xFE) {
        d->jis0212 = 1;
        d->lead = (uint8_t)b;
        return TD_CONT;
    }
    if (d->lead) {
        int l = d->lead;
        uint32_t c = 0;
        d->lead = 0;
        if (l >= 0xA1 && l <= 0xFE && b >= 0xA1 && b <= 0xFE) {
            long p = (long)(l - 0xA1) * 94 + b - 0xA1;
            if (d->jis0212)
                c = p < DYN_TD_JIS0212_N ? dyn_td_jis0212[p] : 0;
            else
                c = p < DYN_TD_JIS0208_N ? dyn_td_jis0208[p] : 0;
        }
        d->jis0212 = 0;
        if (c)
            return (int)c;
        if (b < 0x80)
            td_prepend1(q, b);
        return TD_ERR;
    }
    if (b < 0x80)
        return b;
    if (b == 0x8E || b == 0x8F || (b >= 0xA1 && b <= 0xFE)) {
        d->lead = (uint8_t)b;
        return TD_CONT;
    }
    return TD_ERR;
}

static int td_iso2022jp(dyn_textdec_t* d, int b, td_queue* q)
{
    switch (d->iso_state) {
    case ISO_ASCII:
    case ISO_ROMAN:
    case ISO_KATAKANA:
    case ISO_LEAD:
        if (b == 0x1B) {
            d->iso_state = ISO_ESC_START;
            return TD_CONT;
        }
        if (b == TD_EOQ)
            return TD_FIN;
        d->iso_output = 0;
        if (d->iso_state == ISO_ASCII) {
            if (b <= 0x7F && b != 0x0E && b != 0x0F)
                return b;
        } else if (d->iso_state == ISO_ROMAN) {
            if (b == 0x5C)
                return 0x00A5;
            if (b == 0x7E)
                return 0x203E;
            if (b <= 0x7F && b != 0x0E && b != 0x0F)
                return b;
        } else if (d->iso_state == ISO_KATAKANA) {
            if (b >= 0x21 && b <= 0x5F)
                return 0xFF61 - 0x21 + b;
        } else if (b >= 0x21 && b <= 0x7E) {
            d->lead = (uint8_t)b;
            d->iso_state = ISO_TRAIL;
            return TD_CONT;
        }
        return TD_ERR;
    case ISO_TRAIL:
        if (b == 0x1B) {
            d->iso_state = ISO_ESC_START;
            return TD_ERR;
        }
        d->iso_state = ISO_LEAD;
        if (b >= 0x21 && b <= 0x7E) {
            long p = (long)(d->lead - 0x21) * 94 + b - 0x21;
            uint32_t c = p < DYN_TD_JIS0208_N ? dyn_td_jis0208[p] : 0;
            return c ? (int)c : TD_ERR;
        }
        return TD_ERR;
    case ISO_ESC_START:
        if (b == 0x24 || b == 0x28) {
            d->lead = (uint8_t)b;
            d->iso_state = ISO_ESC;
            return TD_CONT;
        }
        td_prepend1(q, b);
        d->iso_output = 0;
        d->iso_state = d->iso_out_state;
        return TD_ERR;
    default: {
        int l = d->lead, st = -1;
        d->lead = 0;
        if (l == 0x28 && b == 0x42)
            st = ISO_ASCII;
        else if (l == 0x28 && b == 0x4A)
            st = ISO_ROMAN;
        else if (l == 0x28 && b == 0x49)
            st = ISO_KATAKANA;
        else if (l == 0x24 && (b == 0x40 || b == 0x42))
            st = ISO_LEAD;
        if (st >= 0) {
            int out = d->iso_output;
            d->iso_state = d->iso_out_state = (uint8_t)st;
            d->iso_output = 1;
            return out ? TD_ERR : TD_CONT;
        }
        if (b == TD_EOQ)
            td_prepend1(q, l);
        else
            td_prepend2(q, l, b);
        d->iso_output = 0;
        d->iso_state = d->iso_out_state;
        return TD_ERR;
    }
    }
}

static int td_sjis(dyn_textdec_t* d, int b, td_queue* q)
{
    if (b == TD_EOQ) {
        if (!d->lead)
            return TD_FIN;
        d->lead = 0;
        return TD_ERR;
    }
    if (d->lead) {
        int l = d->lead, off = b < 0x7F ? 0x40 : 0x41, loff = l < 0xA0 ? 0x81 : 0xC1;
        long p = -1;
        uint32_t c = 0;
        d->lead = 0;
        if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC))
            p = (long)(l - loff) * 188 + b - off;
        if (p >= 8836 && p <= 10715)
            return (int)(0xE000 - 8836 + p);
        if (p >= 0 && p < DYN_TD_JIS0208_N)
            c = dyn_td_jis0208[p];
        if (c)
            return (int)c;
        if (b < 0x80)
            td_prepend1(q, b);
        return TD_ERR;
    }
    if (b <= 0x80)
        return b;
    if (b >= 0xA1 && b <= 0xDF)
        return 0xFF61 - 0xA1 + b;
    if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
        d->lead = (uint8_t)b;
        return TD_CONT;
    }
    return TD_ERR;
}

static int td_euckr(dyn_textdec_t* d, int b, td_queue* q)
{
    if (b == TD_EOQ) {
        if (!d->lead)
            return TD_FIN;
        d->lead = 0;
        return TD_ERR;
    }
    if (d->lead) {
        int l = d->lead;
        uint32_t c = 0;
        d->lead = 0;
        if (b >= 0x41 && b <= 0xFE)
            c = dyn_td_euckr[(l - 0x81) * 190 + (b - 0x41)];
        if (c)
            return (int)c;
        if (b < 0x80)
            td_prepend1(q, b);
        return TD_ERR;
    }
    if (b < 0x80)
        return b;
    if (b >= 0x81 && b <= 0xFE) {
        d->lead = (uint8_t)b;
        return TD_CONT;
    }
    return TD_ERR;
}

static int td_utf16(dyn_textdec_t* d, int b, td_queue* q, int be)
{
    uint32_t cu;
    if (b == TD_EOQ) {
        if (d->lead_byte < 0 && !d->lead_surrogate)
            return TD_FIN;
        d->lead_byte = -1;
        d->lead_surrogate = 0;
        return TD_ERR;
    }
    if (d->lead_byte < 0) {
        d->lead_byte = (int16_t)b;
        return TD_CONT;
    }
    cu = be ? ((uint32_t)d->lead_byte << 8) | (uint32_t)b : ((uint32_t)b << 8) | (uint32_t)d->lead_byte;
    d->lead_byte = -1;
    if (d->lead_surrogate) {
        uint32_t ls = d->lead_surrogate;
        d->lead_surrogate = 0;
        if (cu >= 0xDC00 && cu <= 0xDFFF)
            return (int)(0x10000 + ((ls - 0xD800) << 10) + (cu - 0xDC00));
        if (be)
            td_prepend2(q, (int)(cu >> 8), (int)(cu & 0xFF));
        else
            td_prepend2(q, (int)(cu & 0xFF), (int)(cu >> 8));
        return TD_ERR;
    }
    if (cu >= 0xD800 && cu <= 0xDBFF) {
        d->lead_surrogate = cu;
        return TD_CONT;
    }
    if (cu >= 0xDC00 && cu <= 0xDFFF)
        return TD_ERR;
    return (int)cu;
}

static long td_single(const uint16_t* t, int user, const uint8_t* in, size_t n, int fatal, uint16_t* out)
{
    uint16_t* o = out;
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t b = in[i], c;
        if (b < 0x80) {
            *o++ = (uint16_t)b;
            continue;
        }
        c = user ? 0xF780 + b - 0x80 : t[b - 0x80];
        if (!c) {
            if (fatal)
                return -1;
            c = 0xFFFD;
        }
        *o++ = (uint16_t)c;
    }
    return (long)(o - out);
}

static inline __attribute__((always_inline)) size_t td_fast(int enc, const uint8_t* in, size_t i, size_t n,
    uint16_t** po)
{
    uint16_t* o = *po;
    while (i < n) {
        uint32_t b = in[i], t, c = 0;
        long p;
        if (b < 0x80) {
            *o++ = (uint16_t)b;
            i++;
            continue;
        }
        if (enc == DYN_TD_SHIFT_JIS && b >= 0xA1 && b <= 0xDF) {
            *o++ = (uint16_t)(0xFF61 - 0xA1 + b);
            i++;
            continue;
        }
        if (i + 1 >= n)
            break;
        t = in[i + 1];
        switch (enc) {
        case DYN_TD_SHIFT_JIS:
            if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) && ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC))) {
                p = (long)(b - (b < 0xA0 ? 0x81 : 0xC1)) * 188 + (long)t - (t < 0x7F ? 0x40 : 0x41);
                if (p < DYN_TD_JIS0208_N && !(p >= 8836 && p <= 10715))
                    c = dyn_td_jis0208[p];
            }
            break;
        case DYN_TD_EUC_JP:
            if (b >= 0xA1 && b <= 0xFE && t >= 0xA1 && t <= 0xFE) {
                p = (long)(b - 0xA1) * 94 + (long)t - 0xA1;
                if (p < DYN_TD_JIS0208_N)
                    c = dyn_td_jis0208[p];
            }
            break;
        case DYN_TD_EUC_KR:
            if (b >= 0x81 && b <= 0xFE && t >= 0x41 && t <= 0xFE)
                c = dyn_td_euckr[(b - 0x81) * 190 + (t - 0x41)];
            break;
        case DYN_TD_BIG5:
            if (b >= 0x81 && b <= 0xFE && ((t >= 0x40 && t <= 0x7E) || (t >= 0xA1 && t <= 0xFE))) {
                p = (long)(b - 0x81) * 157 + (long)t - (t < 0x7F ? 0x40 : 0x62);
                if (p < DYN_TD_BIG5_N && p != 1133 && p != 1135 && p != 1164 && p != 1166)
                    c = dyn_td_big5[p];
            }
            break;
        default:
            if (b >= 0x81 && b <= 0xFE && ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFE)))
                c = dyn_td_gb18030[(b - 0x81) * 190 + (t - (t < 0x7F ? 0x40 : 0x41))];
            break;
        }
        if (!c)
            break;
        o = td_put(o, c);
        i += 2;
    }
    *po = o;
    return i;
}

static inline __attribute__((always_inline)) size_t td_fast16(int be, const uint8_t* in, size_t i, size_t n,
    uint16_t** po)
{
    uint16_t* o = *po;
    while (i + 1 < n) {
        uint32_t cu = be ? ((uint32_t)in[i] << 8) | in[i + 1] : ((uint32_t)in[i + 1] << 8) | in[i];
        if (cu >= 0xD800 && cu <= 0xDFFF) {
            if (cu > 0xDBFF || i + 3 >= n)
                break;
            {
                uint32_t lo = be ? ((uint32_t)in[i + 2] << 8) | in[i + 3] : ((uint32_t)in[i + 3] << 8) | in[i + 2];
                if (lo < 0xDC00 || lo > 0xDFFF)
                    break;
                *o++ = (uint16_t)cu;
                *o++ = (uint16_t)lo;
                i += 4;
                continue;
            }
        }
        *o++ = (uint16_t)cu;
        i += 2;
    }
    *po = o;
    return i;
}

long dyn_textdec_run(dyn_textdec_t* d, const uint8_t* in, size_t n, int flush, int fatal, int strip_bom,
    uint16_t* out)
{
    td_queue q;
    uint16_t* o = out;
    int enc = d->enc;

    if (enc < DYN_TD_COUNT && dyn_td_sb_slot[enc] >= 0)
        return td_single(dyn_td_sb[dyn_td_sb_slot[enc]], 0, in, n, fatal, out);
    if (enc == DYN_TD_X_USER_DEFINED)
        return td_single(NULL, 1, in, n, fatal, out);
    q.in = in;
    q.n = n;
    q.i = 0;
    q.npre = 0;
    for (;;) {
        int b, r;
        uint32_t second = 0;
        if (!q.npre && q.i < q.n) {
            switch (enc) {
            case DYN_TD_UTF_16LE:
                if (d->lead_byte < 0 && !d->lead_surrogate && (d->bom_seen || !strip_bom))
                    q.i = td_fast16(0, q.in, q.i, q.n, &o);
                break;
            case DYN_TD_UTF_16BE:
                if (d->lead_byte < 0 && !d->lead_surrogate && (d->bom_seen || !strip_bom))
                    q.i = td_fast16(1, q.in, q.i, q.n, &o);
                break;
            case DYN_TD_SHIFT_JIS:
                if (!d->lead)
                    q.i = td_fast(DYN_TD_SHIFT_JIS, q.in, q.i, q.n, &o);
                break;
            case DYN_TD_EUC_JP:
                if (!d->lead)
                    q.i = td_fast(DYN_TD_EUC_JP, q.in, q.i, q.n, &o);
                break;
            case DYN_TD_EUC_KR:
                if (!d->lead)
                    q.i = td_fast(DYN_TD_EUC_KR, q.in, q.i, q.n, &o);
                break;
            case DYN_TD_BIG5:
                if (!d->lead)
                    q.i = td_fast(DYN_TD_BIG5, q.in, q.i, q.n, &o);
                break;
            case DYN_TD_GBK:
            case DYN_TD_GB18030:
                if (!d->first)
                    q.i = td_fast(DYN_TD_GB18030, q.in, q.i, q.n, &o);
                break;
            default:
                break;
            }
        }
        b = td_next(&q);
        if (b == TD_EOQ && !flush)
            break;
        switch (enc) {
        case DYN_TD_GBK:
        case DYN_TD_GB18030:
            r = td_gb18030(d, b, &q);
            break;
        case DYN_TD_BIG5:
            r = td_big5(d, b, &q, &second);
            break;
        case DYN_TD_EUC_JP:
            r = td_eucjp(d, b, &q);
            break;
        case DYN_TD_ISO_2022_JP:
            r = td_iso2022jp(d, b, &q);
            break;
        case DYN_TD_SHIFT_JIS:
            r = td_sjis(d, b, &q);
            break;
        case DYN_TD_EUC_KR:
            r = td_euckr(d, b, &q);
            break;
        case DYN_TD_UTF_16BE:
            r = td_utf16(d, b, &q, 1);
            break;
        case DYN_TD_UTF_16LE:
            r = td_utf16(d, b, &q, 0);
            break;
        default:
            if (b == TD_EOQ) {
                r = TD_FIN;
            } else if (!d->first) {
                d->first = 1;
                r = TD_ERR;
            } else {
                r = TD_CONT;
            }
            break;
        }
        if (r == TD_FIN)
            break;
        if (r == TD_CONT)
            continue;
        if (r == TD_ERR) {
            if (fatal)
                return -1;
            r = 0xFFFD;
        }
        if (strip_bom && !d->bom_seen) {
            d->bom_seen = 1;
            if (r == 0xFEFF)
                continue;
        }
        o = td_put(o, (uint32_t)r);
        if (second)
            o = td_put(o, second);
    }
    return (long)(o - out);
}
