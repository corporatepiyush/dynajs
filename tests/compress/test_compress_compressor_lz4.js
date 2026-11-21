// flags: --std
// covers: dyna:compress.Compressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_compressor_lz4");
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

function xxh32Byte(flg, bd) {
    const P1 = 2654435761, P2 = 2246822519, P3 = 3266489917, P5 = 374761393;
    const mul = (a, b) => Math.imul(a, b) >>> 0;
    const rotl = (x, r) => (((x << r) | (x >>> (32 - r))) >>> 0);
    let h = (P5 + 2) >>> 0;
    for (const byte of [flg, bd]) {
        h = (h + mul(byte, P5)) >>> 0;
        h = mul(rotl(h, 11), P1);
    }
    h = (h ^ (h >>> 15)) >>> 0;
    h = mul(h, P2);
    h = (h ^ (h >>> 13)) >>> 0;
    h = mul(h, P3);
    h = (h ^ (h >>> 16)) >>> 0;
    return (h >>> 8) & 0xff;
}

__kitS.section("the dictionary helps a short templated payload:", () => {
    const dict = '{"jsonrpc":"2.0","method":"subscribe","params":';
    const msg = '{"jsonrpc":"2.0","method":"subscribe","params":[1,2,3]}';
    const withDict = lz4Compress(msg, { dict });
    const without = lz4Compress(msg);
    assert(withDict.length < without.length,
           "the dictionary helps a short templated payload: " +
           without.length + " -> " + withDict.length);
    assert(dec.decode(lz4Decompress(withDict, { dict })) === msg,
           "dictionary round trip");

    const c = new Compressor({ algo: "lz4", dict });
    const rec = c.compress(msg);
    assert(typeof c.dictId === "number", "a dictionary Compressor reports its id");
    eq(dec.decode(c.decompress(rec)), msg, "Compressor dictionary round trip");
    const other = new Compressor({ algo: "lz4", dict: dict + "!" });
    throws(() => other.decompress(rec), "a mismatched dictionary throws");
    const plain = new Compressor({ algo: "lz4" });
    eq(plain.dictId, null, "no dictionary, no id");
    throws(() => plain.decompress(rec), "a dictionary record is not a plain block");
    let produced = null;
    try { produced = other.decompress(rec); } catch (e) {  }
    eq(produced, null, "a mismatched dictionary produces NOTHING, not garbage");
});

__kitS.section("Compressor equals the free function on", () => {
    const c = new Compressor({ algo: "lz4", level: 1 });
    for (const [name, data] of Object.entries(corpora)) {
        assert(bytesEqual(c.compress(data), lz4Compress(data, { level: 1 })),
               "Compressor equals the free function on " + name);
        assert(bytesEqual(c.decompress(c.compress(data)), data),
               "Compressor round trip " + name);
    }
    const first = c.compress(corpora.text);
    for (let i = 0; i < 20; i++) c.compress(corpora.random);
    assert(bytesEqual(c.compress(corpora.text), first),
           "20 intervening calls do not change the answer");

    const g = new Compressor({ algo: "gzip" });
    assert(bytesEqual(g.compress(corpora.text), gzip(corpora.text)),
           "gzip Compressor equals gzip()");
    assert(bytesEqual(gunzip(g.compress(corpora.text)), corpora.text),
           "and its output is real gzip");
    eq(g.algo, "gzip", "algo getter");

    const f = new Compressor({ algo: "lz4frame" });
    assert(bytesEqual(lz4Unframe(f.compress(corpora.text)), corpora.text),
           "lz4frame Compressor writes a real frame");

    const c2 = new Compressor({ algo: "lz4" });
    eq(JSON.stringify(Object.keys(c)), JSON.stringify(Object.keys(c2)),
       "two instances have the same shape");

    throws(() => Compressor({ algo: "lz4" }), "calling without new");
    throws(() => new Compressor({ algo: "zstdx" }), "an unknown algo");
    throws(() => new Compressor({ algo: "lz4", level: 0 }), "level 0");
    throws(() => new Compressor({ algo: "lz4", level: 13 }), "level 13");
    throws(() => new Compressor({ algo: "gzip", dict: "x" }),
           "a dictionary with gzip");
    throws(() => new Compressor(5), "a non-object option");
});

__kitS.section("an object argument is rejected, not coerced", () => {
    const c = new Compressor({ algo: "lz4" });
    let fired = false;
    const attack = { toString() { fired = true; c.compress("x"); return "payload"; } };
    throws(() => c.compress(attack),
           "an object argument is rejected, not coerced");
    eq(fired, false, "so its toString never ran -- the attack is VACUOUS here, " +
       "and a test written to CLAUDE.md section 8 as stated would prove nothing");
    const valueOfAttack = { valueOf() { fired = true; return "payload"; } };
    throws(() => c.compress(valueOfAttack), "valueOf is not reached either");
    eq(fired, false, "confirmed for both hooks");
    assert(bytesEqual(c.decompress(c.compress("payload")), enc.encode("payload")),
           "a rejected call leaves the scratch usable");
});

__kitS.section("compress after close", () => {
    const c = new Compressor({ algo: "lz4" });
    c.compress("x");
    c.close();
    c.close();
    throws(() => c.compress("x"), "compress after close");
    throws(() => c.decompress(new Uint8Array([0])), "decompress after close");
});
__kitS.end();

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));
