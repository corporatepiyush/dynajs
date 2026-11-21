#include "dyn-resp.h"
#include "dyn-frame.h"
#include "../../tests/lib/dyn-resp-model.h"

#define main frame_test_main
#include "../../tests/net/test_frame_decoder.c"
#undef main

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rm_msg_t g_msg;

static void die(const char* m)
{
    fprintf(stderr, "oracle: %s\n", m);
    abort();
}

static void fz_tree(rm_src_t* s)
{
    const char* e;
    rm_gen(s, &g_msg);
    if ((e = rm_check_tree(&g_msg, s)) != NULL)
        die(e);
}

static void fz_raw(rm_src_t* s)
{
    static const uint8_t TAGS[] = { '+', '-', ':', '$', '*', '_', ',', '#', '!', '=', '(', '%', '~', '|', '>' };
    uint8_t* b;
    size_t mb = rm_pick(s, 4) ? 0 : rm_pick(s, 64), n = s->n;
    const char* e;
    if (!n)
        return;
    b = (uint8_t*)malloc(n);
    memcpy(b, s->p, n);
    if (b[0] & 1)
        b[0] = TAGS[b[0] / 2 % sizeof TAGS];
    s->p += n;
    s->n = 0;
    e = rm_check_raw(b, n, mb, s);
    free(b);
    if (e)
        die(e);
}

static void fz_cmd(rm_src_t* s)
{
    const char* e = rm_check_cmd(s);
    if (e)
        die(e);
}

static void fz_frame(rm_src_t* s)
{
    static const int widths[5] = { 1, 2, 3, 4, 8 };
    static uint8_t S[4096], dl[4][8];
    static const char alpha[] = "abc\r\n<>";
    spec_t sp;
    size_t n = 0, cuts[64], nc = 0, i, j;
    dyn_frame_t* d;
    int err = 0, before = g_fail;
    memset(&sp, 0, sizeof sp);
    sp.kind = 1 + (int)rm_pick(s, 3);
    sp.resume = rm_pick(s, 3) == 0;
    if (sp.kind == 1) {
        sp.L.width = widths[rm_pick(s, 5)];
        sp.L.offset = rm_pick(s, 5);
        sp.L.little_endian = (int)rm_pick(s, 2);
        sp.L.max_frame = sp.L.offset + (uint64_t)sp.L.width + rm_pick(s, 300);
        sp.L.adjustment = (int64_t)rm_pick(s, 12) - 6;
        sp.L.strip = rm_pick(s, (uint32_t)(sp.L.offset + (uint64_t)sp.L.width + 3));
        sp.L.fail_fast = (int)rm_pick(s, 2);
        sp.L.stop_on_error = rm_pick(s, 3) == 0;
        sp.L.reject_unsafe = (int)rm_pick(s, 2);
        if (rm_pick(s, 2)) {
            g_rng = ((uint64_t)rm_pick(s, 65536) << 32 | rm_pick(s, 65536)) | 1;
            n = gen_length_stream(&sp, S, sizeof S, (int)rm_pick(s, 2));
        }
    } else if (sp.kind == 2) {
        sp.nd = 1 + rm_pick(s, 4);
        for (i = 0; i < sp.nd; i++) {
            sp.dlen[i] = 1 + rm_pick(s, rm_pick(s, 2) ? 2 : 5);
            for (j = 0; j < sp.dlen[i]; j++)
                dl[i][j] = (uint8_t)alpha[rm_pick(s, 7)];
            sp.dptr[i] = dl[i];
        }
        sp.strip_delim = (int)rm_pick(s, 2);
        sp.dmax = 1 + rm_pick(s, rm_pick(s, 2) ? 8 : 60);
        sp.dff = (int)rm_pick(s, 2);
        sp.dhold = rm_pick(s, 3) == 0;
        sp.dstop = (int)rm_pick(s, 2);
        n = rm_pick(s, 300);
        for (i = 0; i < n; i++)
            S[i] = (uint8_t)alpha[rm_pick(s, rm_pick(s, 3) ? 3 : 7)];
    } else {
        sp.fixed = 1 + rm_pick(s, rm_pick(s, 2) ? 4 : 50);
    }
    if (!n) {
        n = s->n < sizeof S ? s->n : sizeof S;
        memcpy(S, s->p, n);
        s->p += n;
        s->n -= n;
    }
    for (i = 0, j = 0; i < 64; i++) {
        j += 1 + rm_pick(s, i & 1 ? 64 : 5);
        if (j >= n || !s->n)
            break;
        cuts[nc++] = j;
    }
    if (sp.kind == 1) {
        dyn_frame_length_cfg L = sp.L;
        L.resume_after_error = sp.resume;
        d = dyn_frame_new_length(&L, &err);
    } else if (sp.kind == 2) {
        dyn_frame_delim_cfg c;
        memset(&c, 0, sizeof c);
        c.delims = sp.dptr;
        c.lens = sp.dlen;
        c.count = sp.nd;
        c.max_frame = sp.dmax;
        d = dyn_frame_new_delim(&c, &err);
    } else {
        d = dyn_frame_new_fixed(sp.fixed, &err);
    }
    if (!d)
        return;
    dyn_frame_free(d);
    if (!nc || rm_pick(s, 2))
        compare_one(&sp, S, n, NULL, 0, "fuzz");
    if (nc)
        compare_one(&sp, S, n, cuts, nc, "fuzz");
    if (n <= 64 && rm_pick(s, 4) == 0) {
        for (i = 1; i < n; i++)
            g_cuts[i - 1] = i;
        compare_one(&sp, S, n, g_cuts, n ? n - 1 : 0, "fuzz-1byte");
    }
    if (g_fail != before)
        die("frame decoder differs from the Netty port or the invariant reference (FRAME_DUMP=1 for the stream)");
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    rm_src_t s = { data + 1, size ? size - 1 : 0, NULL };
    rm_nprefix = 4;
    if (size < 1 || size > 65536)
        return 0;
    switch (data[0] % 4) {
    case 0:
        fz_tree(&s);
        break;
    case 1:
        fz_raw(&s);
        break;
    case 2:
        fz_cmd(&s);
        break;
    default:
        fz_frame(&s);
        break;
    }
    return 0;
}
