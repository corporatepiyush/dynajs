#include "../dyn-mailk.h"

#include <string.h>

enum { S_SOL, S_MID, S_CR };

void mailk_stuff_init(mailk_stuff_t* s, unsigned flags)
{
    memset(s, 0, sizeof *s);
    s->flags = flags;
    s->st = S_SOL;
}

static void stuff_eol(mailk_stuff_t* s, dyn_sb_t* out)
{
    dyn_sb_put(out, "\r\n", 2);
    s->out += 2;
    s->col = 0;
    s->st = S_SOL;
}

int mailk_stuff_feed(mailk_stuff_t* s, dyn_sb_t* out, const uint8_t* p, size_t n)
{
    size_t i = 0;
    s->in += n;
    while (i < n) {
        uint8_t c = p[i];
        if (s->st == S_CR) {
            if (c == '\n') {
                stuff_eol(s, out);
                i++;
                continue;
            }
            if (s->flags & MAILK_F_STRICT)
                return MAILK_EBARE;
            s->fixed_bare++;
            stuff_eol(s, out);
            continue;
        }
        if (c == '\r') {
            s->st = S_CR;
            i++;
            continue;
        }
        if (c == '\n') {
            if (s->flags & MAILK_F_STRICT)
                return MAILK_EBARE;
            s->fixed_bare++;
            stuff_eol(s, out);
            i++;
            continue;
        }
        if (s->st == S_SOL && c == '.') {
            dyn_sb_putc(out, '.');
            s->out++;
        }
        {
            size_t j = i;
            while (j < n && p[j] != '\r' && p[j] != '\n') {
                if (p[j] >= 0x80)
                    s->has8bit = 1;
                else if (!p[j])
                    s->hasnul = 1;
                j++;
            }
            dyn_sb_put(out, (const char*)p + i, j - i);
            s->out += j - i;
            s->col += (uint32_t)(j - i);
            if (s->col > s->maxcol)
                s->maxcol = s->col;
            s->st = S_MID;
            i = j;
        }
    }
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_stuff_end(mailk_stuff_t* s, dyn_sb_t* out)
{
    if (s->st == S_CR) {
        if (s->flags & MAILK_F_STRICT)
            return MAILK_EBARE;
        s->fixed_bare++;
        stuff_eol(s, out);
    } else if (s->st == S_MID)
        stuff_eol(s, out);
    dyn_sb_put(out, ".\r\n", 3);
    s->out += 3;
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

enum { U_SOL, U_DOT, U_DOTCR, U_MID, U_CR };

void mailk_unstuff_init(mailk_unstuff_t* u, unsigned flags, uint64_t max)
{
    memset(u, 0, sizeof *u);
    u->flags = flags;
    u->max = max;
    u->st = U_SOL;
}

static void emit(mailk_unstuff_t* u, dyn_sb_t* out, const char* s, size_t n)
{
    if (u->over)
        return;
    if (u->max && u->bytes + n > u->max) {
        n = (size_t)(u->max - u->bytes);
        u->over = 1;
    }
    dyn_sb_put(out, s, n);
    u->bytes += n;
}

int mailk_unstuff_feed(mailk_unstuff_t* u, dyn_sb_t* out, const uint8_t* p, size_t n, size_t* used)
{
    size_t i = 0;
    if (u->done)
        return *used = 0, MAILK_OK;
    while (i < n) {
        uint8_t c = p[i];
        switch (u->st) {
        case U_SOL:
            if (c == '.') {
                u->st = U_DOT;
                i++;
                continue;
            }
            break;
        case U_DOT:
            if (c == '\r') {
                u->st = U_DOTCR;
                i++;
                continue;
            }
            u->st = U_MID;
            break;
        case U_DOTCR:
            if (c == '\n') {
                u->done = 1;
                *used = i + 1;
                return out->oom ? MAILK_ENOMEM : MAILK_OK;
            }
            if (u->flags & MAILK_F_STRICT)
                return *used = i, MAILK_EBARE;
            u->bare++;
            emit(u, out, "\r", 1);
            u->st = U_MID;
            continue;
        case U_CR:
            if (c == '\n') {
                emit(u, out, "\r\n", 2);
                u->st = U_SOL;
                i++;
                continue;
            }
            if (u->flags & MAILK_F_STRICT)
                return *used = i, MAILK_EBARE;
            u->bare++;
            emit(u, out, "\r", 1);
            u->st = U_MID;
            continue;
        default:
            break;
        }
        if (c == '\r') {
            u->st = U_CR;
            i++;
            continue;
        }
        if (c == '\n') {
            if (u->flags & MAILK_F_STRICT)
                return *used = i, MAILK_EBARE;
            u->bare++;
            emit(u, out, "\n", 1);
            u->st = U_MID;
            i++;
            continue;
        }
        {
            size_t j = i + 1;
            while (j < n && p[j] != '\r' && p[j] != '\n')
                j++;
            emit(u, out, (const char*)p + i, j - i);
            u->st = U_MID;
            i = j;
        }
    }
    *used = n;
    return out->oom ? MAILK_ENOMEM : MAILK_NEED;
}
