/*
 * dyna:csv -- RFC 4180 reader/writer with a streaming file reader.
 *
 * Field scanning is scalar on purpose: a SIMD find_first_of measured 15-20%
 * SLOWER because fields are too short to amortise the dispatch call.
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "dyna-simd-kernels.h"
#include "cutils.h"            /* swar_has_byte: inline SWAR for the quoted scan */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ============================ growable byte buffer ============================ */
typedef struct { char *p; size_t len, cap; } Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra) nc *= 2;
    char *np = (char *)realloc(b->p, nc);
    if (!np) return -1;
    b->p = np; b->cap = nc;
    return 0;
}
static int buf_put(Buf *b, const char *s, size_t n) {
    if (buf_reserve(b, n)) return -1;
    memcpy(b->p + b->len, s, n); b->len += n; return 0;
}
static int buf_putc(Buf *b, char c) {
    if (buf_reserve(b, 1)) return -1;
    b->p[b->len++] = c; return 0;
}
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ============================ CSV table (jagged) ============================ */
/* Row 0 is the header. A cell is a nul-terminated string owned by the table's
 * ARENA, not individually: parsing allocated one malloc per cell (10M for a
 * 1M x 10 file), which dominated ingest. Cells are carved from a block chain;
 * replaced/removed cells stay in the arena until table_free, so mutations
 * must NOT free() them. A NULL cell pointer denotes the empty string. */
typedef struct csv_ablock {
    struct csv_ablock *next;
    size_t off, cap;
    char data[];
} csv_ablock;
typedef struct { char **f; size_t n, cap; } Row;
typedef struct { Row *r; size_t n, cap; csv_ablock *arena; } Table;

/* Carve a nul-terminated copy of s[0..n) from the arena; NULL for n == 0. */
#define CSV_ARENA_BLOCK 65536
static char *tcell_dup(Table *t, const char *s, size_t n) {
    if (n == 0) return NULL;
    if (n + 1 > CSV_ARENA_BLOCK) {
        /* An exact-fit block is FULL on arrival, so making it the head would
           orphan whatever room the current head still has and force a fresh
           64 KiB block for the next short cell. Splice it behind the head
           instead: measured 46 MB of waste on a 140 MB file with one blob
           column and one short one. */
        csv_ablock *b = (csv_ablock *)malloc(sizeof(*b) + n + 1);
        char *d;
        if (!b) return NULL;
        b->cap = n + 1;
        b->off = n + 1;
        if (t->arena) { b->next = t->arena->next; t->arena->next = b; }
        else          { b->next = NULL; t->arena = b; }
        d = b->data;
        memcpy(d, s, n); d[n] = 0;
        return d;
    }
    if (!t->arena || t->arena->off + n + 1 > t->arena->cap) {
        csv_ablock *b = (csv_ablock *)malloc(sizeof(*b) + CSV_ARENA_BLOCK);
        if (!b) return NULL;
        b->next = t->arena; b->off = 0; b->cap = CSV_ARENA_BLOCK;
        t->arena = b;
    }
    char *d = t->arena->data + t->arena->off;
    memcpy(d, s, n); d[n] = 0;
    t->arena->off += n + 1;
    return d;
}

static int row_push(Row *row, char *s) {
    if (row->n == row->cap) {
        size_t nc = row->cap ? row->cap * 2 : 8;
        char **nv = (char **)realloc(row->f, nc * sizeof(char *));
        if (!nv) return -1;
        row->f = nv; row->cap = nc;
    }
    row->f[row->n++] = s; return 0;
}
static int table_push(Table *t, Row row) {
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        Row *nr = (Row *)realloc(t->r, nc * sizeof(Row));
        if (!nr) return -1;
        t->r = nr; t->cap = nc;
    }
    t->r[t->n++] = row; return 0;
}
static void table_free(Table *t) {
    for (size_t i = 0; i < t->n; i++) free(t->r[i].f);   /* the row arrays, not the cells */
    free(t->r);
    while (t->arena) { csv_ablock *b = t->arena; t->arena = b->next; free(b); }
    t->r = NULL; t->n = t->cap = 0;
}
/* header column count */
static size_t table_ncols(const Table *t) { return t->n ? t->r[0].n : 0; }
/* a cell as a C string ("" for missing/NULL) */
static const char *cell(const Table *t, size_t r, size_t c) {
    if (r >= t->n || c >= t->r[r].n || !t->r[r].f[c]) return "";
    return t->r[r].f[c];
}

/* ============================ RFC-4180 parser (SIMD) ============================ */
/* Strict-mode syntax codes (returned by csv_parse_ex only when `strict`). */
#define CSV_SYNTAX_QUOTE    1   /* a quoted field still open at EOF */
#define CSV_SYNTAX_GARBAGE  2   /* text after a closing quote */
/* Parse `buf`/`len` into `t`. Returns 0, or -1 on OOM (t is freed). In strict
 * mode (csv_parse_ex) also returns a CSV_SYNTAX_* code with *errrow set to the
 * 1-based row (the header is row 1); t then still holds the rows parsed so far
 * and the CALLER frees it. The default is tolerant: an unterminated quote
 * commits its partial data and text after a closing quote ends the record
 * (the known, disclosed RFC-4180 divergences); `{strict:true}` on the readers
 * turns both into named syntax errors instead.
 * the field delimiter and quote characters are parameters (single bytes,
 * validated by opts_delim_quote before they get here); the module defaults are
 * ',' and '"'. */
static int csv_parse_ex(const uint8_t *buf, size_t len, Table *t, int strict,
                        int *errrow, uint8_t delim, uint8_t quote) {
    uint8_t stop[3] = { delim, '\n', '\r' };   /* unquoted field terminators */
    size_t i = 0;
    memset(t, 0, sizeof(*t));
    if (len == 0) return 0;

    /* pre-size the row vector from a SIMD newline count (an upper bound; embedded
     * newlines in quoted fields only make it an over-estimate, which is fine). */
    size_t est = simd.count_u8(buf, '\n', len) + 1;
    t->cap = est < 64 ? 64 : est;
    t->r = (Row *)malloc(t->cap * sizeof(Row));
    if (!t->r) return -1;

    Row row; memset(&row, 0, sizeof(row));
    Buf fld; memset(&fld, 0, sizeof(fld));

    for (;;) {
        /* --- one field --- */
        int closed = 0;
        fld.len = 0;
        if (i < len && buf[i] == quote) {         /* quoted field */
            i++;
            while (i < len) {
                /* Inline SWAR x8 over the quoted body (cutils.h swar_has_byte,
                 * the CLAUDE.md sec-9 "inline SWAR at short-to-medium spans"
                 * shape): the gap between quotes runs ~30 B, which amortizes an
                 * 8-byte block scan but NOT a kernel call (the deep sweep
                 * measured find_u8('"') runs at 0.93x -- per-call overhead --
                 * and this inline form at 1.35-1.39x on the same corpus).
                 * Clean 8-byte blocks bulk-append; a block containing a quote
                 * is rescanned byte-wise from `i` (endian-independent, the
                 * cutils.h contract) and the hit position then decides
                 * escaped-quote vs close exactly like the scalar loop it
                 * replaced. */
                while (i + 8 <= len) {
                    uint64_t v;
                    memcpy(&v, buf + i, 8);
                    if (!swar_has_byte(v, quote)) {
                        if (buf_put(&fld, (const char *)buf + i, 8)) goto oom;
                        i += 8;
                        continue;
                    }
                    while (buf[i] != quote) {     /* hit block: byte-wise up to
                                                     the quote (bounded by 8) */
                        if (buf_putc(&fld, (char)buf[i])) goto oom;
                        i++;
                    }
                    goto quote;
                }
                while (i < len && buf[i] != quote) { /* scalar tail: <8 B left */
                    if (buf_putc(&fld, (char)buf[i])) goto oom;
                    i++;
                }
                if (i >= len) break;              /* no closing quote before EOF */
            quote:
                if (i + 1 < len && buf[i + 1] == quote) {  /* escaped quote */
                    if (buf_putc(&fld, (char)quote)) goto oom;
                    i += 2;
                    continue;
                }
                i++;
                closed = 1;
                break;                            /* closing quote */
            }
            if (strict) {
                if (!closed) { *errrow = (int)t->n + 1; goto syntax_quote; }
                /* after a closing quote only a delimiter may follow */
                if (i < len && buf[i] != delim && buf[i] != '\n' && buf[i] != '\r') {
                    *errrow = (int)t->n + 1; goto syntax_garbage;
                }
            }
        } else {                                    /* unquoted field: SIMD jump to next terminator */
            size_t p = simd.find_first_of(buf + i, len - i, stop, countof(stop));
            if (p == SIZE_MAX) p = len - i;
            if (buf_put(&fld, (const char *)(buf + i), p)) goto oom;
            i += p;
        }
        /* commit the field (NULL for empty to avoid tiny allocs) */
        {
            char *s = NULL;
            if (fld.len) { s = tcell_dup(t, fld.p, fld.len); if (!s) goto oom; }
            if (row_push(&row, s)) goto oom;
        }
        /* --- delimiter / record end / EOF --- */
        if (i >= len) { if (table_push(t, row)) goto oom; memset(&row, 0, sizeof(row)); break; }
        {
            uint8_t c = buf[i];
            if (c == delim) { i++; continue; }
            /* record terminator (handle CRLF) */
            if (c == '\r' && i + 1 < len && buf[i + 1] == '\n') i += 2; else i++;
            if (table_push(t, row)) goto oom;
            memset(&row, 0, sizeof(row));
            if (i >= len) break;                    /* trailing newline: no spurious empty row */
        }
    }
    buf_free(&fld);
    return 0;
syntax_quote:
    buf_free(&fld);
    free(row.f);            /* cells are arena-owned; t stays for the caller */
    return CSV_SYNTAX_QUOTE;
syntax_garbage:
    buf_free(&fld);
    free(row.f);            /* cells are arena-owned; t stays for the caller */
    return CSV_SYNTAX_GARBAGE;
oom:
    buf_free(&fld);
    free(row.f);            /* cells are arena-owned: table_free covers them */
    table_free(t);
    return -1;
}
/* The tolerant parse (historical behavior, also the fuzz entry point, which
 * includes this file). The reader calls csv_parse_ex directly; nothing in the
 * module itself needs the tolerant wrapper outside fuzz builds, hence the
 * unused-attribute that keeps the 0-warning gate honest about it. */
__attribute__((unused)) static int csv_parse(const uint8_t *buf, size_t len,
                                             Table *t) {
    int rr;
    return csv_parse_ex(buf, len, t, 0, &rr, /*delim=*/',', /*quote=*/'"') < 0 ? -1 : 0;
}

/* ============================ serializer ============================ */
/*the delimiter and quote are parameters. The quote char is the only one
 * that doubles inside an emitted field; a field needs quoting when it contains
 * the delimiter, the quote, CR or LF. */
static int field_needs_quote(const char *s, uint8_t delim, uint8_t quote) {
    for (const char *p = s; *p; p++)
        if (*p == (char)delim || *p == (char)quote || *p == '\n' || *p == '\r') return 1;
    return 0;
}
static int emit_field(Buf *b, const char *s, uint8_t delim, uint8_t quote) {
    if (!field_needs_quote(s, delim, quote)) return buf_put(b, s, strlen(s));
    if (buf_putc(b, (char)quote)) return -1;
    for (const char *p = s; *p; p++) {
        if (*p == (char)quote && buf_putc(b, (char)quote)) return -1;
        if (buf_putc(b, *p)) return -1;
    }
    return buf_putc(b, (char)quote);
}
static int csv_serialize_dq(const Table *t, Buf *out, uint8_t delim, uint8_t quote) {
    for (size_t r = 0; r < t->n; r++) {
        size_t nc = t->r[r].n;
        for (size_t c = 0; c < nc; c++) {
            if (c && buf_putc(out, (char)delim)) return -1;
            if (emit_field(out, t->r[r].f[c] ? t->r[r].f[c] : "", delim, quote)) return -1;
        }
        if (buf_putc(out, '\n')) return -1;
    }
    return 0;
}
/* The module-default serializer (',' / '"'). Kept as a distinct entry point:
 * fuzz_csv.c includes this file and round-trips through it. */
static int csv_serialize(const Table *t, Buf *out) {
    return csv_serialize_dq(t, out, ',', '"');
}

/* ==================== file I/O (shared dyn_io_* primitives) ====================
 * Reads go through dyn_io_slurp (a zero-copy mmap view for large files, an
 * advise-hinted heap read otherwise); writes go through dyn_io_write_whole_atomic
 * (temp file + durable fsync + rename) -- the shared io-core primitives that
 * dyna:file and the engine's own loader use. */

/* Create parent directories of `path` (mkdir -p of the dirname). */
static void csv_mkparents(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    free(tmp);
}
/* Atomic write via the shared io core (temp file + rename over `path`).
 * `durable` controls whether the temp file is fsync'd to stable storage before
 * the rename (a per-edit fsync on a batch of K edits costs K fsyncs; the
 * temp+rename already prevents a torn/corrupt file on a process crash, so the
 * fsync only buys power-loss durability and is therefore opt-in per edit). */
static int csv_write_atomic(const char *path, const char *data, size_t len, int durable) {
    return dyn_io_write_whole_atomic(path, data, len, durable);
}

/* ============================ JS option helpers ============================ */
/* obj[key] as an owned C string, or NULL (absent/undefined). *present set. */
static char *opt_str(JSContext *ctx, JSValueConst obj, const char *key, int *present) {
    if (present) *present = 0;
    if (!JS_IsObject(obj)) return NULL;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return NULL; }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return NULL;
    char *d = strdup(s);
    JS_FreeCString(ctx, s);
    if (present) *present = 1;
    return d;
}
/* obj[key] as an int with a default; sets *present if the key was given. */
static int opt_int(JSContext *ctx, JSValueConst obj, const char *key, int64_t def,
                   int64_t *out, int *present) {
    if (present) *present = 0;
    if (!JS_IsObject(obj)) { *out = def; return 0; }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); *out = def; return 0; }
    int r = JS_ToInt64(ctx, out, v);
    JS_FreeValue(ctx, v);
    if (r) return -1;
    if (present) *present = 1;
    return 0;
}
static int opt_bool_def(JSContext *ctx, JSValueConst obj, const char *key, int def) {
    if (!JS_IsObject(obj)) return def;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return def; }
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}
static int opt_bool(JSContext *ctx, JSValueConst obj, const char *key) {
    return opt_bool_def(ctx, obj, key, 0);
}
/* The one READER option: `{strict:true}` makes the two known tolerant-parse
 * divergences -- garbage after a closing quote, an unterminated quote at EOF
 * -- throw a syntax error naming the row (see csv_parse_ex). The default (0)
 * stays tolerant, which is the documented historical behavior. */
/* Per-edit durability is opt-in: a batch of K edits fsyncs K times when every
 * mutation requests it. The temp+rename already makes each edit crash-safe (no
 * torn file), so the fsync only buys power-loss durability and `{durable:true}`
 * is the explicit ask. Default is non-durable (0). */
static int opt_durable(JSContext *ctx, JSValueConst obj) {
    return opt_bool(ctx, obj, "durable");
}
/* obj[key] as an owned array of owned C strings. Returns count, or -1 (throwing).
 * *arr NULL if the key is absent (returns 0). */
static int opt_str_array(JSContext *ctx, JSValueConst obj, const char *key, char ***arr) {
    *arr = NULL;
    if (!JS_IsObject(obj)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return 0; }
    if (!JS_IsArray(ctx, v)) { JS_FreeValue(ctx, v); JS_ThrowTypeError(ctx, "csv: '%s' must be an array", key); return -1; }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    char **out = n ? (char **)calloc(n, sizeof(char *)) : NULL;
    if (n && !out) { JS_FreeValue(ctx, v); JS_ThrowOutOfMemory(ctx); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        const char *s = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!s) { for (uint32_t j = 0; j < i; j++) free(out[j]); free(out); JS_FreeValue(ctx, v); return -1; }
        out[i] = strdup(s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    *arr = out;
    return (int)n;
}
static void free_str_array(char **a, int n) { if (a) { for (int i = 0; i < n; i++) free(a[i]); free(a); } }

/* ==================== strict options (the pilot) ====================
 * Until now an unknown option key was silently ignored -- `{bytes:true}`-grade
 * bugs: the call "worked" and did the slow/wrong thing. This module is the
 * rollout PILOT: every options bag is checked against the calling method's
 * valid-key table and an unknown key throws a TypeError naming the key AND the
 * full valid set. The table lives at the call site (opts_check just below each
 * method's entry), so a new option is added to exactly one list.
 *
 * Memory split (src/dyna-nat.h rule): the JSPropertyEnum table and its atoms
 * are ENGINE allocations -- freed with JS_FreePropertyEnum (js_free inside);
 * the key text JS_AtomToCString hands back is released with JS_FreeCString.
 * Nothing here goes through libc free. */
static int opts_check(JSContext *ctx, JSValueConst obj, const char *pfx,
                      const char *const *keys, size_t nkeys) {
    JSPropertyEnum *tab = NULL;
    uint32_t n, i;
    if (!JS_IsObject(obj)) return 0;    /* an absent bag has no unknown keys */
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, obj, JS_GPN_STRING_MASK) < 0)
        return -1;                      /* a throwing getter on the bag propagates */
    for (i = 0; i < n; i++) {
        const char *key = JS_AtomToCString(ctx, tab[i].atom);
        int known;
        if (!key) { JS_FreePropertyEnum(ctx, tab, n); return -1; }
        known = 0;
        for (size_t k = 0; k < nkeys; k++)
            if (strcmp(key, keys[k]) == 0) { known = 1; break; }
        if (!known) {
            Buf vb; memset(&vb, 0, sizeof(vb));
            for (size_t k = 0; k < nkeys; k++) {
                if ((k && buf_put(&vb, ", ", 2)) || buf_put(&vb, keys[k], strlen(keys[k]))) break;
            }
            /* %s over the Buf needs a C string: NUL-terminate it or the
             * format's strlen reads past the allocation (the ASan harness
             * caught exactly that). On append-OOM drop the list and format
             * an empty one. */
            if (buf_putc(&vb, '\0')) { buf_free(&vb); vb.p = NULL; }
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\" (valid: %s)",
                              pfx, key, vb.p ? vb.p : "");
            buf_free(&vb);
            JS_FreePropertyEnum(ctx, tab, n);
            /* the key text must go back through JS_FreeCString on THIS path
             * too: JS_AtomToCString may return an interior pointer into the
             * owning JSString (zero-copy ASCII fast path), so skipping the
             * free pins one engine string per rejected bag -- a leak under a
             * `while (true) { try { f.read({x: 1}) } catch {} }` loop. */
            JS_FreeCString(ctx, key);
            return -1;
        }
        JS_FreeCString(ctx, key);
    }
    JS_FreePropertyEnum(ctx, tab, n);
    return 0;
}

/* ====================: {delimiter, quote} ====================
 * Both default to the RFC-4180 pair (',' and '"'). Each must be exactly one
 * ASCII code unit (a multi-byte UTF-8 character cannot back a byte-delimited
 * grammar), neither may be CR or LF (they end records and can never be field
 * syntax), and the canonical roles are reserved: the delimiter can never be
 * the quote character and vice versa -- swapping the two keys is always a
 * caller bug, so {delimiter:'"'} and {quote:','} are refused outright. This is
 * what makes TSV / semicolon files work while keeping one quoting rule. */
/* One single-character option key: exactly one ASCII code unit, never CR or
 * LF, and never the other role's canonical character (delimiter != quote).
 * Both keys share the whole rule set, so the checks -- and the one strlen
 * the validation needs -- live here, measured once per string (the
 * hoisted-length rule). Consumes s (frees it on every path). Returns 0, or
 * -1 with the TypeError pending. Messages are byte-identical to the
 * pre-dedup per-key ones. */
static int opts_one_char(JSContext *ctx, const char *pfx, const char *key,
                         char *s, uint8_t *out,
                         uint8_t reserved, const char *reserved_role) {
    size_t slen = strlen(s);
    if (slen != 1) { JS_ThrowTypeError(ctx, "%s: '%s' must be exactly one ASCII character", pfx, key); free(s); return -1; }
    if (s[0] == (char)reserved) { JS_ThrowTypeError(ctx, "%s: '%s' must not be the %s character '%c'", pfx, key, reserved_role, reserved); free(s); return -1; }
    if (s[0] == '\n' || s[0] == '\r') { JS_ThrowTypeError(ctx, "%s: '%s' must not be CR or LF", pfx, key); free(s); return -1; }
    *out = (uint8_t)s[0];
    free(s);
    return 0;
}

static int opts_delim_quote(JSContext *ctx, JSValueConst obj, const char *pfx,
                            uint8_t *pdelim, uint8_t *pquote) {
    *pdelim = ','; *pquote = '"';
    char *s;
    s = opt_str(ctx, obj, "delimiter", NULL);
    if (!s && JS_HasException(ctx)) return -1;
    if (s && opts_one_char(ctx, pfx, "delimiter", s, pdelim, '"', "quote"))
        return -1;
    s = opt_str(ctx, obj, "quote", NULL);
    if (!s && JS_HasException(ctx)) return -1;
    if (s && opts_one_char(ctx, pfx, "quote", s, pquote, ',', "delimiter"))
        return -1;
    if (*pdelim == *pquote) { JS_ThrowTypeError(ctx, "%s: 'delimiter' and 'quote' must differ", pfx); return -1; }
    return 0;
}

/* The valid-key tables, one per options bag. opts_check rejects anything not
 * listed here, so this list is also the method's option documentation. */
static const char *const opts_create_keys[]   = { "headers", "rows", "overwrite" };
static const char *const opts_read_keys[]     = { "offset", "limit", "columns", "strict", "delimiter", "quote" };
static const char *const opts_add_row_keys[]  = { "rows", "strict", "durable" };
static const char *const opts_update_keys[]   = { "row", "column", "columnIndex", "value", "strict", "durable" };
static const char *const opts_remove_row_keys[]  = { "row", "strict", "durable" };
static const char *const opts_add_col_keys[]  = { "column", "defaultValue", "strict", "durable" };
static const char *const opts_remove_col_keys[]  = { "column", "columnIndex", "strict", "durable" };
static const char *const opts_rename_keys[]   = { "oldName", "newName", "strict", "durable" };
/*the range methods share one windowing surface ({start,end} xor
 * {offset,limit}, plus {maxRows}). */
static const char *const opts_range_keys[]    = { "column", "start", "end", "offset", "limit", "maxRows", "strict" };
static const char *const opts_row_range_keys[]    = { "start", "end", "offset", "limit", "maxRows", "strict" };
static const char *const opts_select_keys[]   = { "columns", "start", "end", "offset", "limit", "maxRows", "strict" };
static const char *const opts_parse_keys[]    = { "delimiter", "quote", "hasHeader", "strict" };
static const char *const opts_stringify_keys[]    = { "delimiter", "quote", "hasHeader" };

/* find a header column index by name, or -1 */
static int header_index(const Table *t, const char *name) {
    if (t->n == 0) return -1;
    for (size_t c = 0; c < t->r[0].n; c++)
        if (strcmp(t->r[0].f[c] ? t->r[0].f[c] : "", name) == 0) return (int)c;
    return -1;
}

/* Own-property fetch: like JS_GetPropertyStr but does NOT walk the prototype
 * chain. A named addRow must map by the row object's OWN properties only --
 * inherited values are not the row's data, and a header named "__proto__"
 * must not read Object.prototype's accessor (which stringifies to
 * "[object Object]"). Returns a JSValue the caller owns (JS_UNDEFINED when
 * the own property is absent); JS_EXCEPTION only on an internal error. */
static JSValue csv_own_get(JSContext *ctx, JSValueConst obj, const char *key) {
    JSAtom a = JS_NewAtom(ctx, key);
    JSPropertyDescriptor desc;
    int r;
    if (a == JS_ATOM_NULL) return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &desc, obj, a);
    JS_FreeAtom(ctx, a);
    if (r < 0) return JS_EXCEPTION;
    if (r == 0) return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return desc.value;
}

/* Load + parse a file into `t`. `strict` turns the two known tolerant-reader
 * divergences (garbage after a closing quote, an unterminated quote at EOF)
 * into named syntax errors; the default of 0 keeps the tolerant parse.
 * `delim`/`quote` are the field/quote bytes (validated upstream). Only
 * read() takes them: mutators rewrite with the canonical pair, so a file
 * written in another dialect must not be corrupted by a blind edit.
 * Returns 0, or -1 with a thrown exception. */
static int csv_load(JSContext *ctx, const char *path, Table *t, int strict,
                    uint8_t delim, uint8_t quote) {
    dyn_iobuf_t src;
    if (dyn_io_slurp(path, &src, 0, DYN_MAX_INPUT) < 0) {
        if (errno == EFBIG) {
            JS_ThrowRangeError(ctx, "csv: file exceeds %u bytes (DYN_MAX_INPUT)",
                               (unsigned)DYN_MAX_INPUT);
            return -1;
        }
        JS_ThrowTypeError(ctx, "csv: cannot read '%s': %s", path, strerror(errno));
        return -1;
    }
    /* parse straight from the buffer (an mmap view for large files: zero copy) */
    const uint8_t *data = (const uint8_t *)dyn_iobuf_rdata(&src);
    size_t dlen = dyn_iobuf_rlen(&src);
    /* A leading UTF-8 BOM is an encoding signature, not a header cell. */
    if (dlen >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3; dlen -= 3;
    }
    int errrow = 0;
    int r = csv_parse_ex(data, dlen, t, strict, &errrow, delim, quote);
    dyn_iobuf_free(&src);
    if (r == CSV_SYNTAX_QUOTE) {
        table_free(t);
        JS_ThrowSyntaxError(ctx, "csv: row %d: unterminated quoted field", errrow);
        return -1;
    }
    if (r == CSV_SYNTAX_GARBAGE) {
        table_free(t);
        JS_ThrowSyntaxError(ctx, "csv: row %d: unexpected text after a closing quote", errrow);
        return -1;
    }
    if (r < 0) { JS_ThrowOutOfMemory(ctx); return -1; }
    if (t->n == 0) { table_free(t); JS_ThrowTypeError(ctx, "csv: '%s' is empty (no header)", path); return -1; }
    return 0;
}
/* Serialize + atomically write `t`. Returns 0, or -1 with a thrown exception. */
static int csv_store(JSContext *ctx, const char *path, const Table *t, int durable) {
    Buf out; memset(&out, 0, sizeof(out));
    if (csv_serialize(t, &out)) { buf_free(&out); JS_ThrowOutOfMemory(ctx); return -1; }
    int r = csv_write_atomic(path, out.p ? out.p : "", out.len, durable);
    buf_free(&out);
    if (r < 0) { JS_ThrowTypeError(ctx, "csv: cannot write '%s': %s", path, strerror(errno)); return -1; }
    return 0;
}

/* build a JS array from a table row's cells [c0,c1) */
static JSValue row_to_js(JSContext *ctx, const Table *t, size_t r, size_t ncols) {
    JSValue a = JS_NewArray(ctx);
    for (size_t c = 0; c < ncols; c++)
        JS_SetPropertyUint32(ctx, a, (uint32_t)c, JS_NewString(ctx, cell(t, r, c)));
    return a;
}

/* ============================ CSVFile class ============================ */
/* The instance owns nothing but its file path; every method load-modify-stores
 * that file. `path` is immutable after construction. */
typedef struct { char *path; } DynCsvFile;

static JSClassID dyn_csvfile_class_id;

static void dyn_csvfile_dispose(void *native) {
    DynCsvFile *f = (DynCsvFile *)native;
    if (!f) return;
    free(f->path);
    free(f);
}

static const JSClassDef dyn_csvfile_class = {
    "CSVFile",
    .finalizer = dyn_res_finalizer,
};

/* new CSVFile(path) -- binds a path; does not touch the disk. */
static JSValue js_csvfile_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "new CSVFile(path) requires a Path");
    /* Borrowed, not coerced: a Path has no close(), so nothing can invalidate
     * these bytes mid-call. The strdup below is still required -- the instance
     * outlives the constructor argument. */
    const char *s = dyn_path_borrow(ctx, argv[0], "CSVFile(path)", NULL);
    if (!s) return JS_EXCEPTION;
    DynCsvFile *f = (DynCsvFile *)calloc(1, sizeof(*f));
    if (!f) { return JS_ThrowOutOfMemory(ctx); }
    f->path = strdup(s);
    if (!f->path) { free(f); return JS_ThrowOutOfMemory(ctx); }
    return dyn_res_wrap(ctx, new_target, dyn_csvfile_class_id, f, dyn_csvfile_dispose);
}

/* Resolve `this` to a PRIVATE owned copy of its path (caller frees), or NULL
 * (throwing). Copying up front makes the method reentrancy-safe: a later
 * argument coercion may close `this` and free the instance, but not our copy. */
static char *csvfile_path(JSContext *ctx, JSValueConst this_val) {
    DynResource *r = dyn_res_get(ctx, this_val, dyn_csvfile_class_id);
    if (!r) return NULL;
    char *p = strdup(((DynCsvFile *)r->native)->path);
    if (!p) { JS_ThrowOutOfMemory(ctx); return NULL; }
    return p;
}

/* the options object (may be absent -- QuickJS pads argv to the method's
 * declared arity, so argv[0] is `undefined` when omitted, and the opt_* helpers
 * treat a non-object as "all keys absent"); data methods validate it via
 * need_obj. */
#define OPTS argv[0]
/* require an options object (for methods whose data lives in it) */
static int need_obj(JSContext *ctx, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) { JS_ThrowTypeError(ctx, "csv: expected an options object"); return -1; }
    return 0;
}

/* ============================ create ============================ */
static JSValue js_csv_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.create", opts_create_keys, countof(opts_create_keys))) { free(path); return JS_EXCEPTION; }
    char **headers = NULL; int nh = opt_str_array(ctx, OPTS, "headers", &headers);
    JSValue ret = JS_EXCEPTION;
    if (nh < 0) { free(path); return JS_EXCEPTION; }
    if (nh == 0) { JS_ThrowTypeError(ctx, "csv.create: 'headers' must have at least one entry"); goto done; }

    int overwrite = opt_bool(ctx, OPTS, "overwrite");
    if (!overwrite) { struct stat st; if (stat(path, &st) == 0) { JS_ThrowTypeError(ctx, "csv.create: '%s' already exists (set overwrite:true)", path); goto done; } }

    Buf out; memset(&out, 0, sizeof(out));
    for (int c = 0; c < nh; c++) {
        if ((c && buf_putc(&out, ',')) || emit_field(&out, headers[c], ',', '"')) {
            buf_free(&out); JS_ThrowOutOfMemory(ctx); goto done;
        }
    }
    if (buf_putc(&out, '\n')) { buf_free(&out); JS_ThrowOutOfMemory(ctx); goto done; }

    /* optional initial rows */
    JSValue rows = JS_GetPropertyStr(ctx, OPTS, "rows");
    uint32_t nrows = 0;
    /* a PRESENT but non-array rows is a caller bug: refuse like addRow does,
       rather than silently writing a headers-only file */
    if (!JS_IsUndefined(rows) && !JS_IsNull(rows) && !JS_IsArray(ctx, rows)) {
        JS_FreeValue(ctx, rows); buf_free(&out);
        JS_ThrowTypeError(ctx, "csv.create: 'rows' must be an array");
        goto done;
    }
    if (JS_IsArray(ctx, rows)) {
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length"); JS_ToUint32(ctx, &nrows, lv); JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < nrows; i++) {
            JSValue rv = JS_GetPropertyUint32(ctx, rows, i);
            uint32_t rc = 0; JSValue rl = JS_GetPropertyStr(ctx, rv, "length"); JS_ToUint32(ctx, &rc, rl); JS_FreeValue(ctx, rl);
            if (!JS_IsArray(ctx, rv) || rc != (uint32_t)nh) {
                JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows); buf_free(&out);
                JS_ThrowTypeError(ctx, "csv.create: row %u must have exactly %d values", i, nh); goto done;
            }
            for (uint32_t c = 0; c < rc; c++) {
                JSValue cv = JS_GetPropertyUint32(ctx, rv, c);
                const char *s = JS_ToCString(ctx, cv); JS_FreeValue(ctx, cv);
                if (!s) {
                    /* a cell's conversion threw: surface THAT exception and
                       write nothing, rather than silently recording "" */
                    JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows); buf_free(&out); goto done;
                }
                if ((c && buf_putc(&out, ',')) || emit_field(&out, s, ',', '"')) {
                    JS_FreeCString(ctx, s); JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
                    buf_free(&out); JS_ThrowOutOfMemory(ctx); goto done;
                }
                JS_FreeCString(ctx, s);
            }
            if (buf_putc(&out, '\n')) {
                JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
                buf_free(&out); JS_ThrowOutOfMemory(ctx); goto done;
            }
            JS_FreeValue(ctx, rv);
        }
    }
    JS_FreeValue(ctx, rows);

    csv_mkparents(path);
    if (csv_write_atomic(path, out.p ? out.p : "", out.len, /*durable=*/1) < 0) {
        buf_free(&out); JS_ThrowTypeError(ctx, "csv.create: cannot write '%s': %s", path, strerror(errno)); goto done;
    }
    buf_free(&out);
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, ret, "rows", JS_NewInt64(ctx, nrows));
done:
    free_str_array(headers, nh); free(path);
    return ret;
}

/* ============================ read ============================ */
static JSValue js_csv_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc;
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (opts_check(ctx, OPTS, "csv.read", opts_read_keys, countof(opts_read_keys))) { free(path); return JS_EXCEPTION; }
    uint8_t delim, quote;
    if (opts_delim_quote(ctx, OPTS, "csv.read", &delim, &quote)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), delim, quote)) { free(path); return JS_EXCEPTION; }

    /* cols/ret are declared (and initialized) BEFORE any `goto done` so the
       done: cleanup never reads an indeterminate value */
    char **cols = NULL; int ncsel = 0;
    JSValue ret = JS_EXCEPTION;
    int64_t offset = 0, limit = -1; int has_limit = 0;
    if (opt_int(ctx, OPTS, "offset", 0, &offset, NULL)) goto done;
    if (opt_int(ctx, OPTS, "limit", -1, &limit, &has_limit)) goto done;
    if (offset < 0) offset = 0;
    ncsel = opt_str_array(ctx, OPTS, "columns", &cols);
    if (ncsel < 0) goto done;

    size_t ncols = table_ncols(&t);
    size_t total = t.n - 1;                        /* data rows */

    /* resolve the projected column indices (all columns if none requested) */
    size_t nproj = ncsel > 0 ? (size_t)ncsel : ncols;
    int *idx = (int *)malloc((nproj ? nproj : 1) * sizeof(int));
    if (!idx) { JS_ThrowOutOfMemory(ctx); goto done; }
    JSValue hjs = JS_NewArray(ctx);
    for (size_t k = 0; k < nproj; k++) {
        if (ncsel > 0) {
            int ci = header_index(&t, cols[k]);
            if (ci < 0) { free(idx); JS_FreeValue(ctx, hjs); JS_ThrowTypeError(ctx, "csv.read: no such column '%s'", cols[k]); goto done; }
            idx[k] = ci;
        } else idx[k] = (int)k;
        JS_SetPropertyUint32(ctx, hjs, (uint32_t)k, JS_NewString(ctx, cell(&t, 0, (size_t)idx[k])));
    }

    /* clamp start FIRST, then compute the window from the clamped value:
       computing the window before the clamp let a large offset produce a
       [start,end) pair that never intersected the real rows consistently */
    size_t start = (size_t)offset;
    if (start > total) start = total;
    size_t end = total;
    if (has_limit && limit >= 0 && start + (size_t)limit < end) end = start + (size_t)limit;

    JSValue rjs = JS_NewArray(ctx);
    uint32_t out_i = 0;
    for (size_t r = start; r < end; r++) {
        JSValue a = JS_NewArray(ctx);
        for (size_t k = 0; k < nproj; k++)
            JS_SetPropertyUint32(ctx, a, (uint32_t)k, JS_NewString(ctx, cell(&t, r + 1, (size_t)idx[k])));
        JS_SetPropertyUint32(ctx, rjs, out_i++, a);
    }
    free(idx);

    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "headers", hjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)total));
done:
    free_str_array(cols, ncsel > 0 ? ncsel : 0);
    table_free(&t); free(path);
    return ret;
}

/* ============================ addRow ============================ */
/*`addRow(row)` accepts a bare positional array row in addition to the
 * {rows: [...]} bag (which stays, named objects included). A bare array is
 * EXACTLY ONE row: nesting arrays inside it ("just pass all my rows") is
 * refused outright, because the cell-wise ToString would silently join each
 * sub-row into one garbled line -- multi-row adds go through the bag. The
 * bare form carries no options (module defaults). */
static JSValue js_csv_add_row(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    int bare = argc >= 1 && JS_IsArray(ctx, argv[0]);
    JSValue rows;
    if (bare) {
        rows = JS_NewArray(ctx);
        if (JS_IsException(rows)) { free(path); return JS_EXCEPTION; }
        JS_SetPropertyUint32(ctx, rows, 0, JS_DupValue(ctx, argv[0]));
        uint32_t rc = 0;
        JSValue rl = JS_GetPropertyStr(ctx, argv[0], "length");
        /* the length read can run a user getter: a throw must propagate,
         * never leave rc stale under a pending exception */
        if (JS_IsException(rl) || JS_ToUint32(ctx, &rc, rl)) {
            JS_FreeValue(ctx, rl);
            JS_FreeValue(ctx, rows); free(path);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, rl);
        for (uint32_t c = 0; c < rc; c++) {
            JSValue cv = JS_GetPropertyUint32(ctx, argv[0], c);
            int nested = JS_IsArray(ctx, cv);
            JS_FreeValue(ctx, cv);
            if (nested) {
                JS_FreeValue(ctx, rows); free(path);
                JS_ThrowTypeError(ctx, "csv.addRow: a bare array is one positional row -- pass {rows: [...]} to add several");
                return JS_EXCEPTION;
            }
        }
    } else {
        if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
        if (opts_check(ctx, OPTS, "csv.addRow", opts_add_row_keys, countof(opts_add_row_keys))) { free(path); return JS_EXCEPTION; }
        rows = JS_GetPropertyStr(ctx, OPTS, "rows");
    }
    Table t;
    if (csv_load(ctx, path, &t, bare ? 0 : opt_bool(ctx, OPTS, "strict"), ',', '"')) {
        JS_FreeValue(ctx, rows); free(path); return JS_EXCEPTION;
    }
    size_t ncols = table_ncols(&t);

    JSValue ret = JS_EXCEPTION;
    if (!JS_IsArray(ctx, rows)) { JS_FreeValue(ctx, rows); JS_ThrowTypeError(ctx, "csv.addRow: 'rows' must be an array"); goto done; }
    uint32_t nr = 0; { JSValue lv = JS_GetPropertyStr(ctx, rows, "length"); JS_ToUint32(ctx, &nr, lv); JS_FreeValue(ctx, lv); }

    for (uint32_t i = 0; i < nr; i++) {
        JSValue rv = JS_GetPropertyUint32(ctx, rows, i);
        Row row; memset(&row, 0, sizeof(row));
        if (JS_IsArray(ctx, rv)) {                 /* positional */
            uint32_t rc = 0; JSValue rl = JS_GetPropertyStr(ctx, rv, "length"); JS_ToUint32(ctx, &rc, rl); JS_FreeValue(ctx, rl);
            for (size_t c = 0; c < ncols; c++) {
                char *s = NULL;
                if (c < rc) {
                    JSValue cv = JS_GetPropertyUint32(ctx, rv, (uint32_t)c);
                    const char *cs = JS_ToCString(ctx, cv); JS_FreeValue(ctx, cv);
                    if (!cs) {
                        /* a cell's conversion threw: surface THAT exception,
                           write no partial row (never a silent "") */
                        free(row.f); JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows); goto done;
                    }
                    s = tcell_dup(&t, cs, strlen(cs));
                    if (*cs && !s) {
                        JS_FreeCString(ctx, cs); free(row.f); JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
                        JS_ThrowOutOfMemory(ctx); goto done;
                    }
                    JS_FreeCString(ctx, cs);
                }
                if (row_push(&row, s)) {   /* cells are arena-owned; never free one */
                    free(row.f); JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
                    JS_ThrowOutOfMemory(ctx); goto done;
                }
            }
        } else if (JS_IsObject(rv)) {              /* named: map by header */
            for (size_t c = 0; c < ncols; c++) {
                JSValue cv = csv_own_get(ctx, rv, cell(&t, 0, c));
                char *s = NULL;
                if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
                    const char *cs = JS_ToCString(ctx, cv);
                    if (!cs) {
                        JS_FreeValue(ctx, cv); free(row.f); JS_FreeValue(ctx, rv);
                        JS_FreeValue(ctx, rows); goto done;
                    }
                    s = tcell_dup(&t, cs, strlen(cs));
                    if (*cs && !s) {
                        JS_FreeCString(ctx, cs); JS_FreeValue(ctx, cv); free(row.f);
                        JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
                        JS_ThrowOutOfMemory(ctx); goto done;
                    }
                    JS_FreeCString(ctx, cs);
                }
                JS_FreeValue(ctx, cv);
                if (row_push(&row, s)) {
                    free(row.f); JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
                    JS_ThrowOutOfMemory(ctx); goto done;
                }
            }
        } else {
            JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
            free(row.f);
            JS_ThrowTypeError(ctx, "csv.addRow: each row must be an array or an object"); goto done;
        }
        JS_FreeValue(ctx, rv);
        if (table_push(&t, row)) { free(row.f); JS_FreeValue(ctx, rows); JS_ThrowOutOfMemory(ctx); goto done; }
    }
    JS_FreeValue(ctx, rows);
    if (csv_store(ctx, path, &t, bare ? 0 : opt_durable(ctx, OPTS))) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "added", JS_NewInt64(ctx, nr));
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)(t.n - 1)));
done:
    table_free(&t); free(path);
    return ret;
}

/* resolve a column selector (column name or columnIndex) to an index, or -1 (throwing).
 * `pfx` is the calling method's error prefix: method-originated errors carry
 * the method name ("csv.updateCell: no such column 'x'"), the file's majority
 * convention -- not the bare "csv:" form. */
static int resolve_column(JSContext *ctx, JSValueConst obj, const Table *t,
                          const char *pfx) {
    int present; char *name = opt_str(ctx, obj, "column", &present);
    if (name) { int ci = header_index(t, name); if (ci < 0) JS_ThrowTypeError(ctx, "%s: no such column '%s'", pfx, name); free(name); return ci; }
    if (JS_HasException(ctx)) return -1;
    int64_t ix; int has;
    if (opt_int(ctx, obj, "columnIndex", 0, &ix, &has)) return -1;
    if (has) { if (ix < 0 || (size_t)ix >= table_ncols(t)) { JS_ThrowRangeError(ctx, "%s: columnIndex %lld out of range", pfx, (long long)ix); return -1; } return (int)ix; }
    JS_ThrowTypeError(ctx, "%s: provide 'column' or 'columnIndex'", pfx);
    return -1;
}

/* ============================ updateCell ============================ */
static JSValue js_csv_update_cell(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.updateCell", opts_update_keys, countof(opts_update_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;

    int64_t rowi; int hasr;
    if (opt_int(ctx, OPTS, "row", 0, &rowi, &hasr) || !hasr) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.updateCell: 'row' is required"); goto done; }
    if (rowi < 0 || (size_t)rowi >= t.n - 1) {
        if (t.n < 2)   /* headers-only: t.n - 2 would underflow the (0..%zu) hint */
            JS_ThrowRangeError(ctx, "csv.updateCell: row %lld out of range (the file has no data rows)", (long long)rowi);
        else
            JS_ThrowRangeError(ctx, "csv.updateCell: row %lld out of range (0..%zu)", (long long)rowi, t.n - 2);
        goto done;
    }
    int ci = resolve_column(ctx, OPTS, &t, "csv.updateCell");
    if (ci < 0) goto done;
    int present; char *value = opt_str(ctx, OPTS, "value", &present);
    if (!present) { free(value); if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.updateCell: 'value' is required"); goto done; }

    Row *row = &t.r[(size_t)rowi + 1];
    while (row->n <= (size_t)ci) { if (row_push(row, NULL)) { free(value); JS_ThrowOutOfMemory(ctx); goto done; } }  /* pad short rows */
    /* the old cell is arena-owned: dropping the pointer is the whole removal */
    row->f[ci] = (value && *value) ? tcell_dup(&t, value, strlen(value)) : NULL;
    if (value && *value && !row->f[ci]) { free(value); JS_ThrowOutOfMemory(ctx); goto done; }
    free(value);

    if (csv_store(ctx, path, &t, opt_durable(ctx, OPTS))) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "row", JS_NewInt64(ctx, rowi));
    JS_SetPropertyStr(ctx, ret, "column", JS_NewString(ctx, cell(&t, 0, (size_t)ci)));
    JS_SetPropertyStr(ctx, ret, "value", JS_NewString(ctx, row->f[ci] ? row->f[ci] : ""));
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ removeRow ============================ */
static JSValue js_csv_remove_row(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.removeRow", opts_remove_row_keys, countof(opts_remove_row_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    int64_t rowi; int hasr;
    if (opt_int(ctx, OPTS, "row", 0, &rowi, &hasr) || !hasr) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.removeRow: 'row' is required"); goto done; }
    if (rowi < 0 || (size_t)rowi >= t.n - 1) { JS_ThrowRangeError(ctx, "csv.removeRow: row %lld out of range", (long long)rowi); goto done; }
    size_t r = (size_t)rowi + 1;
    free(t.r[r].f);                     /* the row array; cells are arena-owned */
    memmove(&t.r[r], &t.r[r + 1], (t.n - r - 1) * sizeof(Row));
    t.n--;
    if (csv_store(ctx, path, &t, opt_durable(ctx, OPTS))) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "removed", JS_NewInt64(ctx, rowi));
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)(t.n - 1)));
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ addColumn ============================ */
static JSValue js_csv_add_column(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.addColumn", opts_add_col_keys, countof(opts_add_col_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    int present; char *col = opt_str(ctx, OPTS, "column", &present);
    if (!col) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.addColumn: 'column' is required"); goto done; }
    if (header_index(&t, col) >= 0) { JS_ThrowTypeError(ctx, "csv.addColumn: column '%s' already exists", col); goto done; }
    char *def = opt_str(ctx, OPTS, "defaultValue", NULL);   /* NULL => "" */

    size_t ncols = table_ncols(&t);
    for (size_t r = 0; r < t.n; r++) {
        Row *row = &t.r[r];
        while (row->n < ncols) { if (row_push(row, NULL)) { free(def); JS_ThrowOutOfMemory(ctx); goto done; } }
        /* NULL is tcell_dup's SUCCESS value for an empty cell, so an empty
           name must not be routed through it or it reads as an OOM. */
        char *v = (r == 0) ? (*col ? tcell_dup(&t, col, strlen(col)) : NULL)
                           : (def && *def ? tcell_dup(&t, def, strlen(def)) : NULL);
        if (r == 0 && *col && !v) { free(def); JS_ThrowOutOfMemory(ctx); goto done; }
        if (row_push(row, v)) { free(def); JS_ThrowOutOfMemory(ctx); goto done; }
    }
    free(def);
    if (csv_store(ctx, path, &t, opt_durable(ctx, OPTS))) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "column", JS_NewString(ctx, col));
    JS_SetPropertyStr(ctx, ret, "totalColumns", JS_NewInt64(ctx, (int64_t)table_ncols(&t)));
done:
    free(col); table_free(&t); free(path);
    return ret;
}

/* ============================ removeColumn ============================ */
static JSValue js_csv_remove_column(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.removeColumn", opts_remove_col_keys, countof(opts_remove_col_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    int ci = resolve_column(ctx, OPTS, &t, "csv.removeColumn");
    if (ci < 0) goto done;
    for (size_t r = 0; r < t.n; r++) {
        Row *row = &t.r[r];
        if ((size_t)ci >= row->n) continue;
        /* the dropped cell stays in the arena until table_free */
        memmove(&row->f[ci], &row->f[ci + 1], (row->n - ci - 1) * sizeof(char *));
        row->n--;
    }
    if (csv_store(ctx, path, &t, opt_durable(ctx, OPTS))) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "removedIndex", JS_NewInt64(ctx, ci));
    JS_SetPropertyStr(ctx, ret, "totalColumns", JS_NewInt64(ctx, (int64_t)table_ncols(&t)));
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ renameColumn ============================ */
static JSValue js_csv_rename_column(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.renameColumn", opts_rename_keys, countof(opts_rename_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    char *oldn = opt_str(ctx, OPTS, "oldName", NULL);
    char *newn = opt_str(ctx, OPTS, "newName", NULL);
    if (!oldn || !newn) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.renameColumn: 'oldName' and 'newName' are required"); goto done; }
    int oi = header_index(&t, oldn);
    if (oi < 0) { JS_ThrowTypeError(ctx, "csv.renameColumn: no such column '%s'", oldn); goto done; }
    if (strcmp(oldn, newn) != 0) {
        if (header_index(&t, newn) >= 0) { JS_ThrowTypeError(ctx, "csv.renameColumn: column '%s' already exists", newn); goto done; }
        /* NULL means "empty cell", not "allocation failed": an empty newName
           must not be reported as OOM. */
        char *nv = *newn ? tcell_dup(&t, newn, strlen(newn)) : NULL;
        if (*newn && !nv) { JS_ThrowOutOfMemory(ctx); goto done; }
        t.r[0].f[oi] = nv;              /* the old header cell is arena-owned */
        if (csv_store(ctx, path, &t, opt_durable(ctx, OPTS))) goto done;
    }
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "oldName", JS_NewString(ctx, oldn));
    JS_SetPropertyStr(ctx, ret, "newName", JS_NewString(ctx, newn));
done:
    free(oldn); free(newn); table_free(&t); free(path);
    return ret;
}

/*ONE windowing convention across the range methods. {start, end}
 * (exclusive) and its {offset, limit} aliases name the same window; mixing the
 * two forms in one call is REFUSED (the strict-options pilot: an ambiguity is
 * a caller bug, not a default to resolve). `limit` counts rows from `start`,
 * with read()'s semantics that a negative limit means "to the end". {maxRows}
 * overrides the method's default cap (`defcap`: 1000 for readColumnValuesRange,
 * 100 for readRowRange/selectColumnRange); when it is absent the default cap
 * still applies, and it constrains the REQUESTED window when the end is
 * explicit. As before, an omitted end runs to the last row uncapped --
 * except `single_default` (readRowRange), whose no-arg form means one row.
 * `pfx` is the calling method's name: window refusals carry the method prefix
 * like every other method-originated error. Returns 0 or -1 (throwing). */
static int range_window(JSContext *ctx, JSValueConst obj, size_t total,
                        size_t defcap, int single_default, const char *pfx,
                        size_t *ps, size_t *pe) {
    int64_t start = 0, end = 0, offset = 0, limit = -1, maxrows = -1;
    int has_start, has_end, has_offset, has_limit, has_max;
    /* a throwing coercion (e.g. a valueOf that throws) must propagate BEFORE
       the locals are read: opt_int leaves *out untouched on failure, so an
       unchecked return would compute the window from the default... after
       raising a pending exception mid-flight */
    if (opt_int(ctx, obj, "start", 0, &start, &has_start)) return -1;
    if (opt_int(ctx, obj, "end", 0, &end, &has_end)) return -1;
    if (opt_int(ctx, obj, "offset", 0, &offset, &has_offset)) return -1;
    if (opt_int(ctx, obj, "limit", -1, &limit, &has_limit)) return -1;
    if ((has_start || has_end) && (has_offset || has_limit)) {
        JS_ThrowTypeError(ctx, "%s: use either {start, end} or {offset, limit} -- not both", pfx);
        return -1;
    }
    if (has_offset) { start = offset; has_start = 1; }
    if (has_limit) {
        if (limit < 0) has_end = 0;          /* negative limit = to the end */
        else {
            has_end = 1;
            /* guard the addition: a huge limit must clamp, not overflow */
            end = (uint64_t)limit <= (uint64_t)INT64_MAX - (uint64_t)start
                  ? start + limit : (int64_t)INT64_MAX;
        }
    }
    if (opt_int(ctx, obj, "maxRows", -1, &maxrows, &has_max)) return -1;
    if (has_max && maxrows < 0) {
        JS_ThrowRangeError(ctx, "%s: maxRows must not be negative", pfx);
        return -1;
    }
    size_t cap = has_max ? (size_t)maxrows : defcap;
    if (start < 0) start = 0;
    /* the cap is on the REQUESTED window when the end is explicit; an omitted
     * end means "to the end" and is not capped. */
    if (has_end && end > start && (uint64_t)(end - start) > (uint64_t)cap) {
        JS_ThrowRangeError(ctx, "%s: requested window %lld exceeds the maximum of %zu rows (raise it with maxRows)", pfx, (long long)(end - start), cap);
        return -1;
    }
    size_t s = (size_t)start > total ? total : (size_t)start;
    size_t e;
    if (has_end) { e = (end < 0) ? 0 : (size_t)end; }
    else if (single_default) e = s + 1;
    else e = total;
    if (e > total) e = total;
    if (e < s) e = s;
    *ps = s; *pe = e;
    return 0;
}

/* ============================ readColumnValuesRange ============================ */
static JSValue js_csv_read_column_values_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.readColumnValuesRange", opts_range_keys, countof(opts_range_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    char *col = opt_str(ctx, OPTS, "column", NULL);
    if (!col) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.readColumnValuesRange: 'column' is required"); goto done; }
    int ci = header_index(&t, col);
    if (ci < 0) { JS_ThrowTypeError(ctx, "csv.readColumnValuesRange: no such column '%s'", col); goto done; }
    size_t s, e;
    if (range_window(ctx, OPTS, t.n - 1, 1000, 0, "csv.readColumnValuesRange", &s, &e)) goto done;
    JSValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    for (size_t r = s; r < e; r++) JS_SetPropertyUint32(ctx, a, k++, JS_NewString(ctx, cell(&t, r + 1, (size_t)ci)));
    ret = a;
done:
    free(col); table_free(&t); free(path);
    return ret;
}

/* ============================ readRowRange ============================ */
static JSValue js_csv_read_row_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    /* options are OPTIONAL here (readRowRange() means row 0), so need_obj is
       too strict -- but a POSITIONAL call was silently ignored and returned
       row 0. Refuse a given non-object; keep the documented no-arg form. */
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsObject(argv[0])) {
        JS_ThrowTypeError(ctx, "csv.readRowRange: expected an options object "
                               "like { start, end } -- positional arguments are "
                               "not read");
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.readRowRange", opts_row_range_keys, countof(opts_row_range_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    size_t ncols = table_ncols(&t);
    /* default: a single row (start..start+1) */
    size_t s, e;
    if (range_window(ctx, OPTS, t.n - 1, 100, 1, "csv.readRowRange", &s, &e)) goto done;
    JSValue hjs = JS_NewArray(ctx);
    for (size_t c = 0; c < ncols; c++) JS_SetPropertyUint32(ctx, hjs, (uint32_t)c, JS_NewString(ctx, cell(&t, 0, c)));
    JSValue rjs = JS_NewArray(ctx);
    uint32_t k = 0;
    for (size_t r = s; r < e; r++) JS_SetPropertyUint32(ctx, rjs, k++, row_to_js(ctx, &t, r + 1, ncols));
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "headers", hjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ selectColumnRange ============================ */
static JSValue js_csv_select_column_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    if (opts_check(ctx, OPTS, "csv.selectColumnRange", opts_select_keys, countof(opts_select_keys))) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"')) { free(path); return JS_EXCEPTION; }
    char **cols = NULL; int ncsel = opt_str_array(ctx, OPTS, "columns", &cols);
    JSValue ret = JS_EXCEPTION;
    if (ncsel < 0) { table_free(&t); free(path); return JS_EXCEPTION; }
    if (ncsel == 0) { JS_ThrowTypeError(ctx, "csv.selectColumnRange: 'columns' must be non-empty"); goto done; }
    int *idx = (int *)malloc(ncsel * sizeof(int));
    if (!idx) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (int k = 0; k < ncsel; k++) { int ci = header_index(&t, cols[k]); if (ci < 0) { free(idx); JS_ThrowTypeError(ctx, "csv.selectColumnRange: no such column '%s'", cols[k]); goto done; } idx[k] = ci; }
    size_t s, e;
    if (range_window(ctx, OPTS, t.n - 1, 100, 0, "csv.selectColumnRange", &s, &e)) { free(idx); goto done; }
    JSValue cjs = JS_NewArray(ctx);
    for (int k = 0; k < ncsel; k++) JS_SetPropertyUint32(ctx, cjs, (uint32_t)k, JS_NewString(ctx, cols[k]));
    JSValue rjs = JS_NewArray(ctx);
    uint32_t out_i = 0;
    for (size_t r = s; r < e; r++) {
        JSValue a = JS_NewArray(ctx);
        for (int k = 0; k < ncsel; k++) JS_SetPropertyUint32(ctx, a, (uint32_t)k, JS_NewString(ctx, cell(&t, r + 1, (size_t)idx[k])));
        JS_SetPropertyUint32(ctx, rjs, out_i++, a);
    }
    free(idx);
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "columns", cjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
done:
    free_str_array(cols, ncsel > 0 ? ncsel : 0);
    table_free(&t); free(path);
    return ret;
}

/* ============================ parse ============================ */
/* dyna:csv.parse(text, opts?) -> { headers, rows, totalRows }
 * The in-memory twin of CSVFile.read(): the same result shape with no temp
 * file + IO. A leading UTF-8 BOM is an encoding signature, not a header cell
 * (stripped, exactly as csv_load does). One deliberate divergence, documented
 * in API.md: read() throws on an empty FILE (a file-backed table needs a
 * header schema), while parse("") is the empty table {headers: [], rows: [],
 * totalRows: 0}. {hasHeader:false} parses headerless text: headers is [] and
 * every parsed row is a data row. Result rows are shaped to the FIRST parsed
 * row's width (the header row when hasHeader): short rows pad with "" and long
 * rows truncate -- the same ragged-row convention read() has always had. */
static JSValue js_csv_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx, "csv.parse: expected CSV text");
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (opts_check(ctx, opts, "csv.parse", opts_parse_keys, countof(opts_parse_keys))) return JS_EXCEPTION;
    uint8_t delim, quote;
    if (opts_delim_quote(ctx, opts, "csv.parse", &delim, &quote)) return JS_EXCEPTION;
    int strict = opt_bool(ctx, opts, "strict");
    int has_header = opt_bool_def(ctx, opts, "hasHeader", 1);

    size_t tlen = 0;
    const char *text = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text) return JS_EXCEPTION;
    const uint8_t *data = (const uint8_t *)text;
    size_t dlen = tlen;
    if (dlen >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3; dlen -= 3;
    }
    Table t;
    int errrow = 0;
    int r = csv_parse_ex(data, dlen, &t, strict, &errrow, delim, quote);
    JS_FreeCString(ctx, text);
    /* rows are 1-based over the TEXT's lines (the header counts as row 1) */
    if (r == CSV_SYNTAX_QUOTE) {
        table_free(&t);
        return JS_ThrowSyntaxError(ctx, "csv.parse: row %d: unterminated quoted field", errrow);
    }
    if (r == CSV_SYNTAX_GARBAGE) {
        table_free(&t);
        return JS_ThrowSyntaxError(ctx, "csv.parse: row %d: unexpected text after a closing quote", errrow);
    }
    if (r < 0) return JS_ThrowOutOfMemory(ctx);

    size_t width = t.n ? t.r[0].n : 0;
    size_t first_data = (has_header && t.n) ? 1 : 0;
    JSValue hjs = JS_NewArray(ctx);
    JSValue rjs = JS_NewArray(ctx);
    if (JS_IsException(hjs) || JS_IsException(rjs)) {
        JS_FreeValue(ctx, hjs); JS_FreeValue(ctx, rjs); table_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (has_header)
        for (size_t c = 0; c < width; c++)
            JS_SetPropertyUint32(ctx, hjs, (uint32_t)c, JS_NewString(ctx, cell(&t, 0, c)));
    uint32_t k = 0;
    for (size_t r2 = first_data; r2 < t.n; r2++)
        JS_SetPropertyUint32(ctx, rjs, k++, row_to_js(ctx, &t, r2, width));
    int64_t total = (int64_t)(t.n - first_data);
    table_free(&t);

    JSValue ret = JS_NewObject(ctx);
    if (JS_IsException(ret)) { JS_FreeValue(ctx, hjs); JS_FreeValue(ctx, rjs); return ret; }
    JS_SetPropertyStr(ctx, ret, "headers", hjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, total));
    return ret;
}

/* ============================ stringify ============================ */
/* dyna:csv.stringify(rows, opts?) -> string
 * The in-memory twin of writing a file. Two row forms:
 *  - string[][]: written verbatim, one line per row. A header line is just the
 *    caller's first row -- the parse() roundtrip is
 *    stringify([t.headers, ...t.rows]).
 *  - Record<string, unknown>[]: objects keyed by header, addRow's named form.
 *    The column list derives from the FIRST row's own enumerable string keys
 *    in insertion order (JS ordering: integer-like keys come first); later
 *    rows map by those names -- a missing key writes "" and an extra key is
 *    ignored, exactly like addRow. A header line of the derived names is
 *    written unless {hasHeader:false}; hasHeader has no effect on array rows
 *    (there is no header to derive from them).
 * Mixing the two forms in one call is refused. Cell values are ToString'd like
 * addRow; a throwing conversion aborts with the exception propagated. Every
 * line ends with \n, matching csv_serialize. */
static JSValue js_csv_stringify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "csv.stringify: 'rows' must be an array");
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (opts_check(ctx, opts, "csv.stringify", opts_stringify_keys, countof(opts_stringify_keys))) return JS_EXCEPTION;
    uint8_t delim, quote;
    if (opts_delim_quote(ctx, opts, "csv.stringify", &delim, &quote)) return JS_EXCEPTION;
    int write_header = opt_bool_def(ctx, opts, "hasHeader", 1);

    uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
    /* the length read can run a user getter: a throw must propagate here,
     * before any buffer or column list exists */
    if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);

    Buf out; memset(&out, 0, sizeof(out));
    char **names = NULL; uint32_t ncols = 0;
    int obj_form = 0;
    JSValue ret = JS_EXCEPTION;

    if (n) {
        JSValue r0 = JS_GetPropertyUint32(ctx, argv[0], 0);
        if (JS_IsArray(ctx, r0)) obj_form = 0;
        else if (JS_IsObject(r0)) obj_form = 1;
        else {
            JS_FreeValue(ctx, r0); buf_free(&out);
            return JS_ThrowTypeError(ctx, "csv.stringify: row 0 must be an array or an object");
        }
        JS_FreeValue(ctx, r0);

        if (obj_form) {
            /* derive the columns from row 0's OWN keys -- no prototype walk,
             * the same rule csv_own_get applies to cell values */
            JSValue first = JS_GetPropertyUint32(ctx, argv[0], 0);
            JSPropertyEnum *tab = NULL; uint32_t ntab = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, first, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(ctx, first); buf_free(&out); return JS_EXCEPTION;
            }
            names = ntab ? (char **)calloc(ntab, sizeof(char *)) : NULL;
            if (ntab && !names) {
                JS_FreePropertyEnum(ctx, tab, ntab); JS_FreeValue(ctx, first);
                buf_free(&out); return JS_ThrowOutOfMemory(ctx);
            }
            for (uint32_t i = 0; i < ntab; i++) {
                const char *nm = JS_AtomToCString(ctx, tab[i].atom);
                if (!nm) { JS_FreePropertyEnum(ctx, tab, ntab); JS_FreeValue(ctx, first); goto err; }
                names[ncols] = strdup(nm);
                JS_FreeCString(ctx, nm);
                if (!names[ncols]) {
                    JS_FreePropertyEnum(ctx, tab, ntab); JS_FreeValue(ctx, first);
                    JS_ThrowOutOfMemory(ctx); goto err;
                }
                ncols++;
            }
            JS_FreePropertyEnum(ctx, tab, ntab);
            JS_FreeValue(ctx, first);
            if (write_header) {
                for (uint32_t c = 0; c < ncols; c++) {
                    if ((c && buf_putc(&out, (char)delim)) || emit_field(&out, names[c], delim, quote)) {
                        JS_ThrowOutOfMemory(ctx); goto err;
                    }
                }
                if (buf_putc(&out, '\n')) { JS_ThrowOutOfMemory(ctx); goto err; }
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        JSValue rv = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsArray(ctx, rv)) {
            if (obj_form) {
                JS_FreeValue(ctx, rv);
                JS_ThrowTypeError(ctx, "csv.stringify: row %u: rows must all be objects keyed by header (row 0 was an object)", i);
                goto err;
            }
            uint32_t rc = 0;
            JSValue rl = JS_GetPropertyStr(ctx, rv, "length"); JS_ToUint32(ctx, &rc, rl); JS_FreeValue(ctx, rl);
            for (uint32_t c = 0; c < rc; c++) {
                JSValue cv = JS_GetPropertyUint32(ctx, rv, c);
                const char *s = JS_ToCString(ctx, cv);
                JS_FreeValue(ctx, cv);
                if (!s) { JS_FreeValue(ctx, rv); goto err; }   /* conversion threw */
                int w = (c && buf_putc(&out, (char)delim)) || emit_field(&out, s, delim, quote);
                JS_FreeCString(ctx, s);
                if (w) { JS_FreeValue(ctx, rv); JS_ThrowOutOfMemory(ctx); goto err; }
            }
        } else if (JS_IsObject(rv)) {
            if (!obj_form) {
                JS_FreeValue(ctx, rv);
                JS_ThrowTypeError(ctx, "csv.stringify: row %u: rows must all be arrays (row 0 was an array)", i);
                goto err;
            }
            for (uint32_t c = 0; c < ncols; c++) {
                JSValue v = csv_own_get(ctx, rv, names[c]);
                if (JS_IsException(v)) { JS_FreeValue(ctx, rv); goto err; }
                const char *s = NULL;
                if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                    s = JS_ToCString(ctx, v);
                    if (!s) { JS_FreeValue(ctx, v); JS_FreeValue(ctx, rv); goto err; }
                }
                int w = (c && buf_putc(&out, (char)delim)) || (s && emit_field(&out, s, delim, quote));
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                if (w) { JS_FreeValue(ctx, rv); JS_ThrowOutOfMemory(ctx); goto err; }
            }
        } else {
            JS_FreeValue(ctx, rv);
            JS_ThrowTypeError(ctx, "csv.stringify: row %u must be an array or an object", i);
            goto err;
        }
        JS_FreeValue(ctx, rv);
        if (buf_putc(&out, '\n')) { JS_ThrowOutOfMemory(ctx); goto err; }
    }

    ret = JS_NewStringLen(ctx, out.p ? out.p : "", out.len);
err:
    free_str_array(names, (int)ncols);
    buf_free(&out);
    return ret;
}

/* ============================ rows({batch}) =================================
 * Streaming row iteration for large files: `for await (const b of
 * file.rows({batch}))`. Each next() loads the file and slices ONE batch
 * [offset, offset+batch) (headers + rows + offset + totalRows), so only one
 * batch is materialised at a time (flat per-batch allocation vs the whole
 * table). Backpressure is the batch size: a slow consumer holds one batch.
 * Aligned with dyna:stream: a manual async iterator (next/return/
 * [Symbol.asyncIterator]) returning promises of {value, done}; breaking the
 * loop calls return() which marks done. Snapshot-per-batch: a concurrent
 * mutation lands on the next batch boundary. Strict bag. */
static const char *const opts_rows_keys[] = { "batch", "delimiter", "quote", "strict" };

typedef struct { char *path; int64_t offset; int64_t batch; uint8_t delim, quote; int strict; int done; } csv_rows_t;
static JSClassID csv_rows_class_id;

static void csv_rows_finalizer(JSRuntime *rt, JSValue val)
{
    csv_rows_t *it = (csv_rows_t *)JS_GetOpaque(val, csv_rows_class_id);
    (void)rt;
    if (!it) return;
    free(it->path);
    free(it);
}

static const JSClassDef csv_rows_class = { "CsvRows", .finalizer = csv_rows_finalizer };

static JSValue csv_promise_resolved(JSContext *ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, val); return promise; }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue csv_done_value(JSContext *ctx)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) return o;
    JS_SetPropertyStr(ctx, o, "value", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, o, "done", JS_TRUE);
    return o;
}

static JSValue csv_rows_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    csv_rows_t *it = (csv_rows_t *)JS_GetOpaque2(ctx, this_val, csv_rows_class_id);
    Table t;
    JSValue hjs, rjs, val, res;
    size_t total, start, end;
    uint32_t k = 0;
    size_t r, c, ncols;
    (void)argc; (void)argv;
    if (!it) return JS_EXCEPTION;
    if (it->done) return csv_promise_resolved(ctx, csv_done_value(ctx));
    memset(&t, 0, sizeof t);
    if (csv_load(ctx, it->path, &t, it->strict, it->delim, it->quote)) {
        JSValue exc = JS_GetException(ctx);
        JSValue funcs[2], promise, rr;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise)) { JS_FreeValue(ctx, exc); return promise; }
        rr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&exc);
        JS_FreeValue(ctx, exc); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (t.n == 0) { table_free(&t); it->done = 1; return csv_promise_resolved(ctx, csv_done_value(ctx)); }
    total = t.n - 1;
    ncols = table_ncols(&t);
    if ((uint64_t)it->offset >= (uint64_t)total) { table_free(&t); it->done = 1; return csv_promise_resolved(ctx, csv_done_value(ctx)); }
    start = (size_t)it->offset;
    end = start + (size_t)it->batch;
    if (end > total) end = total;
    hjs = JS_NewArray(ctx);
    rjs = JS_NewArray(ctx);
    if (JS_IsException(hjs) || JS_IsException(rjs)) {
        JS_FreeValue(ctx, hjs); JS_FreeValue(ctx, rjs);
        table_free(&t);
        return JS_EXCEPTION;
    }
    for (c = 0; c < ncols; c++)
        JS_SetPropertyUint32(ctx, hjs, (uint32_t)c, JS_NewString(ctx, cell(&t, 0, c)));
    for (r = start; r < end; r++) {
        JSValue a = JS_NewArray(ctx);
        for (c = 0; c < ncols; c++)
            JS_SetPropertyUint32(ctx, a, (uint32_t)c, JS_NewString(ctx, cell(&t, r + 1, c)));
        JS_SetPropertyUint32(ctx, rjs, k++, a);
    }
    val = JS_NewObject(ctx);
    if (JS_IsException(val)) { JS_FreeValue(ctx, hjs); JS_FreeValue(ctx, rjs); table_free(&t); return val; }
    JS_SetPropertyStr(ctx, val, "headers", hjs);
    JS_SetPropertyStr(ctx, val, "rows", rjs);
    JS_SetPropertyStr(ctx, val, "offset", JS_NewInt64(ctx, it->offset));
    JS_SetPropertyStr(ctx, val, "totalRows", JS_NewInt64(ctx, (int64_t)total));
    it->offset = (int64_t)end;
    if ((uint64_t)it->offset >= (uint64_t)total) it->done = 1;
    table_free(&t);
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) { JS_FreeValue(ctx, val); return res; }
    JS_SetPropertyStr(ctx, res, "value", val);
    JS_SetPropertyStr(ctx, res, "done", JS_FALSE);
    return csv_promise_resolved(ctx, res);
}

static JSValue csv_rows_return(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    csv_rows_t *it = (csv_rows_t *)JS_GetOpaque2(ctx, this_val, csv_rows_class_id);
    (void)argc; (void)argv;
    if (!it) return JS_EXCEPTION;
    it->done = 1;
    return csv_promise_resolved(ctx, csv_done_value(ctx));
}

static JSValue csv_rows_async_iter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry csv_rows_proto[] = {
    JS_CFUNC_DEF("next", 0, csv_rows_next),
    JS_CFUNC_DEF("return", 0, csv_rows_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, csv_rows_async_iter),
};

static JSValue js_csv_rows(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char *path = csvfile_path(ctx, this_val);
    JSValueConst o;
    csv_rows_t *it;
    JSValue obj, proto;
    int64_t batch = 1000;
    uint8_t delim = ',', quote = '"';
    int strict = 0;
    if (!path) return JS_EXCEPTION;
    o = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsObject(o)) {
        JSValue v;
        if (opts_check(ctx, o, "csv.rows", opts_rows_keys, countof(opts_rows_keys))) { free(path); return JS_EXCEPTION; }
        if (opts_delim_quote(ctx, o, "csv.rows", &delim, &quote)) { free(path); return JS_EXCEPTION; }
        strict = opt_bool(ctx, o, "strict");
        v = JS_GetPropertyStr(ctx, o, "batch");
        if (JS_IsException(v)) { free(path); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &batch, v) < 0) { JS_FreeValue(ctx, v); free(path); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, v);
    } else if (!JS_IsUndefined(o) && !JS_IsNull(o)) { free(path); return JS_ThrowTypeError(ctx, "csv.rows: opts must be an object"); }
    if (batch < 1 || batch > 100000) { free(path); return JS_ThrowRangeError(ctx, "csv.rows: batch must be 1..100000"); }
    it = (csv_rows_t *)calloc(1, sizeof *it);
    if (!it) { free(path); return JS_ThrowOutOfMemory(ctx); }
    it->path = path; it->offset = 0; it->batch = batch; it->delim = delim; it->quote = quote; it->strict = strict;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto)) { free(it->path); free(it); return proto; }
    if (JS_SetPropertyFunctionList(ctx, proto, csv_rows_proto, countof(csv_rows_proto)) < 0) {
        JS_FreeValue(ctx, proto); free(it->path); free(it); return JS_EXCEPTION;
    }
    obj = JS_NewObjectClass(ctx, (int)csv_rows_class_id);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, proto); free(it->path); free(it); return obj; }
    JS_SetOpaque(obj, it);
    JS_SetPrototype(ctx, obj, proto);
    JS_FreeValue(ctx, proto);
    /* keep the async-iterator shape minimal: next/return/asyncIterator only */
    return obj;
}

/* ============================ registration ============================ */
/* the CSVFile prototype methods (all operate on the instance path) */
static const JSCFunctionListEntry csvfile_methods[] = {
    JS_CFUNC_DEF("create", 1, js_csv_create),
    JS_CFUNC_DEF("read", 1, js_csv_read),
    JS_CFUNC_DEF("addRow", 1, js_csv_add_row),
    JS_CFUNC_DEF("updateCell", 1, js_csv_update_cell),
    JS_CFUNC_DEF("removeRow", 1, js_csv_remove_row),
    JS_CFUNC_DEF("addColumn", 1, js_csv_add_column),
    JS_CFUNC_DEF("removeColumn", 1, js_csv_remove_column),
    JS_CFUNC_DEF("renameColumn", 1, js_csv_rename_column),
    JS_CFUNC_DEF("readColumnValuesRange", 1, js_csv_read_column_values_range),
    JS_CFUNC_DEF("readRowRange", 1, js_csv_read_row_range),
    JS_CFUNC_DEF("selectColumnRange", 1, js_csv_select_column_range),
    JS_CFUNC_DEF("rows", 1, js_csv_rows),
};

/* the module-level free functions: in-memory text <-> rows, no file */
static const JSCFunctionListEntry csv_free_funcs[] = {
    JS_CFUNC_DEF("parse", 2, js_csv_parse),
    JS_CFUNC_DEF("stringify", 2, js_csv_stringify),
};

static int csv_module_init(JSContext *ctx, JSModuleDef *m) {
    JS_NewClassID(&csv_rows_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), (JSClassID)csv_rows_class_id, &csv_rows_class) < 0) return -1;
    if (dyn_register_class(ctx, m, &dyn_csvfile_class_id, &dyn_csvfile_class,
                           csvfile_methods, countof(csvfile_methods),
                           js_csvfile_ctor, "CSVFile")) return -1;
    return JS_SetModuleExportList(ctx, m, csv_free_funcs, countof(csv_free_funcs));
}

int js_nat_init_csv(JSContext *ctx) {
    simd_init(); /* select the best find_first_of / count_u8 kernels */
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:csv", csv_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "CSVFile");
    JS_AddModuleExportList(ctx, m, csv_free_funcs, countof(csv_free_funcs));
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_CSV */
