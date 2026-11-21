#include "sftp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SFTP) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <math.h>
#include <stdio.h>
#include <time.h>

JSValue sftp_attr_js(JSContext* ctx, const sftp_attr_t* a)
{
    JSValue o = JS_NewObject(ctx);
    uint32_t t = a->perm & S_IFMT;
    if (JS_IsException(o))
        return o;
    if (a->flags & SFTP_A_SIZE)
        JS_SetPropertyStr(ctx, o, "size", JS_NewFloat64(ctx, (double)a->size));
    if (a->flags & SFTP_A_UIDGID) {
        JS_SetPropertyStr(ctx, o, "uid", JS_NewUint32(ctx, a->uid));
        JS_SetPropertyStr(ctx, o, "gid", JS_NewUint32(ctx, a->gid));
    }
    if (a->flags & SFTP_A_PERM)
        JS_SetPropertyStr(ctx, o, "mode", JS_NewUint32(ctx, a->perm));
    if (a->flags & SFTP_A_TIME) {
        JS_SetPropertyStr(ctx, o, "atime", JS_NewUint32(ctx, a->atime));
        JS_SetPropertyStr(ctx, o, "mtime", JS_NewUint32(ctx, a->mtime));
    }
    JS_SetPropertyStr(ctx, o, "isFile", JS_NewBool(ctx, (a->flags & SFTP_A_PERM) && t == S_IFREG));
    JS_SetPropertyStr(ctx, o, "isDirectory", JS_NewBool(ctx, (a->flags & SFTP_A_PERM) && t == S_IFDIR));
    JS_SetPropertyStr(ctx, o, "isSymbolicLink", JS_NewBool(ctx, (a->flags & SFTP_A_PERM) && t == S_IFLNK));
    return o;
}

static int num_prop(JSContext* ctx, JSValueConst o, const char* k, double hi, int* has, double* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    *has = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SFTP: attrs.%s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (!(d >= 0 && d <= hi) || floor(d) != d) {
        JS_ThrowRangeError(ctx, "SFTP: attrs.%s must be an integer in 0..%.0f", k, hi);
        return -1;
    }
    *has = 1;
    *out = d;
    return 0;
}

int sftp_attr_from_js(JSContext* ctx, JSValueConst o, sftp_attr_t* a)
{
    static const char* const keys[] = { "size", "uid", "gid", "mode", "atime", "mtime" };
    int hs, hu, hg, hm, ha, ht;
    double s = 0, u = 0, g = 0, m = 0, at = 0, mt = 0;
    memset(a, 0, sizeof *a);
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "SFTP: attrs must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, o, keys, (int)countof(keys)))
        return -1;
    if (num_prop(ctx, o, "size", 9007199254740991.0, &hs, &s) || num_prop(ctx, o, "uid", 4294967295.0, &hu, &u)
        || num_prop(ctx, o, "gid", 4294967295.0, &hg, &g) || num_prop(ctx, o, "mode", 4294967295.0, &hm, &m)
        || num_prop(ctx, o, "atime", 4294967295.0, &ha, &at) || num_prop(ctx, o, "mtime", 4294967295.0, &ht, &mt))
        return -1;
    if (hu != hg || ha != ht) {
        JS_ThrowTypeError(ctx, "SFTP: attrs.uid and attrs.gid go together, and so do attrs.atime and attrs.mtime");
        return -1;
    }
    if (hs) {
        a->flags |= SFTP_A_SIZE;
        a->size = (uint64_t)s;
    }
    if (hu) {
        a->flags |= SFTP_A_UIDGID;
        a->uid = (uint32_t)u;
        a->gid = (uint32_t)g;
    }
    if (hm) {
        a->flags |= SFTP_A_PERM;
        a->perm = (uint32_t)m;
    }
    if (ha) {
        a->flags |= SFTP_A_TIME;
        a->atime = (uint32_t)at;
        a->mtime = (uint32_t)mt;
    }
    return 0;
}

JSValue sftp_error(JSContext* ctx, uint32_t code, const char* msg, size_t n)
{
    char buf[400];
    JSValue e = JS_NewError(ctx);
    snprintf(buf, sizeof buf, "SFTP: %.*s", (int)(n > 300 ? 300 : n), msg);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, buf), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, sftp_status_name(code)), JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "status", JS_NewUint32(ctx, code), JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return e;
}

int sftp_send(ssh_chan_t* ch, sshb_t* b)
{
    if (!ch->conn || ch->sent_eof || ch->sent_close)
        return -1;
    sftp_pkt_end(b);
    if (sshb_put(&ch->inq, b->p, b->len))
        return -1;
    ch->in_total += b->len;
    return 0;
}

static int sftp_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (sftp_client_init(ctx, m) < 0 || sftp_server_init(ctx, m) < 0)
        return -1;
    return 0;
}

int js_nat_init_sftp(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:sftp", sftp_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "SFTPClient");
    JS_AddModuleExport(ctx, m, "SFTPServer");
    return 0;
}

#endif
