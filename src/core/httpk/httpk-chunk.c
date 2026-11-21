#include "httpk.h"

enum {
    S_SIZE = 0,
    S_BWS,
    S_EXT,
    S_SIZE_LF,
    S_DATA,
    S_DATA_CR,
    S_DATA_LF,
    S_TR_BOL,
    S_TR,
    S_TR_LF,
    S_END_LF,
    S_DONE
};

void httpk_dechunk_init(httpk_dechunk_t* d)
{
    memset(d, 0, sizeof *d);
    d->state = S_SIZE;
}

static int httpk_hex(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int httpk_bad(httpk_dechunk_t* d, const char* why)
{
    d->why = why;
    return HTTPK_DC_BAD;
}

static int httpk_size_end(httpk_dechunk_t* d)
{
    d->line = 0;
    d->digits = 0;
    d->state = d->left ? S_DATA : S_TR_BOL;
    return 0;
}

int httpk_dechunk(httpk_dechunk_t* d, uint8_t* buf, size_t n,
    size_t* consumed, size_t* out_n)
{
    size_t r = 0, w = 0;
    while (r < n && d->state != S_DONE) {
        unsigned char c = buf[r];
        int h;
        switch (d->state) {
        case S_SIZE:
            h = httpk_hex(c);
            if (h >= 0) {
                if ((d->left >> 48) || d->digits == UINT8_MAX)
                    return httpk_bad(d, "chunk size overflows");
                d->left = (d->left << 4) | (uint64_t)h;
                d->digits++;
                r++;
                break;
            }
            if (!d->digits)
                return httpk_bad(d, "chunk size is not hexadecimal");
            if (c == ' ' || c == '\t')
                d->state = S_BWS;
            else if (c == ';')
                d->state = S_EXT;
            else if (c == '\r')
                d->state = S_SIZE_LF;
            else
                return httpk_bad(d, "junk after chunk size");
            d->line++;
            r++;
            break;
        case S_BWS:
            if (c == ';')
                d->state = S_EXT;
            else if (c == '\r')
                d->state = S_SIZE_LF;
            else if (c != ' ' && c != '\t')
                return httpk_bad(d, "junk after chunk size");
            if (++d->line > HTTPK_CHUNK_EXT_MAX)
                return httpk_bad(d, "chunk extension too long");
            r++;
            break;
        case S_EXT:
            if (c == '\r')
                d->state = S_SIZE_LF;
            else if ((c < 0x20 && c != '\t') || c == 0x7f)
                return httpk_bad(d, "control byte in chunk extension");
            if (++d->line > HTTPK_CHUNK_EXT_MAX)
                return httpk_bad(d, "chunk extension too long");
            r++;
            break;
        case S_SIZE_LF:
            if (c != '\n')
                return httpk_bad(d, "chunk size line not ended by CRLF");
            r++;
            httpk_size_end(d);
            break;
        case S_DATA: {
            size_t take = n - r;
            if ((uint64_t)take > d->left)
                take = (size_t)d->left;
            if (w != r)
                memmove(buf + w, buf + r, take);
            w += take;
            r += take;
            d->left -= take;
            d->total += take;
            if (!d->left)
                d->state = S_DATA_CR;
            break;
        }
        case S_DATA_CR:
            if (c != '\r')
                return httpk_bad(d, "chunk data not followed by CRLF");
            d->state = S_DATA_LF;
            r++;
            break;
        case S_DATA_LF:
            if (c != '\n')
                return httpk_bad(d, "chunk data not followed by CRLF");
            d->state = S_SIZE;
            r++;
            break;
        case S_TR_BOL:
            if (c == '\n' || c == 0)
                return httpk_bad(d, "malformed trailer line");
            d->state = c == '\r' ? S_END_LF : S_TR;
            if (c != '\r' && ++d->line > HTTPK_TRAILER_MAX)
                return httpk_bad(d, "trailer section too large");
            if (c != '\r')
                d->trailer++;
            r++;
            break;
        case S_TR:
            if (c == '\r')
                d->state = S_TR_LF;
            else if (c == '\n' || c == 0)
                return httpk_bad(d, "malformed trailer line");
            if (++d->line > HTTPK_TRAILER_MAX)
                return httpk_bad(d, "trailer section too large");
            d->trailer++;
            r++;
            break;
        case S_TR_LF:
            if (c != '\n')
                return httpk_bad(d, "malformed trailer line");
            d->state = S_TR_BOL;
            d->trailer++;
            r++;
            break;
        case S_END_LF:
            if (c != '\n')
                return httpk_bad(d, "chunked body not ended by CRLF");
            d->state = S_DONE;
            r++;
            break;
        default:
            return httpk_bad(d, "dechunker misuse");
        }
    }
    *consumed = r;
    *out_n = w;
    return d->state == S_DONE ? HTTPK_DC_DONE : HTTPK_DC_MORE;
}

int httpk_chunk_size(const char* line, size_t n, uint64_t* size)
{
    httpk_dechunk_t d;
    uint8_t tmp[2] = { '\r', '\n' };
    size_t i, used, outn;
    httpk_dechunk_init(&d);
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)line[i];
        if (c == '\r' || c == '\n')
            return -1;
        if (httpk_dechunk(&d, &c, 1, &used, &outn) != HTTPK_DC_MORE)
            return -1;
    }
    if (d.state == S_SIZE && !d.digits)
        return -1;
    *size = d.left;
    if (httpk_dechunk(&d, tmp, 2, &used, &outn) == HTTPK_DC_BAD || used != 2)
        return -1;
    return 0;
}
