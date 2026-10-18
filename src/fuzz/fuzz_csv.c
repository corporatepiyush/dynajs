#include "dyna-csv.c"

#include <stdint.h>

void simd_init(void);

static int inited;

static void walk(const Table* t)
{
    size_t r, c;
    volatile size_t sink = 0;
    for (r = 0; r < t->n; r++)
        for (c = 0; c < t->r[r].n; c++)
            if (t->r[r].f[c])
                sink += strlen(t->r[r].f[c]);
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Table t, t2;
    Buf out;

    if (!inited) {
        simd_init();
        inited = 1;
    }
    if (size > 1u << 20)
        return 0;

    if (csv_parse(data, size, &t) < 0)
        return 0;
    walk(&t);

    memset(&out, 0, sizeof(out));
    if (csv_serialize(&t, &out) == 0) {
        if (csv_parse((const uint8_t*)(out.p ? out.p : ""), out.len, &t2) == 0) {
            size_t r;
            if (t2.n != t.n)
                abort();
            for (r = 0; r < t.n; r++)
                if (t2.r[r].n != t.r[r].n)
                    abort();
            walk(&t2);
            table_free(&t2);
        }
    }
    buf_free(&out);
    table_free(&t);
    return 0;
}
