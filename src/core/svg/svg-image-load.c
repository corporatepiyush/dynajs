#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include "svg-int.h"
#include <ctype.h>
#include <limits.h>
#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define IMG_POSIX 1
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define IMG_THREADLOCAL __declspec(thread)
#else
#define IMG_THREADLOCAL _Thread_local
#endif

#define IMG_FILE_MAX_BYTES ((size_t)256u << 20)
#define IMG_SVG_MAX_NEST   3

static IMG_THREADLOCAL int     g_svg_nest = 0;
static IMG_THREADLOCAL int64_t g_img_px_used = 0;

void image_budget_reset(void) {
    if (g_svg_nest == 0) g_img_px_used = 0;
}

static int budget_take(int w, int h, const svg2img_limits* lim) {
    int64_t max = lim && lim->max_pixels > 0 ? (int64_t)lim->max_pixels
                                             : (int64_t)SVG2IMG_LIMIT_MAX_PIXELS;
    int64_t n = (int64_t)w * (int64_t)h;
    if (n < 0 || n > max - g_img_px_used) return 0;
    g_img_px_used += n;
    return 1;
}

typedef union { size_t refs; max_align_t align; } ImgHdr;

static unsigned char* img_buf_alloc(size_t n) {
    if (n > SIZE_MAX - sizeof(ImgHdr)) return NULL;
    ImgHdr* h = (ImgHdr*)svg_malloc(sizeof(ImgHdr) + n);
    if (!h) return NULL;
    h->refs = 1;
    return (unsigned char*)(h + 1);
}

unsigned char* img_buf_retain(unsigned char* px) {
    if (px) ((ImgHdr*)px - 1)->refs++;
    return px;
}

void img_buf_release(unsigned char* px) {
    if (!px) return;
    ImgHdr* h = (ImgHdr*)px - 1;
    if (--h->refs == 0) svg_mfree(h);
}

static unsigned char* img_buf_adopt(unsigned char* plain, int w, int h) {
    if (!plain) return NULL;
    unsigned char* px = img_buf_alloc((size_t)w * (size_t)h * 4);
    if (px) memcpy(px, plain, (size_t)w * (size_t)h * 4);
    svg_mfree(plain);
    return px;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned char* b64decode(const char* s, size_t* outn) {
    size_t cap = strlen(s) / 4 * 3 + 4;
    unsigned char* out = (unsigned char*)svg_malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    int acc = 0, bits = 0;
    for (const char* p = s; *p; p++) {
        int v;
        if (*p >= 'A' && *p <= 'Z') v = *p - 'A';
        else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 26;
        else if (*p >= '0' && *p <= '9') v = *p - '0' + 52;
        else if (*p == '+') v = 62;
        else if (*p == '/') v = 63;
        else continue;
        acc = ((acc << 6) | v) & 0xFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) out[n++] = (unsigned char)((acc >> bits) & 0xFF);
            acc &= (1 << bits) - 1;
        }
    }
    *outn = n;
    return out;
}

static char* pctdecode(const char* s, size_t len, size_t* outn) {
    char* out = (char*)svg_malloc(len + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[n++] = (char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') out[n++] = ' ';
        else out[n++] = s[i];
    }
    out[n] = 0;
    if (outn) *outn = n;
    return out;
}

static int data_uri_is_svg(const char* href, size_t meta_len) {
    static const char mime[] = "image/svg+xml";
    const size_t off = 5;
    if (meta_len < off + sizeof mime - 1) return 0;
    for (size_t i = 0; i < sizeof mime - 1; i++)
        if (tolower((unsigned char)href[off + i]) !=
            (int)(unsigned char)mime[i]) return 0;
    return 1;
}

static int href_is_safe_relative(const char* href) {
    if (!href || !*href || href[0] == '/' || href[0] == '#') return 0;
    for (const char* p = href; *p; p++) {
        if (*p == ':' || *p == '\\' || (unsigned char)*p < 0x20) return 0;
        if (*p == '/' && p[1] == '/') return 0;
    }
    const char* seg = href;
    for (;;) {
        const char* e = strchr(seg, '/');
        size_t n = e ? (size_t)(e - seg) : strlen(seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        if (!e) break;
        seg = e + 1;
    }
    return 1;
}

static unsigned char* read_file(const char* base_path, const char* path,
                                size_t* out_n) {
#ifdef IMG_POSIX

    char rbase[PATH_MAX], rpath[PATH_MAX];
    if (!realpath(base_path, rbase) || !realpath(path, rpath)) return NULL;
    size_t bl = strlen(rbase);
    if (strncmp(rpath, rbase, bl) != 0) return NULL;
    if (!(bl == 1 && rbase[0] == '/') && rpath[bl] != '/') return NULL;

    int fd = open(rpath, O_RDONLY | O_NONBLOCK
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
                  );
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > (unsigned long long)IMG_FILE_MAX_BYTES) {
        close(fd);
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    unsigned char* buf = (unsigned char*)svg_malloc(sz);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != sz) { svg_mfree(buf); return NULL; }
    *out_n = got;
    return buf;
#else
    (void)base_path;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > (unsigned long)IMG_FILE_MAX_BYTES) {
        fclose(f); return NULL;
    }
    rewind(f);
    unsigned char* buf = (unsigned char*)svg_malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { svg_mfree(buf); return NULL; }
    *out_n = got;
    return buf;
#endif
}

static unsigned char* decode_bytes(const unsigned char* d, size_t n,
                                   int* w, int* h) {
    unsigned char* px = NULL;
    if (n >= 8 && d[0] == 137 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        px = png_decode_rgba(d, n, w, h);
    else if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8)
        px = jpeg_decode_rgba(d, n, w, h);
    return img_buf_adopt(px, *w, *h);
}

static unsigned char* render_svg_image(const char* svg, size_t slen, int aa,
                                       const svg2img_limits* lim,
                                       int* out_w, int* out_h, int* limit) {
    if (g_svg_nest >= IMG_SVG_MAX_NEST) { *limit = 1; return NULL; }
    svg2img_limits rl;
    limits_resolve(lim, &rl);
    svg2img_options o;
    memset(&o, 0, sizeof(o));
    o.aa = aa;
    o.scale = 1.0f;
    o.limits = rl;
    int hit = 0;
    g_svg_nest++;
    Node* root = svg_parse(svg, slen, &o, &hit);
    unsigned char* px = NULL;
    if (hit) *limit = 1;
    if (!root || !root->vp.valid || root->vp.w <= 0 || root->vp.h <= 0)
        goto done;
    int w = root->vp.w, h = root->vp.h;
    int64_t npx = (int64_t)w * (int64_t)h;
    if (!budget_take(w, h, &rl)) { *limit = 1; goto done; }
    Canvas* src = canvas_create(w, h);
    if (!src) goto done;
    render_document(src, root, mat_identity(), &o, &rl);
    px = img_buf_alloc((size_t)npx * 4);
    if (px) {
        for (size_t i = 0; i < (size_t)npx; i++) {
            const float* s = src->px + i * 4;
            unsigned char* d = px + i * 4;
            d[0] = (unsigned char)(s[0] * 255.0f + 0.5f);
            d[1] = (unsigned char)(s[1] * 255.0f + 0.5f);
            d[2] = (unsigned char)(s[2] * 255.0f + 0.5f);
            d[3] = (unsigned char)(s[3] * 255.0f + 0.5f);
        }
        *out_w = w;
        *out_h = h;
    }
    canvas_free(src);
done:
    svg_free(root);
    g_svg_nest--;
    return px;
}

int image_load_any(const char* href, const char* base_path, int aa,
                   const svg2img_limits* lim,
                   unsigned char** out, int* out_w, int* out_h) {
    *out = NULL;
    *out_w = *out_h = 0;
    if (!href || !*href) return 0;

    if (!strncmp(href, "data:", 5)) {
        const char* comma = strchr(href, ',');
        if (!comma) return 0;
        size_t meta_len = (size_t)(comma - href);
        const char* data = comma + 1;
        size_t data_len = strlen(data);
        int is_b64 = meta_len >= 7 && !strncmp(comma - 7, ";base64", 7);
        if (data_uri_is_svg(href, meta_len)) {

            char* svg = NULL;
            size_t slen = 0;
            if (is_b64) svg = (char*)b64decode(data, &slen);
            else svg = pctdecode(data, data_len, &slen);
            if (!svg) return 0;
            int limit = 0;
            unsigned char* px = render_svg_image(svg, slen, aa, lim,
                                                 out_w, out_h, &limit);
            svg_mfree(svg);
            if (!px) return limit ? IMG_LIMIT : 0;
            *out = px;
            return 1;
        }
        unsigned char* raw = NULL;
        size_t rawn = 0;
        if (is_b64) raw = b64decode(data, &rawn);
        else {
            char* dec = pctdecode(data, data_len, &rawn);
            raw = (unsigned char*)dec;
        }
        if (!raw) return 0;
        unsigned char* px = decode_bytes(raw, rawn, out_w, out_h);
        svg_mfree(raw);
        if (!px) return 0;
        if (!budget_take(*out_w, *out_h, lim)) {
            img_buf_release(px);
            *out_w = *out_h = 0;
            return IMG_LIMIT;
        }
        *out = px;
        return 1;
    }

    if (!base_path || !*base_path || !href_is_safe_relative(href)) return 0;
    char path[4096];
    size_t bl = strlen(base_path);
    size_t hl = strlen(href);
    if (bl + hl + 2 > sizeof path) return 0;
    memcpy(path, base_path, bl);
    if (base_path[bl - 1] != '/') path[bl++] = '/';
    memcpy(path + bl, href, hl + 1);
    size_t n = 0;
    unsigned char* raw = read_file(base_path, path, &n);
    if (!raw) return 0;
    unsigned char* px = decode_bytes(raw, n, out_w, out_h);
    svg_mfree(raw);
    if (!px) return 0;
    if (!budget_take(*out_w, *out_h, lim)) {
        img_buf_release(px);
        *out_w = *out_h = 0;
        return IMG_LIMIT;
    }
    *out = px;
    return 1;
}
