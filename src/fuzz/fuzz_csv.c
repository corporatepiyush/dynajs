// Fuzz target for the RFC-4180 CSV parser (src/dyna-csv.c, csv_parse).
//
// WHY THIS INCLUDES THE .c INSTEAD OF CALLING A PUBLIC ENTRY POINT -- do not
// "fix" this into a link error: csv_parse() is `static`, and the only path to
// it is csv_load(), which takes a FILE PATH. Calling the JS
// API would mean writing a temp file per execution, which is both slow and a
// different test -- it would measure the loader, not the parser. Including the
// translation unit puts the fuzzer's bytes straight into csv_parse with an
// exact length and no I/O at all.
//
// simd_init() FIRST. csv_parse calls simd.count_u8 and simd.find_first_of
// through a dispatch table the engine fills at module load; a standalone target
// never does, so the first call goes through a NULL slot and segfaults before a
// byte is read -- looking like "crashes on every input" rather than a missing
// initialiser. See fuzz_scram.c:46-50, where this was hit for real.
//
// INPUT: raw pointer, NO +1 NUL slack. csv_parse takes (const uint8_t *, size_t)
// and honours the length, unlike JS_Eval/JS_ParseJSON in fuzz_eval/fuzz_json
// which require buf[len] == '\0'. libFuzzer's buffer is exactly `size` bytes,
// so a read one past the end is a reported heap overflow.

#include "dyna-csv.c"

#include <stdint.h>

void simd_init(void);

static int inited;

/* Every cell is either NULL (the empty string) or an owned NUL-terminated
   string inside the table. Touching each one is what turns a silently wrong
   length into an ASan report rather than a wrong answer nobody reads. */
static void walk(const Table *t)
{
    size_t r, c;
    volatile size_t sink = 0;
    for (r = 0; r < t->n; r++)
        for (c = 0; c < t->r[r].n; c++)
            if (t->r[r].f[c])
                sink += strlen(t->r[r].f[c]);
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    Table t, t2;
    Buf out;

    if (!inited) { simd_init(); inited = 1; }
    if (size > 1u << 20)
        return 0;

    if (csv_parse(data, size, &t) < 0)
        return 0;                      /* OOM only; t is already freed */
    walk(&t);

    /* Round trip: serialize the table and re-parse it. The re-parse must agree
       on the shape -- a quoting bug that loses a field boundary shows up here
       as a column count that moved, which no memory checker would report. */
    memset(&out, 0, sizeof(out));
    if (csv_serialize(&t, &out) == 0) {
        if (csv_parse((const uint8_t *)(out.p ? out.p : ""), out.len, &t2) == 0) {
            size_t r;
            if (t2.n != t.n)
                abort();               /* re-parse lost or invented a row */
            for (r = 0; r < t.n; r++)
                if (t2.r[r].n != t.r[r].n)
                    abort();           /* ... or a field within a row */
            walk(&t2);
            table_free(&t2);
        }
    }
    buf_free(&out);
    table_free(&t);
    return 0;
}
