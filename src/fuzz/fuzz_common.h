#include "dynajs.h"
#include "dyna-libc.h"

static int nbinterrupts = 0;

void reset_nbinterrupts();
void test_one_input_init(JSRuntime* rt, JSContext* ctx);
void fuzz_loop(JSContext* ctx);
