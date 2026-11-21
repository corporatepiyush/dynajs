#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef __attribute__((preserve_none)) long (*tc_fn)(long a, long b);
__attribute__((preserve_none)) long tc_callee(long a, long b);
long tc_enter(tc_fn h, long a, long b);
tc_fn volatile tc_target = tc_callee;

__attribute__((noinline)) static long run(int n, long k)
{
    char* buf = __builtin_alloca(n);
    _Alignas(64) volatile long big[64];
    long s;
    int i;
    memset(buf, 1, n);
    for (i = 0; i < 64; i++)
        big[i] = i;
    s = big[n & 63] - (n & 63);
    for (i = 0; i < n; i++) {
#ifdef TC_PROBE_DIRECT
        s += tc_target(buf[i], k);
#else
        s += tc_enter(tc_target, buf[i], k);
#endif
        s += buf[i];
    }
    return s;
}

int main(int argc, char** argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 16;
    long want = (long)n * 3;
    long got = run(n, 3);
    printf("%ld\n", got);
    return got == want ? 0 : 1;
}
