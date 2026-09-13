/* copyFile, move and sniffType.
   sniffType reads MAGIC BYTES, never the extension: the extension is what the
   sender claims and the bytes are what arrived, so every case below names a
   file the opposite of what it contains. */
import { copyFile, move, sniffType, writeFile, readFile, exists, remove,
         makeTempDir, stat, chmod, Path } from "dyna:file";

let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { fails++; print("FAIL " + m + ": wrong reason -- " + e.message); } }
}
const dir = makeTempDir("dynfc");
const p = (f) => new Path(String(dir) + "/" + f);
const bytes = (...a) => new Uint8Array(a);

try {
    /* --- copyFile --- */
    writeFile(p("src.txt"), "hello copy");
    eq(copyFile(p("src.txt"), p("dst.txt")), 10, "copyFile returns the byte count");
    eq(readFile(p("dst.txt")), "hello copy", "the content survives the copy");

    /* An existing destination is REFUSED by default: a copy that silently
       replaces a file is the one nobody notices until the file is gone. */
    threw(() => copyFile(p("src.txt"), p("dst.txt")), /exists|EEXIST/i,
          "copying onto an existing file");
    writeFile(p("src2.txt"), "second");
    copyFile(p("src2.txt"), p("dst.txt"), { overwrite: true });
    eq(readFile(p("dst.txt")), "second", "overwrite:true replaces it");

    /* The source's mode carries over -- a copy of a private key that lands
       world-readable is the failure this exists to prevent. */
    writeFile(p("secret"), "k");
    chmod(p("secret"), 0o600);
    copyFile(p("secret"), p("secret.copy"));
    eq(stat(p("secret.copy")).mode & 0o777, 0o600, "the source mode is preserved");

    threw(() => copyFile(p("nope"), p("x")), /no such file|ENOENT/i, "a missing source");
    threw(() => copyFile(new Path(String(dir)), p("x")), /directory/i, "copying a directory");
    threw(() => copyFile(p("src.txt")), /both are required/, "one argument");

    /* A large copy exercises the kernel path (fcopyfile / copy_file_range)
       past the 256 KiB fallback block, so both sides of that branch run. */
    {
        const big = "x".repeat(700 * 1024);
        writeFile(p("big.bin"), big);
        eq(copyFile(p("big.bin"), p("big2.bin")), big.length, "a 700 KiB copy");
        eq(readFile(p("big2.bin")).length, big.length, "and it is all there");
    }

    /* --- move --- */
    writeFile(p("m1.txt"), "moved");
    move(p("m1.txt"), p("m2.txt"));
    eq(exists(p("m1.txt")), false, "the source is gone after a move");
    eq(readFile(p("m2.txt")), "moved", "and the content arrived");
    threw(() => move(p("nope"), p("x")), /no such file|ENOENT/i, "moving a missing file");
    /* NOTE: the cross-filesystem arm (rename fails EXDEV, so it falls back to
       copy-then-unlink) cannot be reached from one temp directory. It is
       unverified here rather than proven, and saying so beats implying cover. */

    /* --- sniffType: bytes --- */
    eq(sniffType(bytes(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)), "image/png", "PNG magic");
    eq(sniffType(bytes(0xFF, 0xD8, 0xFF, 0xE0)), "image/jpeg", "JPEG magic");
    eq(sniffType(bytes(0x47, 0x49, 0x46, 0x38, 0x39, 0x61)), "image/gif", "GIF89a");
    eq(sniffType(bytes(0x25, 0x50, 0x44, 0x46, 0x2D)), "application/pdf", "PDF magic");
    eq(sniffType(bytes(0x50, 0x4B, 0x03, 0x04)), "application/zip", "ZIP magic");
    eq(sniffType(bytes(0x1F, 0x8B, 0x08)), "application/gzip", "gzip magic");
    eq(sniffType(bytes(0x00, 0x61, 0x73, 0x6D, 1, 0, 0, 0)), "application/wasm", "wasm magic");
    eq(sniffType(bytes(0x28, 0xB5, 0x2F, 0xFD)), "application/zstd", "zstd magic");

    /* RIFF containers share a four-byte prefix and are told apart at offset 8 --
       a table keyed only on byte 0 would call a WAV a WebP. */
    {
        const riff = (tag) => {
            const a = new Uint8Array(16);
            "RIFF".split("").forEach((c, i) => a[i] = c.charCodeAt(0));
            tag.split("").forEach((c, i) => a[8 + i] = c.charCodeAt(0));
            return a;
        };
        eq(sniffType(riff("WEBP")), "image/webp", "RIFF/WEBP");
        eq(sniffType(riff("WAVE")), "audio/wav", "RIFF/WAVE, not WebP");
    }

    /* SQLite's magic is 16 bytes at offset 0; tar's is at 257. Both would be
       missed by a sniffer that only reads a short prefix. */
    {
        const sq = new Uint8Array(32);
        "SQLite format 3\0".split("").forEach((c, i) => sq[i] = c.charCodeAt(0));
        eq(sniffType(sq), "application/vnd.sqlite3", "SQLite header");
        const tar = new Uint8Array(300);
        "ustar".split("").forEach((c, i) => tar[257 + i] = c.charCodeAt(0));
        eq(sniffType(tar), "application/x-tar", "tar magic at offset 257");
    }

    /* No magic: a NUL in the first block means binary, otherwise text. */
    eq(sniffType(bytes(104, 101, 108, 108, 111)), "text/plain", "plain bytes are text");
    eq(sniffType(bytes(104, 0, 108)), "application/octet-stream", "a NUL means binary");
    eq(sniffType(new Uint8Array(0)), "application/octet-stream", "empty is not text");

    /* --- sniffType: paths, named to contradict their contents --- */
    writeFile(p("actually.png"), "this is really just text");
    eq(sniffType(p("actually.png")), "text/plain",
       "the extension does not decide it");
    {
        const png = bytes(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0);
        writeFile(p("actually.txt"), png);
        eq(sniffType(p("actually.txt")), "image/png", "the bytes decide it");
    }
    threw(() => sniffType(p("missing.bin")), /no such file|ENOENT/i, "sniffing a missing path");
    threw(() => sniffType(), /required/, "sniffType with no argument");
} finally {
    try { remove(new Path(String(dir)), { recursive: true }); } catch (e) { }
}

if (fails) {
    print("test_file_copy: " + fails + " FAILED of " + n);
    throw new Error("test_file_copy failed");
}
print("test_file_copy: " + n + " assertions, 0 failures");
