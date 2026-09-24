// Fuzz target for the SCRAM client (src/core/dyn-scram.c).
//
// This is the earliest untrusted parser in the tree: the bytes are the
// SERVER's challenge during PostgreSQL/Redis authentication, so they arrive
// from whatever endpoint was dialled and they are parsed BEFORE authentication
// completes. A hostile or compromised server is the whole threat model, and
// codegraph's untrusted-unfuzzed query ranked dyn_scram_server_first top of
// the tree -- cyclomatic 36, 26 memory operations, no target reaching it.
//
// Two steps are driven, because they fail differently:
//
//   - server-first parses r=/s=/i= and does the base64 decode, the salt copy
//     and the iteration-count arithmetic. Every server-chosen bound lives here.
//   - server-final parses v=/e= and compares the server signature. Reaching it
//     requires a server-first that PARSED, so the target feeds a repaired
//     message built around the fuzzer's bytes -- without that, this half would
//     never execute and the target would measure the r= check and nothing
//     else (the same trap recorded for the LZ4 frame path).
//
// THE INPUT IS ALLOCATED EXACTLY. Copying into a fixed buffer would put a read
// one past the logical length inside that buffer's spare capacity, where no
// sanitizer can see it -- which is the bug class this target exists for
// (CLAUDE.md section 5).
//
//     make fuzz_scram

#include "dyn-scram.h"
#include "dyna-simd-kernels.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Big enough for a client-final at any nonce the parser will accept; the
   return value is checked, so a short buffer would be a refusal, not a write. */
#define OUTCAP 1024

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    dyn_scram_t s;
    char out[OUTCAP];
    uint8_t *exact;
    int rc;

    /* base64 goes through the SIMD dispatch table, which the engine fills at
       module load. A standalone target never does, so the first call went
       through a NULL slot and segfaulted before any input was read -- the
       uninitialised-dispatch-table hazard, hit for real. */
    simd_init();

    if (size > 65536)
        return 0;

    /* Exactly-sized: one byte over the end is a heap overflow, reported. */
    exact = (uint8_t *)malloc(size ? size : 1);
    if (!exact)
        return 0;
    memcpy(exact, data, size);

    /* 1. Raw bytes straight at server-first, from a properly started
          exchange -- client_first seeds the nonce that r= is checked against. */
    memset(&s, 0, sizeof s);
    if (dyn_scram_client_first(&s, out, sizeof out) > 0) {
        rc = dyn_scram_server_first(&s, (const char *)exact, size,
                                    "hunter2", out, sizeof out);
        /* If it parsed, the exchange is in a state where server-final is
           reachable; feed it the same bytes. */
        if (rc > 0)
            dyn_scram_server_final(&s, (const char *)exact, size);
    }
    dyn_scram_free(&s);

    /* 2. A REPAIRED server-first: a well-formed frame with the fuzzer's bytes
          as the salt, so the base64 decode and the salt copy are reached on
          essentially every input rather than only on ones that happen to be
          valid. The nonce must start with ours or r= rejects immediately. */
    memset(&s, 0, sizeof s);
    if (dyn_scram_client_first(&s, out, sizeof out) > 0) {
        size_t cap = size * 2 + sizeof s.nonce + 128;
        char *msg = (char *)malloc(cap);
        if (msg) {
            size_t n = 0, i;
            n += (size_t)snprintf(msg + n, cap - n, "r=%s", s.nonce);
            /* the fuzzer's bytes tail the nonce, then a salt and an i= */
            for (i = 0; i < size && n + 1 < cap; i++) {
                char c = (char)exact[i];
                if (c == ',' || c == '\0')
                    c = 'x';                /* keep the field boundary intact */
                msg[n++] = c;
            }
            n += (size_t)snprintf(msg + n, cap - n, ",s=");
            for (i = 0; i < size && n + 1 < cap; i++)
                msg[n++] = (char)exact[i];
            n += (size_t)snprintf(msg + n, cap - n, ",i=4096");
            rc = dyn_scram_server_first(&s, msg, n, "hunter2", out, sizeof out);
            if (rc > 0)
                dyn_scram_server_final(&s, msg, n);
            free(msg);
        }
    }
    dyn_scram_free(&s);

    free(exact);
    return 0;
}
