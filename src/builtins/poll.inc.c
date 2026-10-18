#ifndef DYN_BUILTINS_POLL_INC_C
#define DYN_BUILTINS_POLL_INC_C

#define DYN_POLL_INTERVAL 4096
#define DYN_POLL_MASK (DYN_POLL_INTERVAL - 1)

static int dyn_poll_interrupts(JSContext* ctx, int64_t i)
{
    if (likely((i & DYN_POLL_MASK) != 0))
        return 0;
    ctx->interrupt_counter = 1;
    return js_poll_interrupts(ctx);
}

#endif
