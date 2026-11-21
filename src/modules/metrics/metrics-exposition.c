#include "metrics-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_METRICS)

int met_labels(JSContext* ctx, JSValueConst v, char* out, size_t cap)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, k;
    size_t off = 0;
    int rc = 0;

    out[0] = '\0';
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "Metrics: labels must be an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        const char* key = JS_AtomToCString(ctx, tab[k].atom);
        JSValue val;
        const char* sv;
        int nw;
        if (!key) {
            rc = -1;
            break;
        }
        {
            size_t ki;
            int bad = key[0] == '\0';
            for (ki = 0; key[ki]; ki++) {
                char c = key[ki];
                int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || c == '_';
                if (!(alpha || (ki > 0 && c >= '0' && c <= '9'))) {
                    bad = 1;
                    break;
                }
            }
            if (bad) {
                JS_ThrowTypeError(ctx, "Metrics: label name '%s' is not "
                                       "[a-zA-Z_][a-zA-Z0-9_]*",
                    key);
                JS_FreeCString(ctx, key);
                rc = -1;
                break;
            }
        }
        val = JS_GetProperty(ctx, v, tab[k].atom);
        if (JS_IsException(val)) {
            JS_FreeCString(ctx, key);
            rc = -1;
            break;
        }
        sv = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);
        if (!sv) {
            JS_FreeCString(ctx, key);
            rc = -1;
            break;
        }
        nw = snprintf(out + off, cap - off, "%s%s=\"", off ? "," : "", key);
        if (nw < 0 || (size_t)nw >= cap - off)
            rc = -1;
        else {
            size_t i;
            off += (size_t)nw;
            for (i = 0; sv[i] && rc == 0; i++) {
                char c = sv[i];
                if (off + 3 >= cap) {
                    rc = -1;
                    break;
                }
                if (c == '"' || c == '\\' || c == '\n') {
                    out[off++] = '\\';
                    out[off++] = c == '\n' ? 'n' : c;
                } else {
                    out[off++] = c;
                }
            }
            if (rc == 0) {
                if (off + 2 >= cap)
                    rc = -1;
                else
                    out[off++] = '"';
            }
            out[off] = '\0';
        }
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, sv);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (rc < 0 && !JS_HasException(ctx))
        JS_ThrowRangeError(ctx, "Metrics: the label set is too long (max %d "
                                "bytes rendered)",
            MET_LABELS - 1);
    return rc;
}

static int met_append(char** buf, size_t* len, size_t* cap, const char* s)
{
    size_t n = strlen(s);

    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        char* np;
        while (nc < *len + n + 1)
            nc *= 2;
        np = (char*)realloc(*buf, nc);
        if (!np)
            return -1;
        *buf = np;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

char* met_scrape_buf(size_t* out_len)
{
    char *buf = NULL, line[512];
    size_t len = 0, cap = 0;
    uint32_t i, n;

    pthread_mutex_lock(&met_lock);
    n = atomic_load_explicit(&met_used, memory_order_relaxed);
    for (i = 0; i < n; i++) {
        const met_cold_t* c = &met_cold[i];
        const char* lb = c->labels;
        char open_[2] = "{", close_[2] = "}";
        if (!lb[0]) {
            open_[0] = '\0';
            close_[0] = '\0';
        }
        int k;
        if (c->kind == MET_FREE)
            continue;
        snprintf(line, sizeof line, "# TYPE %s %s\n", c->name,
            c->kind == MET_COUNTER     ? "counter"
                : c->kind == MET_GAUGE ? "gauge"
                                       : "histogram");
        if (met_append(&buf, &len, &cap, line) < 0)
            goto oom;
        if (c->kind == MET_COUNTER) {
            snprintf(line, sizeof line, "%s%s%s%s %llu\n", c->name,
                open_, lb, close_,
                (unsigned long long)atomic_load_explicit(
                    &met_hot[i].v, memory_order_relaxed));
        } else if (c->kind == MET_GAUGE) {
            double gv = met_utod(atomic_load_explicit(&met_hot[i].v,
                memory_order_relaxed));
            uint64_t gb = met_dtou(gv);
            if ((gb & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
                snprintf(line, sizeof line, "%s%s%s%s %s\n", c->name,
                    open_, lb, close_,
                    (gb & 0xFFFFFFFFFFFFFULL) ? "NaN"
                        : (gb >> 63)          ? "-Inf"
                                              : "+Inf");
            } else {
                snprintf(line, sizeof line, "%s%s%s%s %.17g\n", c->name,
                    open_, lb, close_, gv);
            }
        } else {
            uint64_t cum = 0, total;
            uint8_t nb = c->n_bounds ? c->n_bounds : (uint8_t)MET_NBUCKET;
            for (k = 0; k < nb; k++) {
                cum = atomic_load_explicit(&met_hot[i].bucket[k],
                    memory_order_relaxed);
                snprintf(line, sizeof line, "%s_bucket{%s%sle=\"%g\"} %llu\n",
                    c->name, lb, lb[0] ? "," : "", c->bounds[k],
                    (unsigned long long)cum);
                if (met_append(&buf, &len, &cap, line) < 0)
                    goto oom;
            }
            total = atomic_load_explicit(&met_hot[i].v, memory_order_relaxed);
            snprintf(line, sizeof line, "%s_bucket{%s%sle=\"+Inf\"} %llu\n",
                c->name, lb, lb[0] ? "," : "", (unsigned long long)total);
            if (met_append(&buf, &len, &cap, line) < 0)
                goto oom;
            snprintf(line, sizeof line, "%s_sum%s%s%s %.17g\n",
                c->name, open_, lb, close_,
                met_utod(atomic_load_explicit(&met_hot[i].sum_bits,
                    memory_order_relaxed)));
            if (met_append(&buf, &len, &cap, line) < 0)
                goto oom;
            snprintf(line, sizeof line, "%s_count%s%s%s %llu\n",
                c->name, open_, lb, close_, (unsigned long long)total);
        }
        if (met_append(&buf, &len, &cap, line) < 0)
            goto oom;
    }
    if (!buf) {
        buf = (char*)malloc(1);
        if (buf)
            buf[0] = '\0';
    }
    pthread_mutex_unlock(&met_lock);
    *out_len = len;
    return buf;
oom:
    pthread_mutex_unlock(&met_lock);
    free(buf);
    return NULL;
}

JSValue dyn_met_scrape(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t len = 0;
    char* buf;
    JSValue r;
    (void)this_val;
    (void)argc;
    (void)argv;

    buf = met_scrape_buf(&len);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    r = JS_NewStringLen(ctx, buf, len);
    free(buf);
    return r;
}

#endif
