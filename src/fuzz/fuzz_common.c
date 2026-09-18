/* Copyright 2020 Google Inc.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

#include <string.h>

#include "src/fuzz/fuzz_common.h"

// handle timeouts from infinite loops
static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    nbinterrupts++;
    return (nbinterrupts > 100);
}

void reset_nbinterrupts() {
    nbinterrupts = 0;
}

/* Pump the pending job queue without js_std_loop()'s promise-rejection check
   (dyna-libc.c:5038-5052 exit(1)s on an unhandled rejection; fuzz input must
   not be able to kill the run). No poll: with std/os gone there is nothing to
   wait on, and a fuzz input must not park. Mirrors js_std_loop()'s inner loop. */
void fuzz_loop(JSContext *ctx)
{
    int err;
    for (;;) {
        err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
        if (err <= 0) {
            if (err < 0)
                js_std_dump_error(ctx);
            break;
        }
    }
}

void test_one_input_init(JSRuntime *rt, JSContext *ctx) {
    // 64 Mo
    JS_SetMemoryLimit(rt, 0x4000000);
    // 64 Kb
    JS_SetMaxStackSize(rt, 0x10000);

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);
    JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);

    // No std/os in fuzz contexts (audit 13.4.1): input must not be able to
    // std.exit(0) or drive host file I/O. js_std_init_handlers() stays -- it
    // owns the timer/IO handler lists the helpers need, and free_handlers
    // assumes it ran.
    js_std_init_handlers(rt);
}
