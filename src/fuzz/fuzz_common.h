#include "dynajs.h"
#include "dyna-libc.h"

#include <stddef.h>
#include <stdint.h>

static int nbinterrupts = 0;

void reset_nbinterrupts(void);
void test_one_input_init(JSRuntime* rt, JSContext* ctx);
void fuzz_loop(JSContext* ctx);
void fuzz_drain_or_abort(JSContext* ctx, JSValueConst v);

#define FZ_BATCH_MAX 256

typedef struct fz_env fz_env_t;
struct fz_env {
    JSRuntime* rt;
    JSContext* ctx[2];
    int nctx;
    unsigned used;
    unsigned recycle;
    unsigned batches;
    int64_t base;
    int dirty;
    void (*init)(JSContext* ctx);
    void (*run)(fz_env_t* e, const uint8_t* data, size_t size);
    uint8_t* bq[FZ_BATCH_MAX];
    size_t bl[FZ_BATCH_MAX];
    unsigned nb;
    JSRuntime* leaked_rt[8];
    unsigned nleaked;
};

typedef struct fz_in {
    const uint8_t* p;
    size_t n;
} fz_in_t;

void fz_args(int argc, char** argv);
void fz_env_open(fz_env_t* e, int nctx, unsigned recycle, void (*init)(JSContext*), void (*run)(fz_env_t*, const uint8_t*, size_t));
void fz_one(fz_env_t* e, const uint8_t* data, size_t size);
void fz_env_recycle(fz_env_t* e);
void fz_arm(uint64_t budget_ns, unsigned max_ticks);
int fz_fired(void);
void fz_settle(JSContext* ctx, JSValue v, const char* what);
int fz_is_budget_error(JSContext* ctx, JSValueConst exc);
void fz_fail(const char* what);
void fz_drain(JSContext* ctx);
unsigned fz_u8(fz_in_t* in);
unsigned fz_below(fz_in_t* in, unsigned n);
int fz_print(void);
