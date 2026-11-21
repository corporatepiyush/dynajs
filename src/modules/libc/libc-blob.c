#include "libc-internal.h"
#include "libc-wst.h"
#include "core/dyn-prng.h"
#include "dyna-nat.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define BLOB_SHARE_MIN 1024
#define BLOB_CHUNK 65536

typedef struct BStore {
    int ref;
    int file;
    size_t len;
    uint8_t data[];
} BStore;

#if !defined(_WIN32)
typedef struct BFile {
    dev_t dev;
    ino_t ino;
    int64_t mt;
    int fd;
    char path[];
} BFile;

enum { BS_FD_CAP = 64, BS_BLK = 65536, BS_SMALL = 4096 };
static int bs_fd_open;
static _Thread_local uint8_t* bs_blk;
static _Thread_local const BStore* bs_blk_s;
static _Thread_local size_t bs_blk_off, bs_files;

static int64_t st_mtime_ns(const struct stat* st)
{
#if defined(__APPLE__)
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000 + st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
#endif
}

static int bs_same(const BFile* f, const struct stat* st, size_t len)
{
    return st->st_dev == f->dev && st->st_ino == f->ino && (uint64_t)st->st_size == (uint64_t)len && st_mtime_ns(st) == f->mt;
}
#endif

static inline uint8_t* bs_ptr(BStore* s)
{
    return (uint8_t*)s->data;
}

static int bs_read(JSContext* ctx, BStore* s, size_t off, uint8_t* dst, size_t n)
{
#if !defined(_WIN32)
    BFile* f;
    struct stat st;
    size_t got = 0;
    int fd, own = 0;
#endif
    if (!n)
        return 0;
    if (likely(!s->file)) {
        memcpy(dst, s->data + off, n);
        return 0;
    }
#if !defined(_WIN32)
    f = (BFile*)(void*)s->data;
    fd = -1;
    if (stat(f->path, &st) == 0 && bs_same(f, &st, s->len)) {
        fd = f->fd;
        if (fd < 0) {
            fd = open(f->path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0 && (fstat(fd, &st) != 0 || !bs_same(f, &st, s->len))) {
                close(fd);
                fd = -1;
            }
            if (fd >= 0) {
                if (__atomic_add_fetch(&bs_fd_open, 1, __ATOMIC_RELAXED) <= BS_FD_CAP) {
                    f->fd = fd;
                } else {
                    __atomic_sub_fetch(&bs_fd_open, 1, __ATOMIC_RELAXED);
                    own = 1;
                }
            }
        }
    }
    if (fd >= 0 && n <= BS_SMALL && ((off ^ (off + n - 1)) & ~(size_t)(BS_BLK - 1)) == 0) {
        size_t b0 = off & ~(size_t)(BS_BLK - 1);
        int hit = bs_blk_s == s && bs_blk_off == b0;
        if (!hit) {
            size_t want = s->len - b0 < BS_BLK ? s->len - b0 : BS_BLK, k = 0;
            if (!bs_blk)
                bs_blk = malloc(BS_BLK);
            bs_blk_s = NULL;
            while (bs_blk && k < want) {
                ssize_t r = pread(fd, bs_blk + k, want - k, (off_t)(b0 + k));
                if (r < 0 && errno == EINTR)
                    continue;
                if (r <= 0)
                    break;
                k += (size_t)r;
            }
            hit = bs_blk && k == want;
            if (hit) {
                bs_blk_s = s;
                bs_blk_off = b0;
            }
        }
        if (hit) {
            memcpy(dst, bs_blk + (off - b0), n);
            got = n;
        }
    }
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = pread(fd, dst + got, n - got, (off_t)(off + got));
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += (size_t)r;
        }
    }
    if (own)
        close(fd);
    if (got == n)
        return 0;
#endif
    JS_Throw(ctx, js_dom_exception(ctx, "NotReadableError", "The file changed or could not be read after os.openAsBlob"));
    return -1;
}

typedef struct BSeg {
    BStore* s;
    size_t off, len;
} BSeg;

typedef struct Blob {
    uint64_t size;
    uint32_t nseg, cap;
    BSeg* segs;
    BSeg one;
    JSValue type;
} Blob;

typedef struct FBlob {
    Blob b;
    JSValue name;
    double mtime;
} FBlob;

typedef struct BCur {
    Blob b;
    uint64_t pos;
    SRBSC* c;
} BCur;

enum { FR_EMPTY, FR_LOADING, FR_DONE };
enum { FE_LOADSTART, FE_PROGRESS, FE_LOAD, FE_ABORT, FE_ERROR, FE_LOADEND, FE_N };
enum { FM_BUFFER, FM_BINARY, FM_TEXT, FM_DATAURL };

typedef struct FrL {
    JSValue fn;
    uint32_t id;
    uint8_t type, capture, once, handler;
} FrL;

typedef struct FR {
    int state, mode;
    uint32_t gen;
    JSValue result, error, blob, enc;
    JSValue on[FE_N];
    FrL* ls;
    uint32_t nls, cap, next_id;
} FR;


static JSClassID blob_cid, file_cid, cur_cid, fr_cid;
static JSAtom atom_iter;

static const char* const fe_names[FE_N] = { "loadstart", "progress", "load", "abort", "error", "loadend" };

static BStore* bs_new(JSRuntime* rt, size_t len)
{
    BStore* s;
    if (len > SIZE_MAX - sizeof(BStore))
        return NULL;
    s = js_malloc_rt(rt, sizeof(BStore) + len);
    if (!s)
        return NULL;
    s->ref = 1;
    s->file = 0;
    s->len = len;
    return s;
}

static void bs_unref(JSRuntime* rt, BStore* s)
{
    if (!s || --s->ref > 0)
        return;
#if !defined(_WIN32)
    if (s->file) {
        BFile* f = (BFile*)(void*)s->data;
        if (f->fd >= 0) {
            close(f->fd);
            __atomic_sub_fetch(&bs_fd_open, 1, __ATOMIC_RELAXED);
        }
        if (bs_blk_s == s)
            bs_blk_s = NULL;
        if (--bs_files == 0) {
            free(bs_blk);
            bs_blk = NULL;
        }
    }
#endif
    js_free_rt(rt, s);
}

static void blob_init(Blob* b)
{
    memset(b, 0, sizeof(*b));
    b->segs = &b->one;
    b->type = JS_UNDEFINED;
}

static void blob_clear(JSRuntime* rt, Blob* b)
{
    uint32_t i;
    for (i = 0; i < b->nseg; i++)
        bs_unref(rt, b->segs[i].s);
    if (b->segs != &b->one)
        js_free_rt(rt, b->segs);
    JS_FreeValueRT(rt, b->type);
    blob_init(b);
}

static int blob_push(JSContext* ctx, Blob* b, BStore* s, size_t off, size_t len)
{
    BSeg* g;
    if (!len)
        return 0;
    if (b->nseg) {
        g = &b->segs[b->nseg - 1];
        if (g->s == s && g->off + g->len == off) {
            g->len += len;
            b->size += len;
            return 0;
        }
    }
    if (b->nseg == 1 && b->segs == &b->one) {
        BSeg* n = js_malloc(ctx, 4 * sizeof(BSeg));
        if (!n)
            return -1;
        n[0] = b->one;
        b->segs = n;
        b->cap = 4;
    } else if (b->nseg >= 1 && b->nseg == b->cap) {
        BSeg* n = js_realloc(ctx, b->segs, (size_t)b->cap * 2 * sizeof(BSeg));
        if (!n)
            return -1;
        b->segs = n;
        b->cap *= 2;
    }
    s->ref++;
    b->segs[b->nseg].s = s;
    b->segs[b->nseg].off = off;
    b->segs[b->nseg].len = len;
    b->nseg++;
    b->size += len;
    return 0;
}

static int blob_copy(JSContext* ctx, const Blob* b, uint8_t* dst, uint64_t from, size_t n)
{
    uint32_t i;
    for (i = 0; i < b->nseg && n; i++) {
        const BSeg* g = &b->segs[i];
        size_t k;
        if (from >= g->len) {
            from -= g->len;
            continue;
        }
        k = g->len - (size_t)from;
        if (k > n)
            k = n;
        if (bs_read(ctx, g->s, g->off + (size_t)from, dst, k) < 0)
            return -1;
        dst += k;
        n -= k;
        from = 0;
    }
    return 0;
}

static int blob_sub(JSContext* ctx, Blob* out, const Blob* b, uint64_t from, uint64_t n)
{
    uint32_t i;
    for (i = 0; i < b->nseg && n; i++) {
        const BSeg* g = &b->segs[i];
        size_t k;
        if (from >= g->len) {
            from -= g->len;
            continue;
        }
        k = g->len - (size_t)from;
        if (k > n)
            k = (size_t)n;
        if (blob_push(ctx, out, g->s, g->off + (size_t)from, k) < 0)
            return -1;
        n -= k;
        from = 0;
    }
    return 0;
}

static Blob* blob_of(JSValueConst v)
{
    Blob* b;
    if (!JS_IsObject(v))
        return NULL;
    b = JS_GetOpaque(v, blob_cid);
    return b ? b : JS_GetOpaque(v, file_cid);
}

static Blob* blob_this(JSContext* ctx, JSValueConst v)
{
    Blob* b = blob_of(v);
    if (!b)
        JS_ThrowTypeError(ctx, "Illegal invocation: not a Blob");
    return b;
}

int js_blob_is(JSValueConst v)
{
    return blob_of(v) != NULL;
}

static void blob_fin(JSRuntime* rt, JSValue v)
{
    JSClassID cid = JS_GetClassID(v);
    Blob* b = JS_GetOpaque(v, cid);
    if (b) {
        if (cid == file_cid)
            JS_FreeValueRT(rt, ((FBlob*)b)->name);
        blob_clear(rt, b);
        js_free_rt(rt, b);
    }
}

static JSValue blob_wrap(JSContext* ctx, JSValueConst proto, JSClassID cid, Blob* src, JSValue name, double mtime)
{
    Blob* b;
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, cid);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, name);
        blob_clear(JS_GetRuntime(ctx), src);
        return obj;
    }
    b = js_malloc(ctx, cid == file_cid ? sizeof(FBlob) : sizeof(Blob));
    if (!b) {
        JS_FreeValue(ctx, name);
        blob_clear(JS_GetRuntime(ctx), src);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    *b = *src;
    if (cid == file_cid) {
        ((FBlob*)b)->name = name;
        ((FBlob*)b)->mtime = mtime;
    } else
        JS_FreeValue(ctx, name);
    if (src->segs == &src->one)
        b->segs = &b->one;
    JS_SetOpaque(obj, b);
    return obj;
}

static JSValue blob_new(JSContext* ctx, Blob* src)
{
    JSValue proto = JS_GetClassProto(ctx, blob_cid), r;
    r = blob_wrap(ctx, proto, blob_cid, src, JS_UNDEFINED, 0);
    JS_FreeValue(ctx, proto);
    return r;
}

static JSValue str_lit(JSContext* ctx, const char* s)
{
    return JS_NewStringLen(ctx, s, strlen(s));
}

static JSValue btype(JSContext* ctx, const Blob* b)
{
    return JS_IsUndefined(b->type) ? JS_NewStringLen(ctx, "", 0) : JS_DupValue(ctx, b->type);
}

static JSValue type_normalize(JSContext* ctx, JSValueConst v)
{
    size_t n, i;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    char small[64], *t;
    JSValue r;
    if (!s)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) {
            JS_FreeCString(ctx, s);
            return str_lit(ctx, "");
        }
    }
    t = n <= sizeof(small) ? small : js_malloc(ctx, n);
    if (!t) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        t[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    r = JS_NewStringLen(ctx, t, n);
    if (t != small)
        js_free(ctx, t);
    JS_FreeCString(ctx, s);
    return r;
}

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

static double to_ll(double d)
{
    if (!isfinite(d))
        return 0;
    d = trunc(d);
    if (d > -9223372036854775808.0 && d < 9223372036854775808.0)
        return d == 0 ? 0 : d;
    d = fmod(d, 18446744073709551616.0);
    if (d < 0)
        d += 18446744073709551616.0;
    if (d >= 9223372036854775808.0)
        d -= 18446744073709551616.0;
    return d == 0 ? 0 : d;
}

enum { P_STR, P_L1, P_BUF, P_BLOB };

typedef struct BPart {
    int kind;
    const char* s;
    size_t len;
    JSValue v;
} BPart;

typedef struct BParts {
    BPart* a;
    uint32_t n, cap;
    BPart small[4];
} BParts;

static void parts_init(BParts* p)
{
    p->a = p->small;
    p->n = 0;
    p->cap = 4;
}

static void parts_free(JSContext* ctx, BParts* p)
{
    uint32_t i;
    for (i = 0; i < p->n; i++) {
        if (p->a[i].kind == P_STR)
            JS_FreeCString(ctx, p->a[i].s);
        else
            JS_FreeValue(ctx, p->a[i].v);
    }
    if (p->a != p->small)
        js_free(ctx, p->a);
    parts_init(p);
}

static int parts_add(JSContext* ctx, BParts* p, JSValue v)
{
    BPart* e;
    if (p->n == p->cap) {
        uint32_t nc = p->cap * 2;
        BPart* na = p->a == p->small ? js_malloc(ctx, nc * sizeof(BPart)) : js_realloc(ctx, p->a, nc * sizeof(BPart));
        if (!na) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (p->a == p->small)
            memcpy(na, p->small, sizeof(p->small));
        p->a = na;
        p->cap = nc;
    }
    e = &p->a[p->n];
    e->v = JS_UNDEFINED;
    e->s = NULL;
    e->len = 0;
    if (blob_of(v)) {
        e->kind = P_BLOB;
        e->v = v;
    } else if (JS_GetBufferKind(v) != JS_BUFFER_KIND_NONE) {
        JSValue buf = JS_UNDEFINED;
        if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER)
            buf = JS_DupValue(ctx, v);
        else {
            double off, len;
            int type;
            if (JS_GetViewInfo(ctx, v, &buf, &off, &len, &type) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
        }
        if (JS_IsSharedArrayBuffer(buf)) {
            JS_FreeValue(ctx, buf);
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "Blob: a SharedArrayBuffer is not a valid blob part");
            return -1;
        }
        JS_FreeValue(ctx, buf);
        e->kind = P_BUF;
        e->v = v;
    } else {
        JSValue sv = JS_IsString(v) ? v : JS_ToString(ctx, v);
        if (!JS_IsString(v))
            JS_FreeValue(ctx, v);
        if (JS_IsException(sv))
            return -1;
        e->s = (const char*)JS_GetNarrowStringBytes(ctx, sv, &e->len);
        if (e->s) {
            e->kind = P_L1;
            e->v = sv;
        } else {
            e->kind = P_STR;
            e->s = JS_ToCStringLen2(ctx, &e->len, sv, 2);
            JS_FreeValue(ctx, sv);
            if (!e->s)
                return -1;
        }
    }
    p->n++;
    return 0;
}

enum { BA_next, BA_done, BA_value, BA_endings, BA_type, BA_lastModified, BA_prototype, BA_length, BA_N };

static const char* const ba_names[BA_N] = { "next", "done", "value", "endings", "type", "lastModified", "prototype", "length" };

typedef struct BlobRt {
    JSAtom a[BA_N];
    JSContext* ctx;
    JSValue values_fn, aiter_proto, next_fn;
    JSValue ctor[2];
} BlobRt;

static BlobRt* brt(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return ts ? ts->blob : NULL;
}

static JSValue bget(JSContext* ctx, JSValueConst o, int k)
{
    BlobRt* r = brt(ctx);
    JSValue v = r ? JS_GetProperty(ctx, o, r->a[k]) : JS_GetPropertyStr(ctx, o, ba_names[k]);
    return v;
}

void js_blob_free_rt(JSRuntime* rt, JSThreadState* ts)
{
    BlobRt* r = ts->blob;
    int i;
    if (!r)
        return;
    for (i = 0; i < BA_N; i++)
        JS_FreeAtomRT(rt, r->a[i]);
    JS_FreeValueRT(rt, r->values_fn);
    JS_FreeValueRT(rt, r->aiter_proto);
    JS_FreeValueRT(rt, r->next_fn);
    JS_FreeValueRT(rt, r->ctor[0]);
    JS_FreeValueRT(rt, r->ctor[1]);
    js_free_rt(rt, r);
    ts->blob = NULL;
}

static void brt_init(JSContext* ctx, JSValueConst global_obj)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    BlobRt* r;
    JSValue arr, ap, it;
    int i;
    if (!ts || ts->blob)
        return;
    r = js_mallocz(ctx, sizeof(*r));
    if (!r)
        return;
    for (i = 0; i < BA_N; i++)
        r->a[i] = JS_NewAtom(ctx, ba_names[i]);
    r->ctx = ctx;
    r->ctor[0] = JS_UNDEFINED;
    r->ctor[1] = JS_UNDEFINED;
    arr = JS_GetPropertyStr(ctx, global_obj, "Array");
    ap = JS_IsException(arr) ? JS_EXCEPTION : JS_GetProperty(ctx, arr, r->a[BA_prototype]);
    JS_FreeValue(ctx, arr);
    r->values_fn = JS_IsException(ap) ? JS_EXCEPTION : JS_GetProperty(ctx, ap, atom_iter);
    arr = JS_NewArray(ctx);
    it = !JS_IsException(r->values_fn) && JS_IsFunction(ctx, r->values_fn) ? JS_Call(ctx, r->values_fn, arr, 0, NULL) : JS_UNDEFINED;
    r->aiter_proto = JS_IsObject(it) ? JS_GetPrototype(ctx, it) : JS_UNDEFINED;
    r->next_fn = JS_IsObject(r->aiter_proto) ? JS_GetProperty(ctx, r->aiter_proto, r->a[BA_next]) : JS_UNDEFINED;
    JS_FreeValue(ctx, it);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, ap);
    if (JS_IsException(r->values_fn) || JS_IsException(r->aiter_proto) || JS_IsException(r->next_fn)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        if (JS_IsException(r->values_fn))
            r->values_fn = JS_UNDEFINED;
        if (JS_IsException(r->aiter_proto))
            r->aiter_proto = JS_UNDEFINED;
        if (JS_IsException(r->next_fn))
            r->next_fn = JS_UNDEFINED;
    }
    ts->blob = r;
}

static int same_obj(JSValueConst a, JSValueConst b)
{
    return JS_IsObject(a) && JS_IsObject(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

static int array_iter_intact(JSContext* ctx, JSValueConst src, JSValueConst m)
{
    BlobRt* r = brt(ctx);
    JSPropertyDescriptor d;
    JSValue* arr;
    uint32_t cnt;
    int ok;
    if (!r || r->ctx != ctx || !same_obj(m, r->values_fn) || !JS_GetFastArray(ctx, src, &arr, &cnt))
        return 0;
    ok = JS_GetOwnProperty(ctx, &d, r->aiter_proto, r->a[BA_next]);
    if (ok <= 0)
        return ok;
    ok = same_obj(d.value, r->next_fn);
    JS_FreeValue(ctx, d.value);
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    return ok;
}

static int parts_fast(JSContext* ctx, JSValueConst src, BParts* p)
{
    uint32_t i;
    for (i = 0;; i++) {
        JSValue* arr;
        uint32_t cnt;
        JSValue v;
        if (JS_GetFastArray(ctx, src, &arr, &cnt)) {
            if (i >= cnt)
                return 0;
            v = JS_DupValue(ctx, arr[i]);
        } else {
            JSValue l = bget(ctx, src, BA_length);
            int64_t len;
            if (JS_IsException(l) || JS_ToInt64(ctx, &len, l)) {
                JS_FreeValue(ctx, l);
                return -1;
            }
            JS_FreeValue(ctx, l);
            if ((int64_t)i >= len)
                return 0;
            v = JS_GetPropertyUint32(ctx, src, i);
            if (JS_IsException(v))
                return -1;
        }
        if (parts_add(ctx, p, v) < 0)
            return -1;
    }
}

static int parts_collect(JSContext* ctx, JSValueConst src, BParts* p)
{
    JSValue m, it, next = JS_UNDEFINED;
    int ret = -1;
    if (JS_IsUndefined(src))
        return 0;
    if (!JS_IsObject(src)) {
        JS_ThrowTypeError(ctx, "Blob: blobParts must be a sequence");
        return -1;
    }
    m = JS_GetProperty(ctx, src, atom_iter);
    if (JS_IsException(m))
        return -1;
    if (!JS_IsFunction(ctx, m)) {
        JS_FreeValue(ctx, m);
        JS_ThrowTypeError(ctx, "Blob: blobParts is not iterable");
        return -1;
    }
    {
        int intact = array_iter_intact(ctx, src, m);
        if (intact < 0) {
            JS_FreeValue(ctx, m);
            return -1;
        }
        if (intact) {
            JS_FreeValue(ctx, m);
            return parts_fast(ctx, src, p);
        }
    }
    it = JS_Call(ctx, m, src, 0, NULL);
    JS_FreeValue(ctx, m);
    if (JS_IsException(it))
        return -1;
    if (!JS_IsObject(it)) {
        JS_ThrowTypeError(ctx, "Blob: iterator is not an object");
        goto done;
    }
    next = bget(ctx, it, BA_next);
    if (JS_IsException(next))
        goto done;
    for (;;) {
        JSValue r = JS_Call(ctx, next, it, 0, NULL), d, v;
        int dn;
        if (JS_IsException(r))
            goto done;
        if (!JS_IsObject(r)) {
            JS_FreeValue(ctx, r);
            JS_ThrowTypeError(ctx, "Blob: iterator result is not an object");
            goto done;
        }
        d = bget(ctx, r, BA_done);
        if (JS_IsException(d)) {
            JS_FreeValue(ctx, r);
            goto done;
        }
        dn = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
        if (dn) {
            JS_FreeValue(ctx, r);
            break;
        }
        v = bget(ctx, r, BA_value);
        JS_FreeValue(ctx, r);
        if (JS_IsException(v) || parts_add(ctx, p, v) < 0)
            goto done;
    }
    ret = 0;
done:
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, it);
    return ret;
}
static const uint8_t* buf_bytes(JSContext* ctx, JSValueConst v, size_t* n)
{
    const uint8_t* p;
    size_t blen = 0;
    *n = 0;
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
        p = JS_PeekArrayBuffer(v, &blen);
        *n = p ? blen : 0;
        return p;
    } else {
        JSValue buf;
        double off, len;
        int type;
        if (JS_GetViewInfo(ctx, v, &buf, &off, &len, &type) <= 0)
            return NULL;
        p = JS_PeekArrayBuffer(buf, &blen);
        JS_FreeValue(ctx, buf);
        if (!p || off + len > (double)blen)
            return NULL;
        *n = (size_t)len;
        return p + (size_t)off;
    }
}

static size_t native_len(const char* s, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
#if defined(_WIN32)
        if (s[i] == '\r') {
            if (i + 1 < n && s[i + 1] == '\n')
                i++;
            k += 2;
        } else if (s[i] == '\n')
            k += 2;
        else
            k++;
#else
        if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
            i++;
        k++;
#endif
    }
    return k;
}

static uint8_t* native_copy(uint8_t* d, const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\r' || s[i] == '\n') {
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
                i++;
#if defined(_WIN32)
            *d++ = '\r';
#endif
            *d++ = '\n';
        } else
            *d++ = (uint8_t)s[i];
    }
    return d;
}

static size_t l1_len(const uint8_t* s, size_t n, int native)
{
    size_t i, k = n;
    for (i = 0; i < n; i++) {
        if (!native && i + 8 <= n) {
            uint64_t w;
            memcpy(&w, s + i, 8);
            if (!(w & 0x8080808080808080ULL)) {
                i += 7;
                continue;
            }
        }
        if (s[i] >= 0x80)
            k++;
        else if (native && s[i] == 13) {
#if defined(_WIN32)
            if (i + 1 < n && s[i + 1] == 10)
                i++;
            else
                k++;
#else
            if (i + 1 < n && s[i + 1] == 10) {
                k--;
                i++;
            }
#endif
        }
#if defined(_WIN32)
        else if (native && s[i] == 10)
            k++;
#endif
    }
    return k;
}

static uint8_t* l1_copy(uint8_t* d, const uint8_t* s, size_t n, int native)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t c;
        if (!native && i + 8 <= n) {
            uint64_t w;
            memcpy(&w, s + i, 8);
            if (!(w & 0x8080808080808080ULL)) {
                memcpy(d, s + i, 8);
                d += 8;
                i += 7;
                continue;
            }
        }
        c = s[i];
        if (c >= 0x80) {
            *d++ = (uint8_t)(0xC0 | (c >> 6));
            *d++ = (uint8_t)(0x80 | (c & 0x3F));
        } else if (native && (c == 13 || c == 10)) {
            if (c == 13 && i + 1 < n && s[i + 1] == 10)
                i++;
#if defined(_WIN32)
            *d++ = 13;
#endif
            *d++ = 10;
        } else
            *d++ = c;
    }
    return d;
}

static size_t part_len(JSContext* ctx, BPart* e, int native)
{
    size_t n;
    if (e->kind == P_STR)
        return native ? native_len(e->s, e->len) : e->len;
    if (e->kind == P_L1)
        return l1_len((const uint8_t*)e->s, e->len, native);
    if (e->kind == P_BUF) {
        e->s = (const char*)buf_bytes(ctx, e->v, &n);
        e->len = n;
        return n;
    }
    return (size_t)blob_of(e->v)->size;
}

static int part_shared(BPart* e)
{
    return e->kind == P_BLOB && blob_of(e->v)->size >= BLOB_SHARE_MIN;
}

static int parts_build(JSContext* ctx, BParts* p, int native, Blob* out)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    uint32_t i, j;
    for (i = 0; i < p->n;) {
        size_t run = 0, k;
        BStore* s;
        uint8_t* d;
        if (part_shared(&p->a[i])) {
            Blob* b = blob_of(p->a[i].v);
            if (blob_sub(ctx, out, b, 0, b->size) < 0)
                return -1;
            i++;
            continue;
        }
        for (j = i; j < p->n && !part_shared(&p->a[j]); j++) {
            k = part_len(ctx, &p->a[j], native);
            if (run + k < run) {
                JS_ThrowRangeError(ctx, "Blob: size overflow");
                return -1;
            }
            run += k;
        }
        if (run) {
            s = bs_new(rt, run);
            if (!s) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            d = bs_ptr(s);
            for (k = i; k < j; k++) {
                BPart* e = &p->a[k];
                if (e->kind == P_L1) {
                    d = l1_copy(d, (const uint8_t*)e->s, e->len, native);
                } else                if (e->kind == P_STR) {
                    if (native)
                        d = native_copy(d, e->s, e->len);
                    else {
                        memcpy(d, e->s, e->len);
                        d += e->len;
                    }
                } else if (e->kind == P_BUF) {
                    if (e->len)
                        memcpy(d, e->s, e->len);
                    d += e->len;
                } else {
                    Blob* b = blob_of(e->v);
                    if (blob_copy(ctx, b, d, 0, (size_t)b->size) < 0) {
                        bs_unref(rt, s);
                        return -1;
                    }
                    d += b->size;
                }
            }
            if (blob_push(ctx, out, s, 0, run) < 0) {
                bs_unref(rt, s);
                return -1;
            }
            bs_unref(rt, s);
        }
        i = j;
    }
    return 0;
}

static int opt_get(JSContext* ctx, JSValueConst opts, int key, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    *out = bget(ctx, opts, key);
    return JS_IsException(*out) ? -1 : 0;
}

static int opt_endings(JSContext* ctx, JSValueConst opts, int* native)
{
    JSValue v;
    const char* s;
    *native = 0;
    if (opt_get(ctx, opts, BA_endings, &v) < 0)
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (!strcmp(s, "native"))
        *native = 1;
    else if (strcmp(s, "transparent")) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "Blob: endings must be \"transparent\" or \"native\"");
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static int opt_type(JSContext* ctx, JSValueConst opts, JSValue* type)
{
    JSValue v;
    if (opt_get(ctx, opts, BA_type, &v) < 0)
        return -1;
    *type = JS_IsUndefined(v) ? JS_UNDEFINED : type_normalize(ctx, v);
    JS_FreeValue(ctx, v);
    return JS_IsException(*type) ? -1 : 0;
}

static JSValue proto_from(JSContext* ctx, JSValueConst new_target, JSClassID cid)
{
    BlobRt* br = brt(ctx);
    if (br && br->ctx == ctx && JS_IsObject(new_target) && (cid == blob_cid || cid == file_cid)
        && same_obj(new_target, br->ctor[cid == file_cid]))
        return JS_GetClassProto(ctx, cid);
    JSValue p = bget(ctx, new_target, BA_prototype);
    if (JS_IsException(p) || JS_IsObject(p))
        return p;
    JS_FreeValue(ctx, p);
    return JS_GetClassProto(ctx, cid);
}

static JSValue blob_construct(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int file)
{
    BParts parts;
    Blob b;
    JSValueConst opts = argc > (file ? 2 : 1) ? argv[file ? 2 : 1] : JS_UNDEFINED;
    JSValue proto, name = JS_UNDEFINED, r;
    int native;
    double mtime = 0;
    JSClassID cid = file ? file_cid : blob_cid;
    parts_init(&parts);
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor %s cannot be invoked without 'new'", file ? "File" : "Blob");
    if (file && argc < 2)
        return JS_ThrowTypeError(ctx, "File: 2 arguments required, but only %d present", argc);
    blob_init(&b);
    if (parts_collect(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &parts) < 0)
        goto fail;
    if (file) {
        size_t n;
        const char* s = JS_ToCStringLen2(ctx, &n, argv[1], 2);
        if (!s)
            goto fail;
        name = JS_NewStringLen(ctx, s, n);
        JS_FreeCString(ctx, s);
        if (JS_IsException(name))
            goto fail;
    }
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts) && !JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", file ? "File" : "Blob");
        goto fail;
    }
    if (opt_endings(ctx, opts, &native) < 0)
        goto fail;
    if (file) {
        JSValue v;
        if (opt_get(ctx, opts, BA_lastModified, &v) < 0)
            goto fail;
        if (JS_IsUndefined(v))
            mtime = now_ms();
        else {
            int e = JS_ToFloat64(ctx, &mtime, v);
            JS_FreeValue(ctx, v);
            if (e)
                goto fail;
            mtime = to_ll(mtime);
        }
    }
    if (opt_type(ctx, opts, &b.type) < 0)
        goto fail;
    if (parts_build(ctx, &parts, native, &b) < 0)
        goto fail;
    parts_free(ctx, &parts);
    proto = proto_from(ctx, new_target, cid);
    if (JS_IsException(proto))
        goto fail;
    r = blob_wrap(ctx, proto, cid, &b, name, mtime);
    JS_FreeValue(ctx, proto);
    return r;
fail:
    parts_free(ctx, &parts);
    JS_FreeValue(ctx, name);
    blob_clear(JS_GetRuntime(ctx), &b);
    return JS_EXCEPTION;
}

static JSValue blob_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    return blob_construct(ctx, new_target, argc, argv, 0);
}

static JSValue file_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    return blob_construct(ctx, new_target, argc, argv, 1);
}

static JSValue blob_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    Blob* b;
    if (magic >= 2) {
        FBlob* f = JS_IsObject(this_val) ? JS_GetOpaque(this_val, file_cid) : NULL;
        if (!f)
            return JS_ThrowTypeError(ctx, "Illegal invocation: not a File");
        if (magic == 2)
            return JS_DupValue(ctx, f->name);
        if (magic == 3)
            return JS_NewFloat64(ctx, f->mtime);
        return str_lit(ctx, "");
    }
    b = blob_this(ctx, this_val);
    if (!b)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewFloat64(ctx, (double)b->size);
    return btype(ctx, b);
}

static int clamp_ll(JSContext* ctx, JSValueConst v, double* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (isnan(d))
        d = 0;
    else {
        if (d < -9223372036854775808.0)
            d = -9223372036854775808.0;
        if (d > 9223372036854775807.0)
            d = 9223372036854775807.0;
        d = nearbyint(d);
    }
    *out = d;
    return 0;
}

static uint64_t rel_pos(double d, uint64_t size)
{
    if (d < 0) {
        double r = (double)size + d;
        return r <= 0 ? 0 : (uint64_t)r;
    }
    return d >= (double)size ? size : (uint64_t)d;
}

static JSValue blob_slice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Blob* b = blob_this(ctx, this_val);
    Blob out;
    double ds = 0, de = 0;
    uint64_t s, e;
    JSValue type;
    if (!b)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && clamp_ll(ctx, argv[0], &ds) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (clamp_ll(ctx, argv[1], &de) < 0)
            return JS_EXCEPTION;
    } else
        de = (double)b->size;
    if (argc > 2 && !JS_IsUndefined(argv[2]))
        type = type_normalize(ctx, argv[2]);
    else
        type = JS_UNDEFINED;
    if (JS_IsException(type))
        return type;
    s = rel_pos(ds, b->size);
    e = rel_pos(de, b->size);
    blob_init(&out);
    out.type = type;
    if (e > s && blob_sub(ctx, &out, b, s, e - s) < 0) {
        blob_clear(JS_GetRuntime(ctx), &out);
        return JS_EXCEPTION;
    }
    return blob_new(ctx, &out);
}

static JSValue blob_ab(JSContext* ctx, const Blob* b)
{
    uint8_t* p;
    if (b->size > SIZE_MAX / 2)
        return JS_ThrowRangeError(ctx, "Blob: too large for an ArrayBuffer");
    p = malloc(b->size ? (size_t)b->size : 1);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    if (blob_copy(ctx, b, p, 0, (size_t)b->size) < 0) {
        free(p);
        return JS_EXCEPTION;
    }
    return dyn_nat_ab_take(ctx, p, (size_t)b->size);
}

static JSValue u8_over(JSContext* ctx, JSValue ab)
{
    JSValue r;
    if (JS_IsException(ab))
        return ab;
    {
        size_t n = 0;
        JSValue a[3];
        JS_PeekArrayBuffer(ab, &n);
        a[0] = ab;
        a[1] = JS_NewInt32(ctx, 0);
        a[2] = JS_NewFloat64(ctx, (double)n);
        r = JS_NewTypedArray(ctx, 3, (JSValueConst*)a, JS_TYPED_ARRAY_UINT8);
    }
    JS_FreeValue(ctx, ab);
    return r;
}

static JSValue blob_latin1(JSContext* ctx, const Blob* b)
{
    char* out;
    JSValue r;
    if (b->size > 0x3fffffff)
        return JS_ThrowRangeError(ctx, "Blob: too large for a string");
    r = JS_NewString8Buf(ctx, (size_t)b->size, &out);
    if (!JS_IsException(r) && out && blob_copy(ctx, b, (uint8_t*)out, 0, (size_t)b->size) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

static int ascii_run(const uint8_t* p, size_t n)
{
    uint64_t acc = 0;
    size_t k = 0;
    for (; k + 32 <= n; k += 32) {
        uint64_t w[4];
        memcpy(w, p + k, 32);
        acc |= w[0] | w[1] | w[2] | w[3];
    }
    for (; k + 8 <= n; k += 8) {
        uint64_t w;
        memcpy(&w, p + k, 8);
        acc |= w;
    }
    for (; k < n; k++)
        acc |= p[k];
    return !(acc & 0x8080808080808080ULL);
}

static int blob_ascii_str(JSContext* ctx, const Blob* b, JSValue* out)
{
    char* s;
    size_t o = 0;
    uint32_t i;
    if (b->size > 0x3fffffff)
        return 0;
    *out = JS_NewString8Buf(ctx, (size_t)b->size, &s);
    if (JS_IsException(*out) || !s)
        return 1;
    for (i = 0; i < b->nseg; i++) {
        size_t n = b->segs[i].len, k;
        for (k = 0; k < n; k += 65536) {
            size_t m = n - k < 65536 ? n - k : 65536;
            if (bs_read(ctx, b->segs[i].s, b->segs[i].off + k, (uint8_t*)s + o, m) < 0) {
                JS_FreeValue(ctx, *out);
                *out = JS_EXCEPTION;
                return 1;
            }
            if (!ascii_run((const uint8_t*)s + o, m)) {
                JS_FreeValue(ctx, *out);
                return 0;
            }
            o += m;
        }
    }
    return 1;
}

static JSValue blob_utf8(JSContext* ctx, const Blob* b, uint64_t skip)
{
    DynBuf d;
    JSUtf8DecodeState st;
    uint32_t i;
    JSValue r;
    uint8_t* tmp = NULL;
    if (!skip && blob_ascii_str(ctx, b, &r))
        return r;
    js_std_dbuf_init(ctx, &d);
    js_textcodec_utf8_init(&st);
    for (i = 0; i < b->nseg; i++) {
        const BSeg* g = &b->segs[i];
        size_t off = 0;
        if (skip) {
            off = skip >= g->len ? g->len : (size_t)skip;
            skip -= off;
        }
        if (g->s->file) {
            while (off < g->len) {
                size_t m = g->len - off < BLOB_CHUNK ? g->len - off : BLOB_CHUNK;
                if ((!tmp && !(tmp = js_malloc(ctx, BLOB_CHUNK))) || bs_read(ctx, g->s, g->off + off, tmp, m) < 0
                    || js_textcodec_utf8_feed(ctx, &d, tmp, m, &st, 0) < 0) {
                    js_free(ctx, tmp);
                    dbuf_free(&d);
                    return JS_EXCEPTION;
                }
                off += m;
            }
            continue;
        }
        if (js_textcodec_utf8_feed(ctx, &d, bs_ptr(g->s) + g->off + off, g->len - off, &st, 0) < 0) {
            js_free(ctx, tmp);
            dbuf_free(&d);
            return JS_EXCEPTION;
        }
    }
    js_free(ctx, tmp);
    if (js_textcodec_utf8_feed(ctx, &d, NULL, 0, &st, 1) < 0) {
        dbuf_free(&d);
        return JS_EXCEPTION;
    }
    r = JS_NewStringLen(ctx, (const char*)d.buf, d.size);
    dbuf_free(&d);
    return r;
}

static int blob_head(JSContext* ctx, const Blob* b, uint8_t* h, size_t n)
{
    memset(h, 0, n);
    return blob_copy(ctx, b, h, 0, b->size < n ? (size_t)b->size : n);
}

static JSValue blob_text_utf8(JSContext* ctx, const Blob* b)
{
    uint8_t h[3];
    if (blob_head(ctx, b, h, 3) < 0)
        return JS_EXCEPTION;
    return blob_utf8(ctx, b, (b->size >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) ? 3 : 0);
}

static JSValue blob_utf16(JSContext* ctx, const Blob* b, uint64_t skip, int be)
{
    DynBuf d;
    uint8_t cb[UTF8_CHAR_LEN_MAX];
    uint64_t i, n = b->size;
    uint32_t hi = 0;
    uint8_t* p;
    JSValue r;
    if (n > SIZE_MAX / 2)
        return JS_ThrowRangeError(ctx, "Blob: too large");
    p = js_malloc(ctx, n ? (size_t)n : 1);
    if (!p)
        return JS_EXCEPTION;
    if (blob_copy(ctx, b, p, 0, (size_t)n) < 0) {
        js_free(ctx, p);
        return JS_EXCEPTION;
    }
    js_std_dbuf_init(ctx, &d);
    for (i = skip; i + 1 < n; i += 2) {
        uint32_t u = be ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
        if (hi) {
            if (u >= 0xDC00 && u <= 0xDFFF) {
                dbuf_put(&d, cb, unicode_to_utf8(cb, 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00)));
                hi = 0;
                continue;
            }
            dbuf_put(&d, cb, unicode_to_utf8(cb, 0xFFFD));
            hi = 0;
        }
        if (u >= 0xD800 && u <= 0xDBFF)
            hi = u;
        else if (u >= 0xDC00 && u <= 0xDFFF)
            dbuf_put(&d, cb, unicode_to_utf8(cb, 0xFFFD));
        else
            dbuf_put(&d, cb, unicode_to_utf8(cb, u));
    }
    if (hi || i < n)
        dbuf_put(&d, cb, unicode_to_utf8(cb, 0xFFFD));
    js_free(ctx, p);
    if (d.error) {
        dbuf_free(&d);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, (const char*)d.buf, d.size);
    dbuf_free(&d);
    return r;
}

static JSValue settle(JSContext* ctx, JSValue v)
{
    JSValue f[2], p, r, e;
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, v);
        return p;
    }
    if (JS_IsException(v)) {
        e = JS_GetException(ctx);
        r = JS_Call(ctx, f[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, e);
    } else {
        r = JS_Call(ctx, f[0], JS_UNDEFINED, 1, (JSValueConst*)&v);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, f[0]);
    JS_FreeValue(ctx, f[1]);
    return p;
}

static JSValue blob_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    Blob* b = blob_of(this_val);
    (void)argc;
    (void)argv;
    if (!b)
        return settle(ctx, JS_ThrowTypeError(ctx, "Illegal invocation: not a Blob"));
    if (magic == 0)
        return settle(ctx, blob_text_utf8(ctx, b));
    if (magic == 1)
        return settle(ctx, blob_ab(ctx, b));
    return settle(ctx, u8_over(ctx, blob_ab(ctx, b)));
}

static void cur_fin(JSRuntime* rt, JSValue v)
{
    BCur* k = JS_GetOpaque(v, cur_cid);
    if (k) {
        blob_clear(rt, &k->b);
        js_free_rt(rt, k);
    }
}

static JSValue cur_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    BCur* k = JS_GetOpaque(data, cur_cid);
    SRBSC* c;
    uint64_t left;
    size_t n;
    JSValue req;
    (void)argc;
    (void)argv;
    if (!k || !k->c)
        return wst_resolved_undef(ctx);
    c = k->c;
    left = k->b.size - k->pos;
    req = wst_rbsc_get_byob_request(ctx, c);
    if (JS_IsException(req))
        return wst_reject_exc(ctx);
    if (!JS_IsNull(req) && !JS_IsUndefined(req)) {
        JSValue view = wst_byobreq_view(ctx, req);
        WVInfo vi;
        uint8_t* p;
        size_t blen;
        JS_FreeValue(ctx, req);
        if (JS_IsException(view))
            return wst_reject_exc(ctx);
        if (wst_view_info(ctx, view, &vi) <= 0) {
            JS_FreeValue(ctx, view);
            return wst_reject_err(ctx, 0, "Blob stream: bad BYOB view");
        }
        p = JS_PeekArrayBuffer(vi.buffer, &blen);
        n = left < (uint64_t)vi.len ? (size_t)left : (size_t)vi.len;
        if (p && n && blob_copy(ctx, &k->b, p + (size_t)vi.off, k->pos, n) < 0) {
            JS_FreeValue(ctx, vi.buffer);
            JS_FreeValue(ctx, view);
            return wst_reject_exc(ctx);
        }
        JS_FreeValue(ctx, vi.buffer);
        JS_FreeValue(ctx, view);
        k->pos += n;
        if (wst_rbsc_respond(ctx, c, (double)n) < 0)
            return wst_reject_exc(ctx);
    } else {
        JSValue u8;
        JS_FreeValue(ctx, req);
        n = left < BLOB_CHUNK ? (size_t)left : BLOB_CHUNK;
        {
            uint8_t* p = malloc(n ? n : 1);
            if (!p)
                return wst_reject_err(ctx, 0, "out of memory");
            if (blob_copy(ctx, &k->b, p, k->pos, n) < 0) {
                free(p);
                return wst_reject_exc(ctx);
            }
            u8 = u8_over(ctx, dyn_nat_ab_take(ctx, p, n));
        }
        if (JS_IsException(u8))
            return wst_reject_exc(ctx);
        k->pos += n;
        if (wst_rbsc_enqueue(ctx, c, u8) < 0) {
            JS_FreeValue(ctx, u8);
            return wst_reject_exc(ctx);
        }
        JS_FreeValue(ctx, u8);
    }
    if (k->pos >= k->b.size) {
        k->c = NULL;
        if (wst_rbsc_close(ctx, c) < 0)
            return wst_reject_exc(ctx);
        blob_clear(JS_GetRuntime(ctx), &k->b);
    }
    return wst_resolved_undef(ctx);
}

static JSValue cur_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    BCur* k = JS_GetOpaque(data, cur_cid);
    (void)argc;
    (void)argv;
    if (k) {
        k->c = NULL;
        blob_clear(JS_GetRuntime(ctx), &k->b);
    }
    return wst_resolved_undef(ctx);
}

static JSValue blob_stream_of(JSContext* ctx, const Blob* b)
{
    JSValue ns = js_streams_ns(ctx), cur, obj;
    SAlgo pull, cancel;
    BCur* k;
    SRS* s;
    if (JS_IsException(ns))
        return ns;
    JS_FreeValue(ctx, ns);
    cur = JS_NewObjectClass(ctx, cur_cid);
    if (JS_IsException(cur))
        return cur;
    k = js_mallocz(ctx, sizeof(*k));
    if (!k) {
        JS_FreeValue(ctx, cur);
        return JS_EXCEPTION;
    }
    blob_init(&k->b);
    JS_SetOpaque(cur, k);
    if (blob_sub(ctx, &k->b, b, 0, b->size) < 0) {
        JS_FreeValue(ctx, cur);
        return JS_EXCEPTION;
    }
    sa_init(&pull);
    sa_init(&cancel);
    sa_set_c(ctx, &pull, cur_pull, cur);
    sa_set_c(ctx, &cancel, cur_cancel, cur);
    obj = wst_rbs_create(ctx, JS_UNDEFINED, &pull, &cancel, &s);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, cur);
        return obj;
    }
    k->c = s->c;
    if (!b->size) {
        k->c = NULL;
        if (wst_rbsc_close(ctx, s->c) < 0) {
            JS_FreeValue(ctx, cur);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, cur);
    return obj;
}

static JSValue g_new(JSContext* ctx, const char* name, int argc, JSValueConst* argv)
{
    JSValue g = JS_GetGlobalObject(ctx), c = JS_GetPropertyStr(ctx, g, name), r;
    JS_FreeValue(ctx, g);
    if (JS_IsException(c))
        return c;
    r = JS_CallConstructor(ctx, c, argc, argv);
    JS_FreeValue(ctx, c);
    return r;
}

static JSValue blob_stream(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    Blob* b = blob_this(ctx, this_val);
    JSValue s, t, r;
    JSAtom a;
    (void)argc;
    (void)argv;
    if (!b)
        return JS_EXCEPTION;
    s = blob_stream_of(ctx, b);
    if (!magic || JS_IsException(s))
        return s;
    t = g_new(ctx, "TextDecoderStream", 0, NULL);
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, s);
        return t;
    }
    a = JS_NewAtom(ctx, "pipeThrough");
    r = JS_Invoke(ctx, s, a, 1, (JSValueConst*)&t);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, s);
    return r;
}

JSValue js_blob_bytes(JSContext* ctx, JSValueConst blob)
{
    Blob* b = blob_of(blob);
    if (!b)
        return JS_ThrowTypeError(ctx, "not a Blob");
    return u8_over(ctx, blob_ab(ctx, b));
}

JSValue js_blob_type(JSContext* ctx, JSValueConst blob)
{
    Blob* b = blob_of(blob);
    return b ? btype(ctx, b) : JS_UNDEFINED;
}

JSValue js_blob_file_name(JSContext* ctx, JSValueConst blob)
{
    FBlob* f = JS_IsObject(blob) ? JS_GetOpaque(blob, file_cid) : NULL;
    return f ? JS_DupValue(ctx, f->name) : JS_UNDEFINED;
}

JSValue js_blob_from_bytes(JSContext* ctx, JSValueConst bytes, JSValueConst type)
{
    Blob b;
    size_t n;
    const uint8_t* p = buf_bytes(ctx, bytes, &n);
    BStore* s;
    blob_init(&b);
    b.type = JS_IsString(type) ? type_normalize(ctx, type) : str_lit(ctx, "");
    if (JS_IsException(b.type))
        return b.type;
    if (n) {
        s = bs_new(JS_GetRuntime(ctx), n);
        if (!s) {
            blob_clear(JS_GetRuntime(ctx), &b);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(bs_ptr(s), p, n);
        if (blob_push(ctx, &b, s, 0, n) < 0) {
            bs_unref(JS_GetRuntime(ctx), s);
            blob_clear(JS_GetRuntime(ctx), &b);
            return JS_EXCEPTION;
        }
        bs_unref(JS_GetRuntime(ctx), s);
    }
    return blob_new(ctx, &b);
}

JSValue js_blob_form_entry(JSContext* ctx, JSValueConst value, JSValueConst filename, int has_filename)
{
    Blob* src = blob_of(value);
    Blob b;
    JSValue name, proto;
    if (!src) {
        size_t n;
        const char* s;
        JSValue r;
        if (JS_GetBufferKind(value) != JS_BUFFER_KIND_NONE)
            return JS_DupValue(ctx, value);
        s = JS_ToCStringLen2(ctx, &n, value, 2);
        if (!s)
            return JS_EXCEPTION;
        r = JS_NewStringLen(ctx, s, n);
        JS_FreeCString(ctx, s);
        return r;
    }
    if (!has_filename && JS_GetOpaque(value, file_cid))
        return JS_DupValue(ctx, value);
    if (has_filename) {
        size_t n;
        const char* s = JS_ToCStringLen2(ctx, &n, filename, 2);
        if (!s)
            return JS_EXCEPTION;
        name = JS_NewStringLen(ctx, s, n);
        JS_FreeCString(ctx, s);
    } else
        name = str_lit(ctx, "blob");
    if (JS_IsException(name))
        return name;
    blob_init(&b);
    b.type = JS_DupValue(ctx, src->type);
    if (blob_sub(ctx, &b, src, 0, src->size) < 0) {
        JS_FreeValue(ctx, name);
        blob_clear(JS_GetRuntime(ctx), &b);
        return JS_EXCEPTION;
    }
    proto = JS_GetClassProto(ctx, file_cid);
    name = blob_wrap(ctx, proto, file_cid, &b, name, now_ms());
    JS_FreeValue(ctx, proto);
    return name;
}

static char* blob_fd_path(int fd, const char* path)
{
#if defined(__APPLE__)
    char buf[PATH_MAX];
    if (fcntl(fd, F_GETPATH, buf) != -1 && buf[0] == '/')
        return strdup(buf);
#elif defined(__linux__)
    char link[40], buf[PATH_MAX];
    ssize_t n;
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, buf, sizeof(buf) - 1);
    if (n > 0 && buf[0] == '/') {
        buf[n] = 0;
        return strdup(buf);
    }
#else
    (void)fd;
#endif
    return realpath(path, NULL);
}

JSValue js_os_open_as_blob(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
#if defined(_WIN32)
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "openAsBlob: not supported on this platform");
#else
    const char* path;
    struct stat st;
    int fd;
    Blob b;
    BStore* s = NULL;
    JSValue proto, opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    const char* base;
    JSValue name;
    double mtime;
    (void)this_val;
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts) && !JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "openAsBlob: options must be an object");
    blob_init(&b);
    if (opt_type(ctx, opts, &b.type) < 0)
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!path) {
        blob_clear(JS_GetRuntime(ctx), &b);
        return JS_EXCEPTION;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        int e = fd < 0 ? errno : (S_ISREG(st.st_mode) ? errno : EISDIR);
        if (fd >= 0)
            close(fd);
        JS_ThrowTypeError(ctx, "openAsBlob: %s: %s", path, strerror(e));
        JS_FreeCString(ctx, path);
        blob_clear(JS_GetRuntime(ctx), &b);
        return JS_EXCEPTION;
    }
    if (st.st_size > 0) {
        char* rp = blob_fd_path(fd, path);
        const char* fp = rp ? rp : path;
        size_t pl = strlen(fp);
        BFile* bf;
        s = js_malloc(ctx, sizeof(BStore) + sizeof(BFile) + pl + 1);
        if (!s) {
            free(rp);
            close(fd);
            JS_FreeCString(ctx, path);
            blob_clear(JS_GetRuntime(ctx), &b);
            return JS_EXCEPTION;
        }
        s->ref = 1;
        s->file = 1;
        bs_files++;
        s->len = (size_t)st.st_size;
        bf = (BFile*)(void*)s->data;
        bf->dev = st.st_dev;
        bf->ino = st.st_ino;
        bf->mt = st_mtime_ns(&st);
        bf->fd = -1;
        memcpy(bf->path, fp, pl + 1);
        free(rp);
        if (blob_push(ctx, &b, s, 0, s->len) < 0) {
            bs_unref(JS_GetRuntime(ctx), s);
            close(fd);
            JS_FreeCString(ctx, path);
            blob_clear(JS_GetRuntime(ctx), &b);
            return JS_EXCEPTION;
        }
        bs_unref(JS_GetRuntime(ctx), s);
    }
    if (s && __atomic_add_fetch(&bs_fd_open, 1, __ATOMIC_RELAXED) <= BS_FD_CAP) {
        ((BFile*)(void*)s->data)->fd = fd;
        fd = -1;
    } else if (s) {
        __atomic_sub_fetch(&bs_fd_open, 1, __ATOMIC_RELAXED);
    }
    if (fd >= 0)
        close(fd);
    base = strrchr(path, '/');
    name = str_lit(ctx, base ? base + 1 : path);
    JS_FreeCString(ctx, path);
#if defined(__APPLE__)
    mtime = (double)st.st_mtimespec.tv_sec * 1000.0 + (double)(st.st_mtimespec.tv_nsec / 1000000);
#else
    mtime = (double)st.st_mtim.tv_sec * 1000.0 + (double)(st.st_mtim.tv_nsec / 1000000);
#endif
    if (JS_IsException(name)) {
        blob_clear(JS_GetRuntime(ctx), &b);
        return JS_EXCEPTION;
    }
    proto = JS_GetClassProto(ctx, file_cid);
    {
        JSValue r = blob_wrap(ctx, proto, file_cid, &b, name, mtime);
        JS_FreeValue(ctx, proto);
        return r;
    }
#endif
}

static FR* fr_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, fr_cid);
}

static void fr_fin(JSRuntime* rt, JSValue v)
{
    FR* f = JS_GetOpaque(v, fr_cid);
    uint32_t i;
    if (!f)
        return;
    JS_FreeValueRT(rt, f->result);
    JS_FreeValueRT(rt, f->error);
    JS_FreeValueRT(rt, f->blob);
    JS_FreeValueRT(rt, f->enc);
    for (i = 0; i < FE_N; i++)
        JS_FreeValueRT(rt, f->on[i]);
    for (i = 0; i < f->nls; i++)
        JS_FreeValueRT(rt, f->ls[i].fn);
    js_free_rt(rt, f->ls);
    js_free_rt(rt, f);
}

static void fr_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FR* f = JS_GetOpaque(v, fr_cid);
    uint32_t i;
    if (!f)
        return;
    JS_MarkValue(rt, f->result, mf);
    JS_MarkValue(rt, f->error, mf);
    JS_MarkValue(rt, f->blob, mf);
    JS_MarkValue(rt, f->enc, mf);
    for (i = 0; i < FE_N; i++)
        JS_MarkValue(rt, f->on[i], mf);
    for (i = 0; i < f->nls; i++)
        JS_MarkValue(rt, f->ls[i].fn, mf);
}

static JSValue fr_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    FR* f;
    JSValue proto, obj;
    int i;
    (void)argc;
    (void)argv;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor FileReader cannot be invoked without 'new'");
    proto = proto_from(ctx, new_target, fr_cid);
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, fr_cid);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    f = js_mallocz(ctx, sizeof(*f));
    if (!f) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    f->result = JS_NULL;
    f->error = JS_NULL;
    f->blob = JS_UNDEFINED;
    f->enc = JS_UNDEFINED;
    for (i = 0; i < FE_N; i++)
        f->on[i] = JS_NULL;
    JS_SetOpaque(obj, f);
    return obj;
}

static int fe_index(JSContext* ctx, JSValueConst v)
{
    const char* s = JS_ToCString(ctx, v);
    int i, r = -1;
    if (!s)
        return -2;
    for (i = 0; i < FE_N; i++)
        if (!strcmp(s, fe_names[i]))
            r = i;
    JS_FreeCString(ctx, s);
    return r;
}

static int fr_add(JSContext* ctx, FR* f, int type, JSValueConst fn, int capture, int once, int handler)
{
    uint32_t i;
    if (!handler)
        for (i = 0; i < f->nls; i++)
            if (!f->ls[i].handler && f->ls[i].type == type && f->ls[i].capture == capture && JS_StrictEq(ctx, f->ls[i].fn, fn))
                return 0;
    if (f->nls == f->cap) {
        uint32_t nc = f->cap ? f->cap * 2 : 2;
        FrL* n = js_realloc(ctx, f->ls, nc * sizeof(FrL));
        if (!n)
            return -1;
        f->ls = n;
        f->cap = nc;
    }
    f->ls[f->nls].fn = JS_DupValue(ctx, fn);
    f->ls[f->nls].type = (uint8_t)type;
    f->ls[f->nls].capture = (uint8_t)capture;
    f->ls[f->nls].once = (uint8_t)once;
    f->ls[f->nls].handler = (uint8_t)handler;
    f->ls[f->nls].id = ++f->next_id;
    f->nls++;
    return 0;
}

static void fr_drop(JSContext* ctx, FR* f, uint32_t i)
{
    JS_FreeValue(ctx, f->ls[i].fn);
    memmove(&f->ls[i], &f->ls[i + 1], (f->nls - i - 1) * sizeof(FrL));
    f->nls--;
}

static int listen_opts(JSContext* ctx, JSValueConst o, int* capture, int* once)
{
    JSValue v;
    *capture = *once = 0;
    if (JS_IsObject(o)) {
        v = JS_GetPropertyStr(ctx, o, "capture");
        if (JS_IsException(v))
            return -1;
        *capture = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (once) {
            v = JS_GetPropertyStr(ctx, o, "once");
            if (JS_IsException(v))
                return -1;
            *once = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
        }
    } else
        *capture = JS_ToBool(ctx, o);
    return 0;
}

static JSValue fr_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FR* f = fr_of(ctx, this_val);
    int type, capture, once;
    uint32_t i;
    if (!f)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%sEventListener: 2 arguments required", magic ? "remove" : "add");
    type = fe_index(ctx, argv[0]);
    if (type == -2)
        return JS_EXCEPTION;
    if (listen_opts(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &capture, &once) < 0)
        return JS_EXCEPTION;
    if (type < 0 || (!JS_IsObject(argv[1])))
        return JS_UNDEFINED;
    if (!magic)
        return fr_add(ctx, f, type, argv[1], capture, once, 0) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    for (i = 0; i < f->nls; i++)
        if (!f->ls[i].handler && f->ls[i].type == type && f->ls[i].capture == capture && JS_StrictEq(ctx, f->ls[i].fn, argv[1])) {
            fr_drop(ctx, f, i);
            break;
        }
    return JS_UNDEFINED;
}

static JSValue fr_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    FR* f = fr_of(ctx, this_val);
    return f ? JS_DupValue(ctx, f->on[magic]) : JS_EXCEPTION;
}

static JSValue fr_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    FR* f = fr_of(ctx, this_val);
    uint32_t i;
    int had;
    if (!f)
        return JS_EXCEPTION;
    had = !JS_IsNull(f->on[magic]);
    JS_FreeValue(ctx, f->on[magic]);
    f->on[magic] = JS_IsObject(v) ? JS_DupValue(ctx, v) : JS_NULL;
    if (JS_IsNull(f->on[magic])) {
        for (i = 0; i < f->nls; i++)
            if (f->ls[i].handler && f->ls[i].type == magic) {
                fr_drop(ctx, f, i);
                break;
            }
    } else if (!had && fr_add(ctx, f, magic, JS_UNDEFINED, 0, 0, 1) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue fr_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    FR* f = fr_of(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt32(ctx, f->state);
    if (magic == 1)
        return JS_DupValue(ctx, f->result);
    return JS_DupValue(ctx, f->error);
}

static int fr_fire(JSContext* ctx, JSValueConst self, int type, int drain)
{
    FR* f = JS_GetOpaque(self, fr_cid);
    JSValue ev;
    uint32_t *ids, i, n, k = 0;
    double total = 0;
    int ret = 0;
    if (!f)
        return 0;
    if (!JS_IsUndefined(f->blob))
        total = (double)blob_of(f->blob)->size;
    ev = JS_NewObject(ctx);
    if (JS_IsException(ev))
        return -1;
    JS_SetPropertyStr(ctx, ev, "type", str_lit(ctx, fe_names[type]));
    JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, self));
    JS_SetPropertyStr(ctx, ev, "currentTarget", JS_DupValue(ctx, self));
    JS_SetPropertyStr(ctx, ev, "lengthComputable", JS_TRUE);
    JS_SetPropertyStr(ctx, ev, "loaded", JS_NewFloat64(ctx, (type == FE_PROGRESS || type == FE_LOAD || (type == FE_LOADEND && !JS_IsNull(f->result))) ? total : 0));
    JS_SetPropertyStr(ctx, ev, "total", JS_NewFloat64(ctx, total));
    JS_SetPropertyStr(ctx, ev, "isTrusted", JS_TRUE);
    n = f->nls;
    ids = js_malloc(ctx, (n ? n : 1) * sizeof(uint32_t));
    if (!ids) {
        JS_FreeValue(ctx, ev);
        return -1;
    }
    for (i = 0; i < n; i++)
        if (f->ls[i].type == type)
            ids[k++] = f->ls[i].id;
    for (i = 0; i < k; i++) {
        JSValue r, fn, hf;
        uint32_t j;
        f = JS_GetOpaque(self, fr_cid);
        for (j = 0; j < f->nls && f->ls[j].id != ids[i]; j++) {
        }
        if (j == f->nls)
            continue;
        fn = f->ls[j].handler ? JS_DupValue(ctx, f->on[type]) : JS_DupValue(ctx, f->ls[j].fn);
        if (f->ls[j].once)
            fr_drop(ctx, f, j);
        if (JS_IsFunction(ctx, fn))
            r = JS_Call(ctx, fn, self, 1, (JSValueConst*)&ev);
        else if (!JS_IsObject(fn))
            r = JS_UNDEFINED;
        else {
            hf = JS_GetPropertyStr(ctx, fn, "handleEvent");
            if (JS_IsException(hf))
                r = JS_EXCEPTION;
            else if (!JS_IsFunction(ctx, hf))
                r = JS_ThrowTypeError(ctx, "handleEvent is not a function");
            else
                r = JS_Call(ctx, hf, fn, 1, (JSValueConst*)&ev);
            JS_FreeValue(ctx, hf);
        }
        JS_FreeValue(ctx, fn);
        if (JS_IsException(r))
            js_std_dump_error(ctx);
        JS_FreeValue(ctx, r);
        if (drain) {
            JSRuntime* rt = JS_GetRuntime(ctx);
            int e;
            while ((e = JS_ExecutePendingJob(rt, NULL)) > 0) {
            }
            if (e < 0) {
                ret = -1;
                break;
            }
        }
    }
    js_free(ctx, ids);
    JS_FreeValue(ctx, ev);
    return ret;
}

static int label_is(const char* l, const char* const* set)
{
    for (; *set; set++)
        if (!strcmp(l, *set))
            return 1;
    return 0;
}

static int enc_kind(JSContext* ctx, JSValueConst label, JSValue* dec)
{
    static const char* const u8[] = { "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8", NULL };
    static const char* const le[] = { "csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff", "utf-16", "utf-16le", NULL };
    static const char* const be[] = { "unicodefffe", "utf-16be", NULL };
    char buf[64];
    size_t n, i, a = 0, z;
    const char* s = JS_ToCStringLen(ctx, &n, label);
    int k = -1;
    *dec = JS_UNDEFINED;
    if (!s) {
        wst_drop_exc(ctx);
        return -1;
    }
    z = n;
    while (a < z && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\f' || s[a] == '\r'))
        a++;
    while (z > a && (s[z - 1] == ' ' || s[z - 1] == '\t' || s[z - 1] == '\n' || s[z - 1] == '\f' || s[z - 1] == '\r'))
        z--;
    if (z - a < sizeof(buf)) {
        for (i = a; i < z; i++)
            buf[i - a] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        buf[z - a] = 0;
        if (label_is(buf, u8))
            k = 0;
        else if (label_is(buf, le))
            k = 1;
        else if (label_is(buf, be))
            k = 2;
        else if (strcmp(buf, "replacement") && z > a) {
            JSValue l = JS_NewString(ctx, buf);
            *dec = g_new(ctx, "TextDecoder", 1, (JSValueConst*)&l);
            JS_FreeValue(ctx, l);
            if (JS_IsException(*dec)) {
                wst_drop_exc(ctx);
                *dec = JS_UNDEFINED;
            } else
                k = 3;
        }
    }
    JS_FreeCString(ctx, s);
    return k;
}

static int type_charset(JSContext* ctx, JSValueConst type, JSValue* dec)
{
    size_t n, i;
    const char* s = JS_ToCStringLen(ctx, &n, type);
    int k = -1;
    *dec = JS_UNDEFINED;
    if (!s) {
        wst_drop_exc(ctx);
        return -1;
    }
    for (i = 0; i + 8 <= n; i++) {
        if (s[i] == ';' || s[i] == ' ' || s[i] == '\t') {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t'))
                j++;
            if (j + 8 <= n && !strncmp(s + j, "charset=", 8)) {
                size_t a = j + 8, z = a;
                JSValue l;
                if (a < n && s[a] == '"') {
                    a++;
                    z = a;
                    while (z < n && s[z] != '"')
                        z++;
                } else
                    while (z < n && s[z] != ';')
                        z++;
                l = JS_NewStringLen(ctx, s + a, z - a);
                if (!JS_IsException(l))
                    k = enc_kind(ctx, l, dec);
                JS_FreeValue(ctx, l);
                break;
            }
        }
    }
    JS_FreeCString(ctx, s);
    return k;
}

static JSValue fr_text(JSContext* ctx, const Blob* b, JSValueConst enc)
{
    JSValue dec = JS_UNDEFINED, r;
    uint8_t h[3];
    int k = -1;
    if (blob_head(ctx, b, h, 3) < 0)
        return JS_EXCEPTION;
    if (b->size >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF)
        return blob_utf8(ctx, b, 3);
    if (b->size >= 2 && h[0] == 0xFE && h[1] == 0xFF)
        return blob_utf16(ctx, b, 2, 1);
    if (b->size >= 2 && h[0] == 0xFF && h[1] == 0xFE)
        return blob_utf16(ctx, b, 2, 0);
    if (!JS_IsUndefined(enc))
        k = enc_kind(ctx, enc, &dec);
    if (k < 0)
        k = JS_IsUndefined(b->type) ? -1 : type_charset(ctx, b->type, &dec);
    if (k == 1 || k == 2)
        return blob_utf16(ctx, b, 0, k == 2);
    if (k == 3) {
        JSValue u8 = u8_over(ctx, blob_ab(ctx, b));
        JSAtom a;
        if (JS_IsException(u8)) {
            JS_FreeValue(ctx, dec);
            return u8;
        }
        a = JS_NewAtom(ctx, "decode");
        r = JS_Invoke(ctx, dec, a, 1, (JSValueConst*)&u8);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, u8);
        JS_FreeValue(ctx, dec);
        return r;
    }
    return blob_utf8(ctx, b, 0);
}

static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static JSValue fr_dataurl(JSContext* ctx, const Blob* b)
{
    size_t tl, n = (size_t)b->size, ol, i, o;
    JSValue tv = btype(ctx, b);
    const char* t = JS_ToCStringLen(ctx, &tl, tv);
    const char* pre;
    char* out;
    uint8_t* p;
    JSValue r;
    JS_FreeValue(ctx, tv);
    if (!t)
        return JS_EXCEPTION;
    pre = tl ? t : "application/octet-stream";
    if (!tl)
        tl = strlen(pre);
    if (n > (0x3fffffff / 4) * 3 - 64) {
        JS_FreeCString(ctx, t);
        return JS_ThrowRangeError(ctx, "FileReader: blob too large for a data URL");
    }
    p = js_malloc(ctx, n ? n : 1);
    if (!p) {
        JS_FreeCString(ctx, t);
        return JS_EXCEPTION;
    }
    if (blob_copy(ctx, b, p, 0, n) < 0) {
        js_free(ctx, p);
        JS_FreeCString(ctx, t);
        return JS_EXCEPTION;
    }
    ol = 5 + tl + 8 + (n + 2) / 3 * 4;
    r = JS_NewString8Buf(ctx, ol, &out);
    if (!JS_IsException(r) && out) {
        memcpy(out, "data:", 5);
        memcpy(out + 5, pre, tl);
        memcpy(out + 5 + tl, ";base64,", 8);
        o = 13 + tl;
        for (i = 0; i + 2 < n; i += 3) {
            uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
            out[o++] = b64c[v >> 18];
            out[o++] = b64c[(v >> 12) & 63];
            out[o++] = b64c[(v >> 6) & 63];
            out[o++] = b64c[v & 63];
        }
        if (i < n) {
            uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0);
            out[o++] = b64c[v >> 18];
            out[o++] = b64c[(v >> 12) & 63];
            out[o++] = i + 1 < n ? b64c[(v >> 6) & 63] : '=';
            out[o++] = '=';
        }
    }
    js_free(ctx, p);
    JS_FreeCString(ctx, t);
    return r;
}

static JSValue fr_task(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data);

static int fr_schedule(JSContext* ctx, JSValueConst self, uint32_t gen, int step)
{
    JSValue d[2], fn, a[2], r;
    d[0] = JS_DupValue(ctx, self);
    d[1] = JS_NewUint32(ctx, gen);
    fn = JS_NewCFunctionData(ctx, fr_task, 0, step, 2, d);
    JS_FreeValue(ctx, d[0]);
    if (JS_IsException(fn))
        return -1;
    a[0] = fn;
    a[1] = JS_NewInt32(ctx, 0);
    r = js_os_setTimeout(ctx, JS_UNDEFINED, 2, a, 0);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static JSValue fr_task(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    FR* f = JS_GetOpaque(data[0], fr_cid);
    uint32_t gen;
    Blob* b;
    JSValue res;
    (void)this_val;
    (void)argc;
    (void)argv;
    JS_ToUint32(ctx, &gen, data[1]);
    if (!f || f->gen != gen || f->state != FR_LOADING)
        return JS_UNDEFINED;
    b = blob_of(f->blob);
    if (magic == 0) {
        if (fr_fire(ctx, data[0], FE_LOADSTART, 1) < 0)
            return JS_EXCEPTION;
        f = JS_GetOpaque(data[0], fr_cid);
        if (f->gen == gen && f->state == FR_LOADING && fr_schedule(ctx, data[0], gen, b->size ? 1 : 2) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    if (magic == 1) {
        if (fr_fire(ctx, data[0], FE_PROGRESS, 1) < 0)
            return JS_EXCEPTION;
        f = JS_GetOpaque(data[0], fr_cid);
        if (f->gen == gen && f->state == FR_LOADING && fr_schedule(ctx, data[0], gen, 2) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    switch (f->mode) {
    case FM_BUFFER:
        res = blob_ab(ctx, b);
        break;
    case FM_BINARY:
        res = blob_latin1(ctx, b);
        break;
    case FM_TEXT:
        res = fr_text(ctx, b, f->enc);
        break;
    default:
        res = fr_dataurl(ctx, b);
        break;
    }
    f = JS_GetOpaque(data[0], fr_cid);
    f->state = FR_DONE;
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, f->error);
        f->error = JS_UNDEFINED;
        f->error = js_dom_exception(ctx, "NotReadableError", "The blob could not be read");
        if (JS_IsException(f->error))
            f->error = JS_NULL;
        if (fr_fire(ctx, data[0], FE_ERROR, 1) < 0)
            return JS_EXCEPTION;
    } else {
        JS_FreeValue(ctx, f->result);
        f->result = res;
        if (fr_fire(ctx, data[0], FE_LOAD, 1) < 0)
            return JS_EXCEPTION;
    }
    f = JS_GetOpaque(data[0], fr_cid);
    if (f->state != FR_LOADING && fr_fire(ctx, data[0], FE_LOADEND, 1) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue fr_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FR* f = fr_of(ctx, this_val);
    JSValue enc = JS_UNDEFINED;
    if (!f)
        return JS_EXCEPTION;
    if (argc < 1 || !blob_of(argv[0]))
        return JS_ThrowTypeError(ctx, "FileReader: parameter 1 is not of type 'Blob'");
    if (magic == FM_TEXT && argc > 1 && !JS_IsUndefined(argv[1])) {
        enc = JS_ToString(ctx, argv[1]);
        if (JS_IsException(enc))
            return enc;
    }
    if (f->state == FR_LOADING) {
        JS_FreeValue(ctx, enc);
        return js_throw_dom_exception(ctx, "InvalidStateError", "FileReader: a read is already in progress");
    }
    f->state = FR_LOADING;
    f->mode = magic;
    f->gen++;
    JS_FreeValue(ctx, f->result);
    f->result = JS_NULL;
    JS_FreeValue(ctx, f->error);
    f->error = JS_NULL;
    JS_FreeValue(ctx, f->blob);
    f->blob = JS_DupValue(ctx, argv[0]);
    JS_FreeValue(ctx, f->enc);
    f->enc = enc;
    if (fr_schedule(ctx, this_val, f->gen, 0) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue fr_abort(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FR* f = fr_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    if (f->state == FR_EMPTY || f->state == FR_DONE) {
        JS_FreeValue(ctx, f->result);
        f->result = JS_NULL;
        return JS_UNDEFINED;
    }
    f->state = FR_DONE;
    f->gen++;
    JS_FreeValue(ctx, f->result);
    f->result = JS_NULL;
    if (fr_fire(ctx, this_val, FE_ABORT, 0) < 0)
        return JS_EXCEPTION;
    f = JS_GetOpaque(this_val, fr_cid);
    if (f->state != FR_LOADING && fr_fire(ctx, this_val, FE_LOADEND, 0) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static uint32_t url_seq;

static void url_rand(uint8_t* r)
{
    dyn_prng_t g;
    uint64_t a, b;
    if (dyn_prng_seed_random(&g) < 0)
        dyn_prng_seed(&g, (uint64_t)get_time_ns() ^ ((uint64_t)++url_seq << 40));
    a = dyn_prng_next(&g);
    b = dyn_prng_next(&g);
    memcpy(r, &a, 8);
    memcpy(r + 8, &b, 8);
}

static JSValue url_create(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    JSValue g, loc, org, key;
    uint8_t r[16];
    char u[40], *o;
    const char* os = NULL;
    size_t i, k = 0;
    (void)this_val;
    if (magic) {
        JSValue s;
        JSAtom a;
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "revokeObjectURL: 1 argument required");
        s = JS_ToString(ctx, argv[0]);
        if (JS_IsException(s))
            return s;
        a = JS_NewAtom(ctx, "delete");
        g = JS_Invoke(ctx, data[0], a, 1, (JSValueConst*)&s);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, s);
        if (JS_IsException(g))
            return g;
        JS_FreeValue(ctx, g);
        return JS_UNDEFINED;
    }
    if (argc < 1 || !blob_of(argv[0]))
        return JS_ThrowTypeError(ctx, "createObjectURL: parameter 1 is not of type 'Blob'");
    url_rand(r);
    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            u[k++] = '-';
        u[k++] = "0123456789abcdef"[r[i] >> 4];
        u[k++] = "0123456789abcdef"[r[i] & 15];
    }
    u[k] = 0;
    g = JS_GetGlobalObject(ctx);
    loc = JS_GetPropertyStr(ctx, g, "location");
    JS_FreeValue(ctx, g);
    if (JS_IsException(loc))
        return JS_EXCEPTION;
    org = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "origin") : JS_UNDEFINED;
    JS_FreeValue(ctx, loc);
    if (JS_IsException(org))
        return JS_EXCEPTION;
    if (JS_IsString(org)) {
        os = JS_ToCString(ctx, org);
        if (!os) {
            JS_FreeValue(ctx, org);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, org);
    if (!os || !*os) {
        if (os)
            JS_FreeCString(ctx, os);
        os = NULL;
    }
    o = js_malloc(ctx, 6 + (os ? strlen(os) : 4) + 1 + k + 1);
    if (!o) {
        if (os)
            JS_FreeCString(ctx, os);
        return JS_EXCEPTION;
    }
    snprintf(o, 6 + (os ? strlen(os) : 4) + 1 + k + 1, "blob:%s/%s", os ? os : "null", u);
    if (os)
        JS_FreeCString(ctx, os);
    key = JS_NewString(ctx, o);
    js_free(ctx, o);
    if (JS_IsException(key))
        return key;
    {
        JSValue a2[2];
        JSAtom a = JS_NewAtom(ctx, "set");
        a2[0] = key;
        a2[1] = argv[0];
        g = JS_Invoke(ctx, data[0], a, 2, a2);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(g)) {
            JS_FreeValue(ctx, key);
            return g;
        }
        JS_FreeValue(ctx, g);
    }
    return key;
}

void js_blob_install_url(JSContext* ctx, JSValueConst global_obj)
{
    JSValue u = JS_GetPropertyStr(ctx, global_obj, "URL"), map, f;
    if (JS_IsException(u)) {
        dyn_exc_drop(ctx);
        return;
    }
    if (!JS_IsObject(u)) {
        JS_FreeValue(ctx, u);
        return;
    }
    map = g_new(ctx, "Map", 0, NULL);
    if (JS_IsException(map)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, u);
        return;
    }
    f = JS_NewCFunctionData(ctx, url_create, 1, 0, 1, &map);
    JS_DefinePropertyValueStr(ctx, u, "createObjectURL", f, JS_PROP_C_W_E);
    f = JS_NewCFunctionData(ctx, url_create, 1, 1, 1, &map);
    JS_DefinePropertyValueStr(ctx, u, "revokeObjectURL", f, JS_PROP_C_W_E);
    JS_FreeValue(ctx, map);
    JS_FreeValue(ctx, u);
}

static const JSCFunctionListEntry blob_proto_m[] = {
    JS_CGETSET_MAGIC_DEF("size", blob_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("type", blob_get, NULL, 1),
    JS_CFUNC_DEF("slice", 0, blob_slice),
    JS_CFUNC_MAGIC_DEF("stream", 0, blob_stream, 0),
    JS_CFUNC_MAGIC_DEF("textStream", 0, blob_stream, 1),
    JS_CFUNC_MAGIC_DEF("text", 0, blob_read, 0),
    JS_CFUNC_MAGIC_DEF("arrayBuffer", 0, blob_read, 1),
    JS_CFUNC_MAGIC_DEF("bytes", 0, blob_read, 2),
    WST_TAG("Blob"),
};

static const JSCFunctionListEntry file_proto_m[] = {
    JS_CGETSET_MAGIC_DEF("name", blob_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("lastModified", blob_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("webkitRelativePath", blob_get, NULL, 4),
    WST_TAG("File"),
};

static const JSCFunctionListEntry fr_proto_m[] = {
    JS_CFUNC_MAGIC_DEF("readAsArrayBuffer", 1, fr_read, FM_BUFFER),
    JS_CFUNC_MAGIC_DEF("readAsBinaryString", 1, fr_read, FM_BINARY),
    JS_CFUNC_MAGIC_DEF("readAsText", 1, fr_read, FM_TEXT),
    JS_CFUNC_MAGIC_DEF("readAsDataURL", 1, fr_read, FM_DATAURL),
    JS_CFUNC_DEF("abort", 0, fr_abort),
    JS_CGETSET_MAGIC_DEF("readyState", fr_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("result", fr_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("error", fr_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("onloadstart", fr_on_get, fr_on_set, FE_LOADSTART),
    JS_CGETSET_MAGIC_DEF("onprogress", fr_on_get, fr_on_set, FE_PROGRESS),
    JS_CGETSET_MAGIC_DEF("onload", fr_on_get, fr_on_set, FE_LOAD),
    JS_CGETSET_MAGIC_DEF("onabort", fr_on_get, fr_on_set, FE_ABORT),
    JS_CGETSET_MAGIC_DEF("onerror", fr_on_get, fr_on_set, FE_ERROR),
    JS_CGETSET_MAGIC_DEF("onloadend", fr_on_get, fr_on_set, FE_LOADEND),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, fr_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, fr_listen, 1),
    JS_PROP_INT32_DEF("EMPTY", 0, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("LOADING", 1, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("DONE", 2, JS_PROP_ENUMERABLE),
    WST_TAG("FileReader"),
};

static const JSCFunctionListEntry fr_static[] = {
    JS_PROP_INT32_DEF("EMPTY", 0, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("LOADING", 1, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("DONE", 2, JS_PROP_ENUMERABLE),
};


static const JSClassDef blob_classes[4] = {
    { .class_name = "Blob", .finalizer = blob_fin },
    { .class_name = "File", .finalizer = blob_fin },
    { .class_name = "BlobCursor", .finalizer = cur_fin },
    { .class_name = "FileReader", .finalizer = fr_fin, .gc_mark = fr_mark },
};

static void fix_enum(JSContext* ctx, JSValueConst proto, const JSCFunctionListEntry* l, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        JSAtom a;
        JSPropertyDescriptor d;
        if (l[i].def_type != JS_DEF_CGETSET && l[i].def_type != JS_DEF_CGETSET_MAGIC && l[i].def_type != JS_DEF_CFUNC)
            continue;
        a = JS_NewAtom(ctx, l[i].name);
        if (JS_GetOwnProperty(ctx, &d, proto, a) > 0) {
            JS_DefineProperty(ctx, proto, a, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE);
            JS_FreeValue(ctx, d.value);
            JS_FreeValue(ctx, d.getter);
            JS_FreeValue(ctx, d.setter);
        }
        JS_FreeAtom(ctx, a);
    }
}

static JSValue install_one(JSContext* ctx, JSValueConst global_obj, int i, JSClassID* cid, JSCFunction* ctor_fn,
    int len, const JSCFunctionListEntry* fl, int n, JSValueConst parent_proto, JSValueConst parent_ctor)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;
    JS_NewClassID(cid);
    if (!JS_IsRegisteredClass(rt, *cid))
        JS_NewClass(rt, *cid, &blob_classes[i]);
    proto = JS_IsUndefined(parent_proto) ? JS_NewObject(ctx) : JS_NewObjectProto(ctx, parent_proto);
    JS_SetPropertyFunctionList(ctx, proto, fl, n);
    fix_enum(ctx, proto, fl, n);
    ctor = JS_NewCFunction2(ctx, ctor_fn, blob_classes[i].class_name, len, JS_CFUNC_constructor, 0);
    if (!JS_IsUndefined(parent_ctor))
        JS_SetPrototype(ctx, ctor, parent_ctor);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, *cid, proto);
    JS_DefinePropertyValueStr(ctx, global_obj, blob_classes[i].class_name, JS_DupValue(ctx, ctor),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return ctor;
}

void js_blob_install(JSContext* ctx, JSValueConst global_obj)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue bc, bp, fc;
    if (!atom_iter) {
        JSValue sy = JS_GetPropertyStr(ctx, global_obj, "Symbol");
        JSValue it = JS_IsException(sy) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, sy, "iterator");
        atom_iter = JS_IsException(it) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, it);
        if (atom_iter == JS_ATOM_NULL && JS_HasException(ctx))
            dyn_exc_drop(ctx);
        JS_FreeValue(ctx, it);
        JS_FreeValue(ctx, sy);
    }
    brt_init(ctx, global_obj);
    bc = install_one(ctx, global_obj, 0, &blob_cid, blob_ctor, 0, blob_proto_m, countof(blob_proto_m), JS_UNDEFINED, JS_UNDEFINED);
    bp = JS_GetClassProto(ctx, blob_cid);
    fc = install_one(ctx, global_obj, 1, &file_cid, file_ctor, 2, file_proto_m, countof(file_proto_m), bp, bc);
    {
        BlobRt* br = brt(ctx);
        if (br && br->ctx == ctx && JS_IsUndefined(br->ctor[0])) {
            br->ctor[0] = JS_DupValue(ctx, bc);
            br->ctor[1] = JS_DupValue(ctx, fc);
        }
    }
    JS_FreeValue(ctx, bp);
    JS_FreeValue(ctx, bc);
    JS_FreeValue(ctx, fc);
    JS_NewClassID(&cur_cid);
    if (!JS_IsRegisteredClass(rt, cur_cid))
        JS_NewClass(rt, cur_cid, &blob_classes[2]);
    fc = install_one(ctx, global_obj, 3, &fr_cid, fr_ctor, 0, fr_proto_m, countof(fr_proto_m), JS_UNDEFINED, JS_UNDEFINED);
    JS_SetPropertyFunctionList(ctx, fc, fr_static, countof(fr_static));
    JS_FreeValue(ctx, fc);
}
