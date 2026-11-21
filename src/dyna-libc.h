#ifndef DYNAJS_LIBC_H
#define DYNAJS_LIBC_H

#include <stdio.h>
#include <stdlib.h>

#include "dynajs.h"

#ifdef __cplusplus
extern "C" {
#endif

JSModuleDef* js_init_module_std(JSContext* ctx, const char* module_name);
JSModuleDef* js_init_module_os(JSContext* ctx, const char* module_name);
void dyn_wmsg_set_allocator(void* (*alloc_fn)(size_t),
    void (*free_fn)(void*));
void js_std_add_helpers(JSContext* ctx, int argc, char** argv);
int js_std_loop(JSContext* ctx);
void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);
void js_std_set_io_reactor_weak_rt(JSRuntime* rt, int weak);
void js_std_set_io_reactor_detach(JSContext* ctx, void (*detach)(JSRuntime* rt));
void js_std_set_job_error_rt(JSRuntime* rt, JSValue exc);
int js_std_job_error_pending_rt(JSRuntime* rt);
int js_std_take_job_error_ctx(JSContext* ctx);
void js_std_set_io_reactor_rt(JSRuntime* rt, int fd,
    void (*drain)(void* udata), void* udata);
void js_std_set_io_reactor_wait(JSContext* ctx,
    int (*wait)(void* udata, int timeout_ms));
void js_std_set_io_reactor_wait_rt(JSRuntime* rt,
    int (*wait)(void* udata, int timeout_ms));
void js_std_set_io_reactor_wait_us(JSContext* ctx,
    int (*wait)(void* udata, int64_t timeout_us));
void js_std_set_io_reactor_watch(JSContext* ctx,
    int (*watch)(void* udata, int fd, void (*ready)(void* arg), void* arg),
    int (*unwatch)(void* udata, int fd));
JSValue js_std_await(JSContext* ctx, JSValue obj);
void js_fetch_install(JSContext* ctx, JSValueConst global_obj);
JSValue js_streams_ns(JSContext* ctx);
JSValue js_dom_exception(JSContext* ctx, const char* name, const char* msg);
JSValue js_throw_dom_exception(JSContext* ctx, const char* name, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int js_is_dom_exception(JSValueConst v);
int js_streams_kind(JSValueConst x);
JSValue js_streams_reader_open(JSContext* ctx, JSValueConst rs);
JSValue js_streams_reader_read(JSContext* ctx, JSValueConst reader);
void js_streams_reader_close(JSContext* ctx, JSValueConst reader, int cancel);
JSValue js_streams_transfer_prepare(JSContext* ctx, JSValueConst msg, JSValueConst transfer);
JSValue js_streams_transfer_revive(JSContext* ctx, JSValue data);
JSValue js_dom_exception_copy(JSContext* ctx, JSValueConst v);
JSValue js_streams_call(JSContext* ctx, const char* name, int argc, JSValueConst* argv);
typedef struct JSLoopPost {
    struct JSLoopPost* next;
    void (*fn)(JSContext* ctx, void* udata);
    void* udata;
} JSLoopPost;
void* js_std_loop_hold(JSContext* ctx);
void js_std_loop_release(JSContext* ctx);
void js_std_loop_post(void* handle, JSLoopPost* node);
int js_std_loop_wait_posts(JSContext* ctx);
void js_std_set_shutdown_hook(JSContext* ctx, void (*hook)(JSRuntime* rt));
void js_std_init_handlers(JSRuntime* rt);
void js_std_free_handlers(JSRuntime* rt);
void js_std_dump_error(JSContext* ctx);
int js_std_runtime_is_main(JSRuntime* rt);
uint8_t* js_load_file(JSContext* ctx, size_t* pbuf_len, const char* filename);
int js_module_set_import_meta(JSContext* ctx, JSValueConst func_val,
    JS_BOOL use_realpath, JS_BOOL is_main);
int js_module_test_json(JSContext* ctx, JSValueConst attributes);
int js_module_check_attributes(JSContext* ctx, void* opaque, JSValueConst attributes);
JSModuleDef* js_module_loader(JSContext* ctx,
    const char* module_name, void* opaque,
    JSValueConst attributes);
int dyna_repl_eval(JSContext* ctx);
void js_std_promise_rejection_tracker(JSContext* ctx, JSValueConst promise,
    JSValueConst reason,
    JS_BOOL is_handled, void* opaque);
void js_std_set_worker_new_context_func(JSContext* (*func)(JSRuntime* rt));

#ifdef __cplusplus
}
#endif

#endif
