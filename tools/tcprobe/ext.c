typedef __attribute__((preserve_none)) long (*tc_fn)(long a, long b);

__attribute__((noinline)) long tc_ext(long v)
{
    __asm__ volatile("" : "+r"(v));
    return v;
}

__attribute__((noinline)) long tc_enter(tc_fn h, long a, long b)
{
    return h(a, b);
}

__attribute__((preserve_none, noinline)) long tc_tail(long r, long b)
{
    __asm__ volatile("" : "+r"(r), "+r"(b));
    return r;
}
