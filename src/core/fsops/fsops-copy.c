#include "fsops.h"

#define DYN_COPY_BUF (256 * 1024)

typedef struct {
    uint8_t len;
    uint8_t off;
    uint8_t magic[12];
    const char* mime;
} dyn_sniff_t;

static int dyn_copy_loop(int in, int out)
{
    char* buf = (char*)malloc(DYN_COPY_BUF);
    int rc = 0;

    if (!buf) {
        errno = ENOMEM;
        return -1;
    }
    for (;;) {
        ssize_t n = read(in, buf, DYN_COPY_BUF), off = 0;
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;
            break;
        }
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                goto done;
            }
            off += w;
        }
    }
done:
    free(buf);
    return rc;
}

int dyn_copy_fds(int in, int out)
{
#if defined(__APPLE__)
    if (fcopyfile(in, out, NULL, COPYFILE_DATA) == 0)
        return 0;
    if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0 || ftruncate(out, 0) < 0 || lseek(out, 0, SEEK_SET) < 0)
        return -1;
#elif defined(__linux__)
    {
        struct stat st;
        if (fstat(in, &st) == 0 && S_ISREG(st.st_mode)) {
            off_t done = 0;
            int ok = 1;
            while (done < st.st_size) {
                ssize_t n = (ssize_t)syscall(__NR_copy_file_range, in, NULL,
                    out, NULL,
                    (size_t)(st.st_size - done), 0u);
                if (n > 0) {
                    done += n;
                    continue;
                }
                if (n == 0)
                    break;
                ok = 0;
                break;
            }
            if (ok && st.st_size > 0 && done == st.st_size)
                return 0;
            if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0 || ftruncate(out, 0) < 0 || lseek(out, 0, SEEK_SET) < 0)
                return -1;
        }
    }
#endif
    return dyn_copy_loop(in, out);
}

static const dyn_sniff_t DYN_SNIFF[] = {
    { 8, 0, { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }, "image/png" },
    { 6, 0, { 'G', 'I', 'F', '8', '7', 'a' }, "image/gif" },
    { 6, 0, { 'G', 'I', 'F', '8', '9', 'a' }, "image/gif" },
    { 4, 0, { 'O', 'g', 'g', 'S' }, "audio/ogg" },
    { 4, 0, { 'f', 'L', 'a', 'C' }, "audio/flac" },
    { 4, 0, { 0x00, 0x61, 0x73, 0x6D }, "application/wasm" },
    { 4, 0, { 0x7F, 'E', 'L', 'F' }, "application/x-elf" },
    { 4, 0, { 0xCA, 0xFE, 0xBA, 0xBE }, "application/java-vm" },
    { 4, 0, { 0xFD, '7', 'z', 'X' }, "application/x-xz" },
    { 4, 0, { 0x28, 0xB5, 0x2F, 0xFD }, "application/zstd" },
    { 4, 0, { 'P', 'K', 0x03, 0x04 }, "application/zip" },
    { 4, 0, { 'P', 'K', 0x05, 0x06 }, "application/zip" },
    { 4, 0, { '%', 'P', 'D', 'F' }, "application/pdf" },
    { 4, 0, { 'I', 'I', '*', 0x00 }, "image/tiff" },
    { 4, 0, { 'M', 'M', 0x00, '*' }, "image/tiff" },
    { 4, 4, { 'f', 't', 'y', 'p' }, "video/mp4" },
    { 4, 0, { 'R', 'a', 'r', '!' }, "application/vnd.rar" },
    { 4, 0, { 'W', 'E', 'B', 'P' }, "image/webp" },
    { 3, 0, { 0xFF, 0xD8, 0xFF }, "image/jpeg" },
    { 3, 0, { 'B', 'Z', 'h' }, "application/x-bzip2" },
    { 3, 0, { 'I', 'D', '3' }, "audio/mpeg" },
    { 2, 0, { 0x1F, 0x8B }, "application/gzip" },
    { 2, 0, { 'B', 'M' }, "image/bmp" },
    { 2, 0, { 0x4D, 0x5A }, "application/x-msdownload" },
};

const char* dyn_sniff_bytes(const uint8_t* p, size_t n)
{
    size_t i;

    if (n >= 16 && memcmp(p, "SQLite format 3", 16) == 0)
        return "application/vnd.sqlite3";
    if (n >= 262 && memcmp(p + 257, "ustar", 5) == 0)
        return "application/x-tar";
    if (n >= 12 && memcmp(p, "RIFF", 4) == 0) {
        if (memcmp(p + 8, "WEBP", 4) == 0)
            return "image/webp";
        if (memcmp(p + 8, "WAVE", 4) == 0)
            return "audio/wav";
    }
    for (i = 0; i < sizeof DYN_SNIFF / sizeof DYN_SNIFF[0]; i++) {
        const dyn_sniff_t* s = &DYN_SNIFF[i];
        if (s->mime[0] == 'i' && s->magic[0] == 'W')
            continue;
        if (n >= (size_t)s->off + s->len
            && memcmp(p + s->off, s->magic, s->len) == 0)
            return s->mime;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return "text/plain";
    if (n && memchr(p, 0, n < 512 ? n : 512))
        return "application/octet-stream";
    return n ? "text/plain" : "application/octet-stream";
}
