long tc_ext(long v);
__attribute__((preserve_none)) long tc_tail(long r, long b);

__attribute__((preserve_none, noinline)) long tc_callee(long a, long b)
{
    long x0 = tc_ext(a);
    long x1 = tc_ext(b);
    long x2 = tc_ext(a + 1);
    long x3 = tc_ext(b + 1);
    long x4 = tc_ext(a + 2);
    long x5 = tc_ext(b + 2);
    long x6 = tc_ext(a + 3);
    long x7 = tc_ext(b + 3);
    long x8 = tc_ext(a + 4);
    long x9 = tc_ext(b + 4);
    __attribute__((musttail)) return tc_tail(x0 + x1 - x2 - x3 + x4 + x5 - x6 - x7 + x8 + x9 - b - b - b + 3, b);
}
