#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#define PGQ_ST_BEGIN 1
#define PGQ_ST_DECLARE 2
#define PGQ_ST_FETCH 3
#define PGQ_ST_CLEANUP_CLOSE 4
#define PGQ_ST_CLEANUP_TX 5
#define PGQ_ST_DONE 6

void pgq_iter_finalizer(JSRuntime* rt, JSValue val)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(val, pgq_class_id);
    if (!it)
        return;
    JS_FreeValueRT(rt, it->conn_obj);
    JS_FreeValueRT(rt, it->query_fn);
    JS_FreeValueRT(rt, it->params);
    JS_FreeValueRT(rt, it->rows);
    JS_FreeValueRT(rt, it->error);
    free(it->sql);
    free(it);
}

void pgq_iter_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(val, pgq_class_id);
    if (!it)
        return;
    JS_MarkValue(rt, it->conn_obj, mark);
    JS_MarkValue(rt, it->query_fn, mark);
    JS_MarkValue(rt, it->params, mark);
    JS_MarkValue(rt, it->rows, mark);
    JS_MarkValue(rt, it->error, mark);
}

static JSValue pgq_query(JSContext* ctx, pgq_iter_t* it, const char* sql,
    JSValueConst params)
{
    JSValue s, ret;
    JSValueConst argv[2];
    int argc = 1;

    s = JS_NewString(ctx, sql);
    if (JS_IsException(s))
        return s;
    argv[0] = s;
    if (!JS_IsUndefined(params)) {
        argv[1] = params;
        argc = 2;
    }
    ret = JS_Call(ctx, it->query_fn, it->conn_obj, argc, argv);
    JS_FreeValue(ctx, s);
    return ret;
}

static pgq_op_t* pgq_shift(JSContext* ctx, pgq_iter_t* it)
{
    pgq_op_t* op = it->qhead;
    (void)ctx;
    if (!op)
        return NULL;
    it->qhead = op->qnext;
    if (!it->qhead)
        it->qtail = NULL;
    op->qnext = NULL;
    return op;
}

static void pgq_park(pgq_iter_t* it, pgq_op_t* op)
{
    op->qnext = NULL;
    if (it->qtail)
        it->qtail->qnext = op;
    else
        it->qhead = op;
    it->qtail = op;
}

static void pgq_pump(JSContext* ctx, pgq_iter_t* it);

static void pgq_op_unwind(JSContext* ctx, pgq_iter_t* it, pgq_op_t* op)
{
    it->running = 0;
    pg_cps_defuse_all(ctx, &op->cps);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    pgq_pump(ctx, it);
    JS_FreeValue(ctx, op->it_obj);
    free(op);
}

static void pgq_op_bare_free(JSContext* ctx, pgq_op_t* op)
{
    pg_cps_defuse_all(ctx, &op->cps);
    JS_FreeValue(ctx, op->it_obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

static JSValue pgq_buffered_row(pgq_iter_t* it)
{
    JSContext* ctx = it->ctx;
    JSValue row;

    if (JS_IsUndefined(it->rows) || it->row_index >= it->row_count)
        return JS_UNDEFINED;
    row = JS_GetPropertyUint32(ctx, it->rows, it->row_index);
    it->row_index++;
    if (it->row_index >= it->row_count) {
        JS_FreeValue(ctx, it->rows);
        it->rows = JS_UNDEFINED;
        it->row_index = it->row_count = 0;
    }
    return row;
}

static void pgq_op_settle(JSContext* ctx, pgq_op_t* op, JSValue value,
    int failed)
{
    JSValue fn = failed ? op->jreject : op->jresolve, r;

    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { value };
        r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        if (JS_IsException(r))
            dyn_exc_drop(ctx);
        else
            JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, value);
}

static void pgq_op_emit(JSContext* ctx, pgq_op_t* op, JSValue value)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result)) {
        JS_FreeValue(ctx, value);
        dyn_exc_drop(ctx);
        result = JS_GetException(ctx);
        pgq_op_settle(ctx, op, result, 1);
        return;
    }
    JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done", JS_FALSE, JS_PROP_C_W_E);
    pgq_op_settle(ctx, op, result, 0);
}

static void pgq_op_done(JSContext* ctx, pgq_op_t* op)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result)) {
        dyn_exc_drop(ctx);
        pgq_op_settle(ctx, op, JS_UNDEFINED, 1);
        return;
    }
    JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
    pgq_op_settle(ctx, op, result, 0);
}

static void pgq_fail(JSContext* ctx, pgq_iter_t* it, JSValueConst val)
{
    if (JS_IsUndefined(it->error))
        it->error = JS_DupValue(ctx, val);
    it->state = PGQ_ST_CLEANUP_CLOSE;
}

static void pgq_step(JSContext* ctx, pg_cps_t* cp, JSValueConst val,
    int failed);

static void pgq_advance(JSContext* ctx, pgq_iter_t* it, pgq_op_t* op)
{
    char* sql;
    size_t n;
    JSValue p;

    if (op->want_return && (it->state == PGQ_ST_FETCH || it->state == PGQ_ST_IDLE)) {
        JS_FreeValue(ctx, it->rows);
        it->rows = JS_UNDEFINED;
        it->row_index = it->row_count = 0;
        it->state = (it->declared || it->began) ? PGQ_ST_CLEANUP_CLOSE
                                                : PGQ_ST_DONE;
    }

    switch (it->state) {
    case PGQ_ST_DONE:
        if (!JS_IsUndefined(it->error) && !op->want_return) {
            JSValueConst a[1] = { it->error };
            JSValue r = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1, a);
            if (JS_IsException(r))
                dyn_exc_drop(ctx);
            else
                JS_FreeValue(ctx, r);
        } else {
            pgq_op_done(ctx, op);
        }
        pgq_op_unwind(ctx, it, op);
        return;

    case PGQ_ST_IDLE:
        op->mode = PGQ_ST_BEGIN;
        p = pgq_query(ctx, it, "BEGIN", JS_UNDEFINED);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_DECLARE:
        op->mode = PGQ_ST_DECLARE;
        n = strlen(it->name) + strlen(it->sql) + 32;
        sql = (char*)malloc(n);
        if (!sql) {
            JSValue exc = JS_ThrowOutOfMemory(ctx);
            pgq_fail(ctx, it, exc);
            JS_FreeValue(ctx, exc);
            pgq_advance(ctx, it, op);
            return;
        }
        snprintf(sql, n, "DECLARE %s CURSOR FOR %s", it->name, it->sql);
        p = pgq_query(ctx, it, sql, it->params);
        free(sql);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_FETCH:
        if (!JS_IsUndefined(it->rows) && it->row_index < it->row_count) {
            JSValue row = pgq_buffered_row(it);
            it->emitted++;
            if (it->max_rows > 0 && it->emitted >= it->max_rows) {
                JS_FreeValue(ctx, it->rows);
                it->rows = JS_UNDEFINED;
                it->row_index = it->row_count = 0;
                it->state = PGQ_ST_CLEANUP_CLOSE;
            }
            pgq_op_emit(ctx, op, row);
            pgq_op_unwind(ctx, it, op);
            return;
        }
        op->mode = PGQ_ST_FETCH;
        n = (size_t)snprintf(NULL, 0, "FETCH FORWARD %lld FROM %s",
                (long long)it->batch, it->name)
            + 1;
        sql = (char*)malloc(n);
        if (!sql) {
            JSValue exc = JS_ThrowOutOfMemory(ctx);
            pgq_fail(ctx, it, exc);
            JS_FreeValue(ctx, exc);
            pgq_advance(ctx, it, op);
            return;
        }
        snprintf(sql, n, "FETCH FORWARD %lld FROM %s",
            (long long)it->batch, it->name);
        p = pgq_query(ctx, it, sql, JS_UNDEFINED);
        free(sql);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_CLEANUP_CLOSE:
        if (!it->declared) {
            it->state = PGQ_ST_CLEANUP_TX;
            pgq_advance(ctx, it, op);
            return;
        }
        op->mode = PGQ_ST_CLEANUP_CLOSE;
        n = strlen(it->name) + 16;
        sql = (char*)malloc(n);
        if (!sql) {
            it->state = PGQ_ST_CLEANUP_TX;
            pgq_advance(ctx, it, op);
            return;
        }
        snprintf(sql, n, "CLOSE %s", it->name);
        p = pgq_query(ctx, it, sql, JS_UNDEFINED);
        free(sql);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_CLEANUP_TX:
        op->mode = PGQ_ST_CLEANUP_TX;
        p = pgq_query(ctx, it, JS_IsUndefined(it->error) ? "COMMIT" : "ROLLBACK",
            JS_UNDEFINED);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    default:
        pgq_op_done(ctx, op);
        pgq_op_unwind(ctx, it, op);
        return;
    }
}

static void pgq_step(JSContext* ctx, pg_cps_t* cp, JSValueConst val,
    int failed)
{
    pgq_op_t* op = (pgq_op_t*)cp;
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(op->it_obj, pgq_class_id);

    if (!it) {
        pgq_op_bare_free(ctx, op);
        return;
    }
    if (failed) {
        switch (op->mode) {
        case PGQ_ST_CLEANUP_CLOSE:
            it->state = PGQ_ST_CLEANUP_TX;
            pgq_advance(ctx, it, op);
            return;
        case PGQ_ST_CLEANUP_TX:
            it->state = PGQ_ST_DONE;
            pgq_advance(ctx, it, op);
            return;
        default:
            pgq_fail(ctx, it, val);
            pgq_advance(ctx, it, op);
            return;
        }
    }

    switch (op->mode) {
    case PGQ_ST_BEGIN:
        it->began = 1;
        it->state = PGQ_ST_DECLARE;
        pgq_advance(ctx, it, op);
        return;

    case PGQ_ST_DECLARE:
        it->declared = 1;
        it->state = PGQ_ST_FETCH;
        pgq_advance(ctx, it, op);
        return;

    case PGQ_ST_FETCH: {
        JSValue rows, row;
        uint32_t n = 0;
        JSValue lv;

        rows = JS_IsObject(val)
            ? JS_GetPropertyStr(ctx, val, "rows")
            : JS_UNDEFINED;
        if (JS_IsException(rows)) {
            dyn_exc_drop(ctx);
            rows = JS_UNDEFINED;
        }
        if (!JS_IsUndefined(rows)) {
            lv = JS_GetPropertyStr(ctx, rows, "length");
            if (JS_ToUint32(ctx, &n, lv)) {
                JS_FreeValue(ctx, lv);
                JS_FreeValue(ctx, rows);
                rows = JS_UNDEFINED;
                n = 0;
            } else {
                JS_FreeValue(ctx, lv);
            }
        }
        if (JS_IsUndefined(rows) || n == 0) {
            JS_FreeValue(ctx, rows);
            it->state = PGQ_ST_CLEANUP_CLOSE;
            pgq_advance(ctx, it, op);
            return;
        }
        JS_FreeValue(ctx, it->rows);
        it->rows = rows;
        it->row_index = 0;
        it->row_count = n;
        if (op->want_return) {
            JS_FreeValue(ctx, it->rows);
            it->rows = JS_UNDEFINED;
            it->row_index = it->row_count = 0;
            it->state = PGQ_ST_CLEANUP_CLOSE;
            pgq_advance(ctx, it, op);
            return;
        }
        row = pgq_buffered_row(it);
        it->emitted++;
        if (it->max_rows > 0 && it->emitted >= it->max_rows) {
            JS_FreeValue(ctx, it->rows);
            it->rows = JS_UNDEFINED;
            it->row_index = it->row_count = 0;
            it->state = PGQ_ST_CLEANUP_CLOSE;
        }
        pgq_op_emit(ctx, op, row);
        pgq_op_unwind(ctx, it, op);
        return;
    }

    case PGQ_ST_CLEANUP_CLOSE:
        it->state = PGQ_ST_CLEANUP_TX;
        pgq_advance(ctx, it, op);
        return;

    case PGQ_ST_CLEANUP_TX:
        it->state = PGQ_ST_DONE;
        pgq_advance(ctx, it, op);
        return;

    default:
        pgq_op_unwind(ctx, it, op);
        return;
    }
}

static void pgq_pump(JSContext* ctx, pgq_iter_t* it)
{
    if (it->in_pump)
        return;
    it->in_pump = 1;
    while (!it->running) {
        pgq_op_t* op = pgq_shift(ctx, it);
        if (!op)
            break;
        it->running = 1;
        pgq_advance(ctx, it, op);
    }
    it->in_pump = 0;
}

JSValue pgq_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(this_val, pgq_class_id);
    JSValue funcs[2], promise, row;
    pgq_op_t* op;
    int pend;

    (void)argc;
    (void)argv;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    if (!it) {
        JSValue e;
        JS_ThrowTypeError(ctx, "not a PgRowStream");
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            return JS_EXCEPTION;
        }
        e = JS_GetException(ctx);
        JS_FreeValue(ctx, JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&e));
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (it->state == PGQ_ST_DONE || !JS_IsUndefined(it->error)) {
        if (JS_IsUndefined(it->error)) {
            JSValueConst a[1];
            JSValue result = JS_NewObject(ctx), r;
            JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE,
                JS_PROP_C_W_E);
            a[0] = result;
            r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, a);
            JS_FreeValue(ctx, result);
            if (JS_IsException(r))
                dyn_exc_drop(ctx);
            else
                JS_FreeValue(ctx, r);
        } else {
            JSValueConst a[1] = { it->error };
            JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, a);
            if (JS_IsException(r))
                dyn_exc_drop(ctx);
            else
                JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (!it->running && !it->qhead && it->state == PGQ_ST_FETCH && !JS_IsUndefined(it->rows) && it->row_index < it->row_count) {
        row = pgq_buffered_row(it);
        it->emitted++;
        if (it->max_rows > 0 && it->emitted >= it->max_rows) {
            JS_FreeValue(ctx, it->rows);
            it->rows = JS_UNDEFINED;
            it->row_index = it->row_count = 0;
            it->state = PGQ_ST_CLEANUP_CLOSE;
        }
        {
            JSValueConst a[1];
            JSValue result = JS_NewObject(ctx), r;
            JS_DefinePropertyValueStr(ctx, result, "value", row,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, result, "done", JS_FALSE,
                JS_PROP_C_W_E);
            a[0] = result;
            r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, a);
            JS_FreeValue(ctx, result);
            if (JS_IsException(r))
                dyn_exc_drop(ctx);
            else
                JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    op = (pgq_op_t*)calloc(1, sizeof(*op));
    if (!op) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->it_obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    pend = js_std_job_error_pending_rt(JS_GetRuntime(ctx));
    pgq_park(it, op);
    pgq_pump(ctx, it);
    if (dyn_exc_reclaim(ctx, pend)) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    return promise;
}

JSValue pgq_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(this_val, pgq_class_id);
    JSValue funcs[2], promise;
    pgq_op_t* op;
    int pend;

    (void)argc;
    (void)argv;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    if (!it || it->state == PGQ_ST_DONE) {
        JSValueConst a[1];
        JSValue result = JS_NewObject(ctx), r;
        if (JS_IsException(result)) {
            dyn_exc_drop(ctx);
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return promise;
        }
        JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
        a[0] = result;
        r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, result);
        if (JS_IsException(r))
            dyn_exc_drop(ctx);
        else
            JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    op = (pgq_op_t*)calloc(1, sizeof(*op));
    if (!op) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->it_obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    op->want_return = 1;
    pend = js_std_job_error_pending_rt(JS_GetRuntime(ctx));
    pgq_park(it, op);
    pgq_pump(ctx, it);
    if (dyn_exc_reclaim(ctx, pend)) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    return promise;
}

JSValue pgq_throw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return pgq_return(ctx, this_val, 0, NULL);
}

JSValue pgq_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}
#endif
