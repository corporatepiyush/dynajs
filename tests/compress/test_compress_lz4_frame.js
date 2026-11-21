// flags: --std
// covers: dyna:compress.lz4Frame
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_frame");
import {
    gzip, gunzip, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, Compressor,
} from "dyna:compress";
import * as os from "os";
import * as std from "std";

const REQUIRE_TOOLS = (std.getenv("DYNAJS_REQUIRE_TOOLS") === "1");
let n = 0, skipped = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}
function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function readFileBytes(path) {
    const [st, serr] = os.stat(path);
    if (serr) return null;
    const fd = os.open(path, os.O_RDONLY);
    if (fd < 0) return null;
    const buf = new Uint8Array(st.size);
    let got = 0;
    while (got < st.size) {
        const r = os.read(fd, buf.buffer, got, st.size - got);
        if (r <= 0) break;
        got += r;
    }
    os.close(fd);
    return got === st.size ? buf : null;
}
function writeFileBytes(path, u8) {
    const fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644);
    if (fd < 0) throw new Error("open(w) failed: " + path);
    let put = 0;
    while (put < u8.length) {
        const w = os.write(fd, u8.buffer, put, u8.length - put);
        if (w <= 0) break;
        put += w;
    }
    os.close(fd);
    if (put !== u8.length) throw new Error("short write: " + path);
}

const enc = new TextEncoder();
const dec = new TextDecoder();

const corpora = {};
corpora.empty = new Uint8Array(0);
for (const k of [1, 4, 5, 11, 12, 13, 16, 64]) {
    const b = new Uint8Array(k);
    for (let i = 0; i < k; i++) b[i] = 65 + (i % 26);
    corpora["len" + k] = b;
}
corpora.runs = enc.encode("a".repeat(1000) + "b".repeat(1000) + "ab".repeat(500));
corpora.text = enc.encode("the quick brown fox jumps over the lazy dog. ".repeat(400));
corpora.json = enc.encode(JSON.stringify(
    Array.from({ length: 400 }, (_, i) => ({ jsonrpc: "2.0", id: i, method: "sub" }))));
fixture: {
    const r = new Uint8Array(20000);
    let x = 0x2545f491 >>> 0;
    for (let i = 0; i < r.length; i++) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        r[i] = (x >>> 16) & 0xff;
    }
    corpora.random = r;
}
fixture: {
    const b = new Uint8Array(256 * 4);
    for (let i = 0; i < b.length; i++) b[i] = i & 0xff;
    corpora.allbytes = b;
}

for (const [name, data] of Object.entries(corpora)) {
    for (const checksum of [true, false]) {
        const f = lz4Frame(data, { checksum });
        assert(bytesEqual(lz4Unframe(f), data),
               "frame round trip " + name + " checksum=" + checksum);
    }
}

__kitS.section("rm -rf", () => {
    const tmp = `${std.getenv("TMPDIR") || "/tmp"}/dj_lz4.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}`;
    os.exec(["/bin/sh", "-c", "rm -rf " + tmp + " && mkdir -p " + tmp],
            { block: true });
    const have = os.exec(["/bin/sh", "-c", "command -v lz4 >/dev/null 2>&1"],
                         { block: true }) === 0;
    if (!have) {
        if (REQUIRE_TOOLS)
            throw new Error("REQUIRED tool missing: lz4 -- the interop half of "
                            + "this file is the only foreign-implementation oracle");
        skipped++;
        console.log("  NOTE: no `lz4` binary -- the interop half of this file " +
                    "did not run, and a self round trip does not replace it");
    } else {
        const data = corpora.text;
        for (const checksum of [true, false]) {
            writeFileBytes(tmp + "/ours.lz4", lz4Frame(data, { checksum }));
            const rc = os.exec(["/bin/sh", "-c",
                "lz4 -d -f " + tmp + "/ours.lz4 " + tmp + "/theirs.out >/dev/null 2>&1"],
                { block: true });
            eq(rc, 0, "system lz4 decoded our frame (checksum=" + checksum + ")");
            assert(bytesEqual(readFileBytes(tmp + "/theirs.out"), data),
                   "system lz4 recovered our bytes (checksum=" + checksum + ")");
        }
        writeFileBytes(tmp + "/orig.bin", data);
        for (const opt of ["-1", "-9", "--no-frame-crc", "-BX",
                           "-B4", "-B7", "-BD"]) {
            const rc = os.exec(["/bin/sh", "-c",
                "lz4 " + opt + " -f " + tmp + "/orig.bin " + tmp + "/t.lz4 >/dev/null 2>&1"],
                { block: true });
            eq(rc, 0, "system lz4 " + opt + " produced a frame");
            const theirs = readFileBytes(tmp + "/t.lz4");
            assert(bytesEqual(lz4Unframe(theirs), data),
                   "we decoded system lz4 " + opt);
        }
        {
            const big = new Uint8Array(700000);
            for (let i = 0; i < big.length; i++) big[i] = (i * 7 + (i >> 5)) & 0xff;
            writeFileBytes(tmp + "/big.bin", big);
            os.exec(["/bin/sh", "-c",
                "lz4 -1 -B4 -f " + tmp + "/big.bin " + tmp + "/big.lz4 >/dev/null 2>&1"],
                { block: true });
            assert(bytesEqual(lz4Unframe(readFileBytes(tmp + "/big.lz4")), big),
                   "we decoded a multi-block system frame");
            writeFileBytes(tmp + "/bigours.lz4", lz4Frame(big));
            eq(os.exec(["/bin/sh", "-c",
                "lz4 -d -f " + tmp + "/bigours.lz4 " + tmp + "/big.out >/dev/null 2>&1"],
                { block: true }), 0, "system lz4 decoded our 700KB frame");
            assert(bytesEqual(readFileBytes(tmp + "/big.out"), big),
                   "700KB survived the tool");
        }
        os.exec(["/bin/sh", "-c", "rm -rf " + tmp], { block: true });
    }
});
__kitS.end();

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));
