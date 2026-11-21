#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../dyn-hash.h"
#include "../dyn-par.h"
#include "dd.h"

#define SNAP_MAGIC "DYNADOC"
#define SNAP_END "DYNAEND"
#define SNAP_VERSION 1u
#define SNAP_HDR 32u
#define SNAP_SEC_HDR 16u
#define SNAP_FTR 24u
#define SNAP_K_CATALOG 1u
#define SNAP_K_COLL 2u
#define SNAP_MAX_SECTIONS 65536u

typedef struct {
    int fd;
    uint64_t off;
    uint32_t* crcs;
    uint32_t ncrc, capcrc;
    dd_err* e;
} snap_out;

static int full_write(snap_out* o, const void* p, size_t n)
{
    const uint8_t* b = p;
    while (n) {
        ssize_t w = write(o->fd, b, n > (1u << 30) ? (1u << 30) : n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return dd_fail(o->e, DD_E_IO, "snapshot write failed: %s", strerror(errno));
        }
        b += w;
        n -= (size_t)w;
        o->off += (uint64_t)w;
    }
    return 0;
}

static int write_section(snap_out* o, uint32_t kind, const uint8_t* p, size_t n)
{
    uint8_t h[SNAP_SEC_HDR];
    uint32_t crc = dyn_crc32c(p, n);
    if (o->ncrc == o->capcrc) {
        uint32_t nc = o->capcrc ? o->capcrc * 2 : 16;
        uint32_t* np = realloc(o->crcs, (size_t)nc * sizeof *np);
        if (!np)
            return dd_fail(o->e, DD_E_OOM, "out of memory");
        o->crcs = np;
        o->capcrc = nc;
    }
    o->crcs[o->ncrc++] = crc;
    dyn_bson_wr32(h, kind);
    dyn_bson_wr32(h + 4, (uint32_t)n);
    dyn_bson_wr32(h + 8, (uint32_t)((uint64_t)n >> 32));
    dyn_bson_wr32(h + 12, crc);
    if (full_write(o, h, sizeof h) < 0)
        return -1;
    return full_write(o, p, n);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int dd_snap_coll_meta(const dd_coll* c, dyn_bson_w* w)
{
    int i;
    dyn_bson_w_doc_begin(w);
    if (c->options)
        dyn_bson_w_value(w, "options", 7, (dyn_bson_val) { DYN_BSON_DOC, c->options, (uint32_t)c->options_n });
    dyn_bson_w_int32(w, "ringCapacity", 12, (int32_t)c->ring_cap);
    dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, "indexes", 7);
    for (i = 0; i < c->nix; i++) {
        dyn_bson_w_index_key(w, DYN_BSON_DOC, (uint32_t)i);
        dyn_bson_w_bytes(w, c->ix[i]->spec, c->ix[i]->spec_n);
    }
    dyn_bson_w_sub_end(w);
    dyn_bson_w_doc_end(w);
    return w->oom ? -1 : 0;
}

static int build_catalog(dyn_bson_w* w, const uint8_t* user, const dd_snap_coll* colls, int ncoll)
{
    int i;
    dyn_bson_w_doc_begin(w);
    dyn_bson_w_int32(w, "v", 1, (int32_t)SNAP_VERSION);
    dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, "collections", 11);
    for (i = 0; i < ncoll; i++) {
        char k[12];
        int kl = snprintf(k, sizeof k, "%d", i);
        dyn_bson_w_string(w, k, (size_t)kl, colls[i].name, colls[i].name_n);
    }
    dyn_bson_w_sub_end(w);
    if (user)
        dyn_bson_w_value(w, "user", 4, (dyn_bson_val) { DYN_BSON_DOC, user, dyn_bson_rd32(user) });
    dyn_bson_w_doc_end(w);
    return w->oom ? -1 : 0;
}

static int build_coll(dyn_bson_w* w, const dd_snap_coll* sc)
{
    uint32_t i, live = 0;
    for (i = 0; i < sc->ndocs; i++)
        if (sc->docs[i])
            live++;
    dyn_bson_w_doc_begin(w);
    dyn_bson_w_string(w, "name", 4, sc->name, sc->name_n);
    dyn_bson_w_int64(w, "count", 5, live);
    if (sc->meta)
        dyn_bson_w_value(w, "meta", 4, (dyn_bson_val) { DYN_BSON_DOC, sc->meta, dyn_bson_rd32(sc->meta) });
    dyn_bson_w_doc_end(w);
    for (i = 0; i < sc->ndocs; i++)
        if (sc->docs[i])
            dyn_bson_w_bytes(w, sc->docs[i], dyn_bson_rd32(sc->docs[i]));
    return w->oom ? -1 : 0;
}

static int fsync_dir(const char* path)
{
    char dir[4096];
    const char* slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    int fd, rc;
    if (!slash) {
        dir[0] = '.';
        n = 1;
    } else if (n == 0) {
        dir[0] = '/';
        n = 1;
    } else if (n < sizeof dir) {
        memcpy(dir, path, n);
    } else {
        return -1;
    }
    dir[n] = 0;
    fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    rc = fsync(fd);
    close(fd);
    return rc;
}

int dd_snap_write(const char* path, const uint8_t* catalog, const dd_snap_coll* colls, int ncoll, int do_fsync,
    uint64_t* bytes_out, dd_err* e)
{
    static unsigned seq;
    char tmp[4200];
    snap_out o;
    dyn_bson_w w;
    uint8_t hdr[SNAP_HDR], ftr[SNAP_FTR];
    int i, rc = -1;
    uint32_t nsec = (uint32_t)ncoll + 1;
    if (strlen(path) > 4000)
        return dd_fail(e, DD_E_BADVALUE, "snapshot path too long");
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%u", path, (long)getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
    memset(&o, 0, sizeof o);
    o.e = e;
    o.fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (o.fd < 0)
        return dd_fail(e, DD_E_IO, "cannot create snapshot file '%s': %s", tmp, strerror(errno));
    dyn_bson_w_init(&w);
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, SNAP_MAGIC, 8);
    dyn_bson_wr32(hdr + 8, SNAP_VERSION);
    dyn_bson_wr32(hdr + 12, 0);
    dyn_bson_wr64(hdr + 16, (uint64_t)now_ms());
    dyn_bson_wr32(hdr + 24, nsec);
    dyn_bson_wr32(hdr + 28, dyn_crc32c(hdr, 28));
    if (full_write(&o, hdr, sizeof hdr) < 0)
        goto fail;
    if (build_catalog(&w, catalog, colls, ncoll) < 0) {
        dd_fail(e, DD_E_OOM, "out of memory building snapshot catalog");
        goto fail;
    }
    if (write_section(&o, SNAP_K_CATALOG, w.p, w.n) < 0)
        goto fail;
    for (i = 0; i < ncoll; i++) {
        dyn_bson_w_reset(&w);
        if (build_coll(&w, &colls[i]) < 0) {
            dd_fail(e, DD_E_OOM, "out of memory building snapshot section for '%s'", colls[i].name);
            goto fail;
        }
        if (write_section(&o, SNAP_K_COLL, w.p, w.n) < 0)
            goto fail;
    }
    memcpy(ftr, SNAP_END, 8);
    dyn_bson_wr64(ftr + 8, o.off + SNAP_FTR);
    dyn_bson_wr32(ftr + 16, nsec);
    {
        uint8_t* cb = malloc((size_t)o.ncrc * 4 + 1);
        uint32_t k;
        if (!cb) {
            dd_fail(e, DD_E_OOM, "out of memory");
            goto fail;
        }
        for (k = 0; k < o.ncrc; k++)
            dyn_bson_wr32(cb + 4 * k, o.crcs[k]);
        dyn_bson_wr32(ftr + 20, dyn_crc32c(cb, (size_t)o.ncrc * 4));
        free(cb);
    }
    if (full_write(&o, ftr, sizeof ftr) < 0)
        goto fail;
    if (do_fsync && fsync(o.fd) < 0) {
        dd_fail(e, DD_E_IO, "snapshot fsync failed: %s", strerror(errno));
        goto fail;
    }
    if (close(o.fd) < 0) {
        o.fd = -1;
        dd_fail(e, DD_E_IO, "snapshot close failed: %s", strerror(errno));
        goto fail;
    }
    o.fd = -1;
    if (rename(tmp, path) < 0) {
        dd_fail(e, DD_E_IO, "cannot rename snapshot into '%s': %s", path, strerror(errno));
        goto fail;
    }
    if (do_fsync)
        fsync_dir(path);
    if (bytes_out)
        *bytes_out = o.off;
    rc = 0;
    goto out;
fail:
    if (o.fd >= 0)
        close(o.fd);
    unlink(tmp);
out:
    dyn_bson_w_free(&w);
    free(o.crcs);
    return rc;
}

static int bad(dd_err* e, const char* path, const char* what, int sec)
{
    if (sec >= 0)
        return dd_fail(e, DD_E_BADSNAPSHOT, "invalid snapshot file '%s': %s (section %d)", path, what, sec);
    return dd_fail(e, DD_E_BADSNAPSHOT, "invalid snapshot file '%s': %s", path, what);
}

static int read_all(const char* path, uint8_t** out, size_t* n, dd_err* e)
{
    struct stat st;
    uint8_t* buf;
    size_t got = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return dd_fail(e, DD_E_IO, "cannot open snapshot file '%s': %s", path, strerror(errno));
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return dd_fail(e, DD_E_IO, "snapshot '%s' is not a regular file", path);
    }
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        return dd_fail(e, DD_E_OOM, "out of memory reading snapshot");
    }
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    *out = buf;
    *n = got;
    return 0;
}

static int valid_doc_at(const uint8_t* p, size_t avail, uint32_t* len)
{
    dyn_bson_err be;
    uint32_t l;
    if (avail < 5)
        return 0;
    l = dyn_bson_rd32(p);
    if (l < 5 || l > avail || l > DD_MAX_DOC)
        return 0;
    if (dyn_bson_validate(p, l, 100, &be) != 0)
        return 0;
    *len = l;
    return 1;
}

#define SNAP_PAR_MIN_DOCS 4096

typedef struct {
    dd_coll* c;
    int first, n;
    dd_err* err;
    int* rc;
} snap_fill;

static int bad_index(dd_err* e, const char* path, const dd_err* ie, int sec)
{
    char m[400];
    if (ie->code == DD_E_OOM)
        return dd_fail(e, DD_E_OOM, "out of memory");
    snprintf(m, sizeof m, "index rebuild failed: %s", ie->msg);
    return bad(e, path, m, sec);
}

typedef struct {
    const uint8_t* p;
    size_t* off;
    size_t n, cap;
    int oom;
    unsigned nw;
    size_t* firstbad;
    dyn_ks_buf* kb;
    uint32_t* koff;
    uint32_t* klen;
    uint64_t* kh;
} snap_dv;

static void snap_validate_worker(void* arg, unsigned worker, unsigned nworkers, dyn_par_t* par)
{
    snap_dv* v = arg;
    size_t i, lo = v->n * worker / nworkers, hi = v->n * (worker + 1) / nworkers;
    dyn_ks_buf* kb = &v->kb[worker];
    (void)par;
    v->firstbad[worker] = SIZE_MAX;
    for (i = lo; i < hi; i++) {
        dyn_bson_err be;
        dyn_bson_val id;
        const uint8_t* d = v->p + v->off[i];
        size_t from = kb->n;
        if (dyn_bson_validate(d, dyn_bson_rd32(d), 100, &be) != 0) {
            v->firstbad[worker] = i;
            return;
        }
        v->klen[i] = UINT32_MAX;
        if (!dyn_bson_find(d, "_id", 3, &id))
            continue;
        dyn_ks_value(kb, id, 0);
        if (kb->oom || kb->n - from >= UINT32_MAX)
            continue;
        v->koff[i] = (uint32_t)from;
        v->klen[i] = (uint32_t)(kb->n - from);
        v->kh[i] = dd_hash_bytes(kb->p + from, kb->n - from);
    }
}

static void snap_dv_free(snap_dv* v)
{
    unsigned w;
    for (w = 0; w < 64 && v->kb; w++)
        dyn_ks_free(&v->kb[w]);
    free(v->kb);
    free(v->koff);
    free(v->klen);
    free(v->kh);
    free(v->off);
    memset(v, 0, sizeof *v);
}

static int snap_docs(const uint8_t* p, size_t from, size_t len, snap_dv* v)
{
    size_t q = from, bads[64];
    unsigned w, nw;
    v->p = p;
    v->n = 0;
    v->oom = 0;
    while (q < len) {
        uint32_t l;
        if (len - q < 5)
            return -1;
        l = dyn_bson_rd32(p + q);
        if (l < 5 || l > len - q || l > DD_MAX_DOC)
            return -1;
        if (v->n == v->cap) {
            size_t nc = v->cap ? v->cap * 2 : 1024;
            size_t* no = realloc(v->off, nc * sizeof *no);
            if (!no) {
                v->oom = 1;
                return -1;
            }
            v->off = no;
            v->cap = nc;
        }
        v->off[v->n++] = q;
        q += l;
    }
    nw = v->n >= SNAP_PAR_MIN_DOCS ? dyn_par_plan(v->n, SNAP_PAR_MIN_DOCS / 4, 64) : 1;
    v->firstbad = bads;
    {
        size_t m = v->n ? v->n : 1;
        uint32_t* nk = realloc(v->koff, m * sizeof *nk);
        uint32_t* nl = nk ? realloc(v->klen, m * sizeof *nl) : NULL;
        uint64_t* nh = nl ? realloc(v->kh, m * sizeof *nh) : NULL;
        if (nk)
            v->koff = nk;
        if (nl)
            v->klen = nl;
        if (nh)
            v->kh = nh;
        if (!nh) {
            v->oom = 1;
            return -1;
        }
    }
    if (!v->kb)
        v->kb = calloc(64, sizeof *v->kb);
    if (!v->koff || !v->klen || !v->kh || !v->kb) {
        v->oom = 1;
        return -1;
    }
    for (w = 0; w < 64; w++) {
        v->kb[w].n = 0;
        v->kb[w].oom = 0;
    }
    nw = dyn_par_run(nw, snap_validate_worker, v);
    v->nw = nw;
    for (w = 0; w < nw; w++)
        if (bads[w] != SIZE_MAX)
            return -1;
    return 0;
}

static int load_indexes(dd_coll* c, const uint8_t* meta, const char* path, int sec, dd_err* e)
{
    dyn_bson_val v, ixs;
    dyn_bson_iter it;
    if (!dyn_bson_find(meta, "options", 7, &v) || v.type != DYN_BSON_DOC)
        v.type = 0;
    if (v.type) {
        c->options = malloc(v.vlen);
        if (!c->options)
            return dd_fail(e, DD_E_OOM, "out of memory");
        memcpy(c->options, v.val, v.vlen);
        c->options_n = v.vlen;
    }
    if (dyn_bson_find(meta, "ringCapacity", 12, &v) && v.type == DYN_BSON_INT32) {
        int32_t rc = (int32_t)dyn_bson_rd32(v.val);
        if (rc > 0)
            c->ring_cap = (uint32_t)rc;
    }
    if (!dyn_bson_find(meta, "indexes", 7, &ixs))
        return 0;
    if (ixs.type != DYN_BSON_ARRAY)
        return bad(e, path, "index list is not an array", sec);
    {
        snap_fill f;
        int first = c->nix, rc = 0, i;
        dyn_bson_iter_init(&it, ixs.val, ixs.vlen);
        while (dyn_bson_iter_next(&it) > 0) {
            dyn_bson_val key;
            dd_index* ix;
            dd_err ie;
            if (it.type != DYN_BSON_DOC || !dyn_bson_find(it.val, "key", 3, &key) || key.type != DYN_BSON_DOC)
                return bad(e, path, "index spec without a key document", sec);
            ie.code = 0;
            if (dd_index_create_unfilled(c, key.val, it.val, &ix, &ie) < 0)
                return bad_index(e, path, &ie, sec);
        }
        f.c = c;
        f.first = first;
        f.n = c->nix - first;
        if (f.n == 0)
            return 0;
        f.err = calloc((size_t)f.n, sizeof *f.err);
        f.rc = calloc((size_t)f.n, sizeof *f.rc);
        if (!f.err || !f.rc) {
            free(f.err);
            free(f.rc);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
        dd_index_fill_many(c, c->ix + first, f.n, f.err, f.rc);
        for (i = 0; i < f.n && rc == 0; i++)
            if (f.rc[i] < 0)
                rc = bad_index(e, path, &f.err[i], sec);
        free(f.err);
        free(f.rc);
        return rc;
    }
}

static void free_loaded(dd_snap_loaded* out)
{
    int i;
    for (i = 0; i < out->ncoll; i++)
        dd_coll_free(out->colls[i]);
    free(out->colls);
    free(out->catalog);
    memset(out, 0, sizeof *out);
}

int dd_snap_read(const char* path, dd_db* owner, dd_snap_loaded* out, dd_err* e)
{
    uint8_t* buf = NULL;
    size_t n = 0, pos;
    uint32_t nsec, s, *crcs = NULL;
    const uint8_t* cat = NULL;
    size_t cat_n = 0;
    int rc = -1;
    uint32_t ncat_names = 0;
    snap_dv dv;
    memset(&dv, 0, sizeof dv);
    memset(out, 0, sizeof *out);
    if (read_all(path, &buf, &n, e) < 0)
        return -1;
    if (n < SNAP_HDR + SNAP_FTR) {
        bad(e, path, "file is truncated (shorter than header + footer)", -1);
        goto done;
    }
    if (memcmp(buf, SNAP_MAGIC, 8) != 0) {
        bad(e, path, "bad header magic", -1);
        goto done;
    }
    if (dyn_bson_rd32(buf + 28) != dyn_crc32c(buf, 28)) {
        bad(e, path, "header checksum mismatch", -1);
        goto done;
    }
    if (dyn_bson_rd32(buf + 8) != SNAP_VERSION) {
        bad(e, path, "unsupported version", -1);
        goto done;
    }
    nsec = dyn_bson_rd32(buf + 24);
    if (nsec == 0 || nsec > SNAP_MAX_SECTIONS) {
        bad(e, path, "bad section count", -1);
        goto done;
    }
    if (memcmp(buf + n - SNAP_FTR, SNAP_END, 8) != 0) {
        bad(e, path, "missing footer (file truncated?)", -1);
        goto done;
    }
    if (dyn_bson_rd64(buf + n - SNAP_FTR + 8) != n) {
        bad(e, path, "length mismatch (file truncated or extended)", -1);
        goto done;
    }
    if (dyn_bson_rd32(buf + n - SNAP_FTR + 16) != nsec) {
        bad(e, path, "footer section count mismatch", -1);
        goto done;
    }
    crcs = malloc((size_t)nsec * 4);
    out->colls = calloc(nsec, sizeof *out->colls);
    if (!crcs || !out->colls) {
        dd_fail(e, DD_E_OOM, "out of memory");
        goto done;
    }
    pos = SNAP_HDR;
    for (s = 0; s < nsec; s++) {
        uint32_t kind, crc;
        uint64_t len;
        const uint8_t* p;
        if (n - SNAP_FTR - pos < SNAP_SEC_HDR) {
            bad(e, path, "section header truncated", (int)s);
            goto done;
        }
        kind = dyn_bson_rd32(buf + pos);
        len = (uint64_t)dyn_bson_rd32(buf + pos + 4) | ((uint64_t)dyn_bson_rd32(buf + pos + 8) << 32);
        crc = dyn_bson_rd32(buf + pos + 12);
        pos += SNAP_SEC_HDR;
        if (len > n - SNAP_FTR - pos) {
            bad(e, path, "section payload truncated", (int)s);
            goto done;
        }
        p = buf + pos;
        if (dyn_crc32c(p, (size_t)len) != crc) {
            bad(e, path, "section checksum mismatch", (int)s);
            goto done;
        }
        crcs[s] = crc;
        pos += (size_t)len;
        if ((s == 0) != (kind == SNAP_K_CATALOG) || (kind != SNAP_K_CATALOG && kind != SNAP_K_COLL)) {
            bad(e, path, "unexpected section kind", (int)s);
            goto done;
        }
        if (kind == SNAP_K_CATALOG) {
            uint32_t l;
            dyn_bson_val v;
            if (!valid_doc_at(p, (size_t)len, &l) || l != len) {
                bad(e, path, "catalog is not a valid BSON document", (int)s);
                goto done;
            }
            cat = p;
            cat_n = l;
            if (!dyn_bson_find(cat, "collections", 11, &v) || v.type != DYN_BSON_ARRAY) {
                bad(e, path, "catalog has no collection list", (int)s);
                goto done;
            }
            {
                dyn_bson_iter it;
                dyn_bson_iter_init(&it, v.val, v.vlen);
                while (dyn_bson_iter_next(&it) > 0)
                    ncat_names++;
            }
            continue;
        }
        {
            uint32_t ml, dl;
            size_t q;
            dyn_bson_val nm, cnt, meta;
            dd_coll* c;
            uint64_t docs = 0;
            dd_err ce;
            unsigned w;
            int k;
            if (!valid_doc_at(p, (size_t)len, &ml)) {
                bad(e, path, "collection header is not a valid BSON document", (int)s);
                goto done;
            }
            if (!dyn_bson_find(p, "name", 4, &nm) || nm.type != DYN_BSON_STRING
                || !dyn_bson_find(p, "count", 5, &cnt) || cnt.type != DYN_BSON_INT64) {
                bad(e, path, "collection header lacks name/count", (int)s);
                goto done;
            }
            ce.code = 0;
            if (dd_coll_name_ok((const char*)nm.val + 4, dyn_bson_rd32(nm.val) - 1, &ce) < 0) {
                bad(e, path, "invalid collection name", (int)s);
                goto done;
            }
            for (k = 0; k < out->ncoll; k++)
                if (out->colls[k]->name_n == dyn_bson_rd32(nm.val) - 1
                    && memcmp(out->colls[k]->name, nm.val + 4, out->colls[k]->name_n) == 0) {
                    bad(e, path, "duplicate collection", (int)s);
                    goto done;
                }
            c = dd_coll_new(owner, (const char*)nm.val + 4, dyn_bson_rd32(nm.val) - 1);
            if (!c) {
                dd_fail(e, DD_E_OOM, "out of memory");
                goto done;
            }
            out->colls[out->ncoll++] = c;
            if (snap_docs(p, ml, (size_t)len, &dv) < 0) {
                if (dv.oom)
                    dd_fail(e, DD_E_OOM, "out of memory");
                else
                    bad(e, path, "document is not valid BSON", (int)s);
                goto done;
            }
            if (dv.n > UINT32_MAX / 8 || dd_coll_reserve(c, (uint32_t)dv.n) < 0) {
                dd_fail(e, DD_E_OOM, "out of memory");
                goto done;
            }
            for (q = 0, w = 0; q < dv.n; q++) {
                uint32_t dno;
                int r;
                while (q >= dv.n * (w + 1) / dv.nw)
                    w++;
                dl = dyn_bson_rd32(p + dv.off[q]);
                ce.code = 0;
                r = dv.klen[q] == UINT32_MAX
                    ? dd_coll_insert(c, p + dv.off[q], dl, &dno, &ce)
                    : dd_coll_load_insert(c, p + dv.off[q], dl, dv.kb[w].p + dv.koff[q], dv.klen[q], dv.kh[q], &ce);
                if (r < 0) {
                    char m[400];
                    snprintf(m, sizeof m, "document rejected: %s", ce.msg);
                    if (ce.code == DD_E_OOM)
                        dd_fail(e, DD_E_OOM, "out of memory");
                    else
                        bad(e, path, m, (int)s);
                    goto done;
                }
                docs++;
            }
            if (docs != dyn_bson_rd64(cnt.val)) {
                bad(e, path, "document count does not match the collection header", (int)s);
                goto done;
            }
            out->docs += docs;
            if (dyn_bson_find(p, "meta", 4, &meta) && meta.type == DYN_BSON_DOC
                && load_indexes(c, meta.val, path, (int)s, e) < 0)
                goto done;
        }
    }
    if (pos != n - SNAP_FTR) {
        bad(e, path, "trailing bytes before the footer", -1);
        goto done;
    }
    {
        uint32_t k;
        for (k = 0; k < nsec; k++)
            dyn_bson_wr32((uint8_t*)crcs + 4 * k, crcs[k]);
        if (dyn_crc32c((const uint8_t*)crcs, (size_t)nsec * 4) != dyn_bson_rd32(buf + n - SNAP_FTR + 20)) {
            bad(e, path, "footer checksum mismatch", -1);
            goto done;
        }
    }
    if (ncat_names != (uint32_t)out->ncoll) {
        bad(e, path, "catalog lists a different number of collections than the file holds", -1);
        goto done;
    }
    out->catalog = malloc(cat_n);
    if (!out->catalog) {
        dd_fail(e, DD_E_OOM, "out of memory");
        goto done;
    }
    memcpy(out->catalog, cat, cat_n);
    out->catalog_n = cat_n;
    out->bytes = n;
    rc = 0;
done:
    if (rc < 0)
        free_loaded(out);
    snap_dv_free(&dv);
    free(crcs);
    free(buf);
    return rc;
}

void dd_snap_loaded_free(dd_snap_loaded* out)
{
    free_loaded(out);
}
