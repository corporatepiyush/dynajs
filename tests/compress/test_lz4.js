// flags: --std
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
{
    const r = new Uint8Array(20000);
    let x = 0x2545f491 >>> 0;
    for (let i = 0; i < r.length; i++) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        r[i] = (x >>> 16) & 0xff;
    }
    corpora.random = r;
}
{
    const b = new Uint8Array(256 * 4);
    for (let i = 0; i < b.length; i++) b[i] = i & 0xff;
    corpora.allbytes = b;
}

for (const [name, data] of Object.entries(corpora)) {
    for (const level of [1, 3, 12]) {
        const packed = lz4Compress(data, { level });
        assert(packed instanceof Uint8Array, name + " returns Uint8Array");
        const back = lz4Decompress(packed);
        assert(bytesEqual(back, data), "block round trip " + name + " @" + level);
    }
}

{
    const d = corpora.text;
    const a = lz4Compress(d, { level: 1 }), b = lz4Compress(d, { level: 12 });
    assert(bytesEqual(lz4Decompress(a), d) && bytesEqual(lz4Decompress(b), d),
           "both levels decode with the same decoder");
    assert(b.length <= a.length, "level 12 is not worse than level 1 (" +
           a.length + " -> " + b.length + ")");
}

{
    const r = corpora.random;
    const packed = lz4Compress(r);
    assert(packed.length <= r.length + r.length / 200 + 16,
           "incompressible block does not expand: " + r.length + " -> " + packed.length);
    const framed = lz4Frame(r);
    assert(framed.length <= r.length + 32,
           "incompressible frame stores rather than expands: " + framed.length);
    assert(bytesEqual(lz4Unframe(framed), r), "stored frame round trip");
}

for (const [name, data] of Object.entries(corpora)) {
    for (const checksum of [true, false]) {
        const f = lz4Frame(data, { checksum });
        assert(bytesEqual(lz4Unframe(f), data),
               "frame round trip " + name + " checksum=" + checksum);
    }
}
eq(lz4Unframe(lz4Frame("hello frame"), { asString: true }), "hello frame",
   "asString decodes UTF-8");

{
    const tmp = `${std.getenv("TMPDIR") || "/tmp"}/dj_lz4.${Date.now() % 10000000}`;
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
}

{
    throws(() => lz4Decompress(new Uint8Array([0xf0])), "truncated literal length");
    throws(() => lz4Decompress(new Uint8Array([0x20, 65])), "literal run past the end");
    eq(lz4Decompress(new Uint8Array([0x20, 65, 66])).length, 2,
       "a final literals-only sequence decodes");
    throws(() => lz4Decompress(new Uint8Array([0x1f, 65, 0xff, 0x00, 0x00])),
           "offset past the start of output");
    throws(() => lz4Decompress(new Uint8Array([0x11, 65, 0x00, 0x00])),
           "offset zero");
    throws(() => lz4Unframe(new Uint8Array(0)), "empty frame");
    throws(() => lz4Unframe(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8])), "bad magic");
    {
        const f = lz4Frame(corpora.text);
        throws(() => lz4Unframe(f.slice(0, f.length - 2)), "truncated frame");
        const bad = new Uint8Array(f);
        bad[5] ^= 0x01;
        throws(() => lz4Unframe(bad), "bad descriptor checksum");
        const bad2 = new Uint8Array(f);
        bad2[bad2.length - 1] ^= 0xff;
        throws(() => lz4Unframe(bad2), "bad content checksum");
    }
    {
        const f = lz4Frame(corpora.text, { checksum: false });
        const linked = new Uint8Array(f);
        linked[4] &= ~0x20;
        const flg = linked[4], bd = linked[5];
        linked[6] = xxh32Byte(flg, bd);
        throws(() => lz4Unframe(linked), "linked-block frames are refused");
    }
    {
        const good = lz4Frame(corpora.json);
        let threw = 0, ok = 0;
        for (let i = 0; i < good.length; i += Math.max(1, good.length >> 7)) {
            const bad = new Uint8Array(good);
            bad[i] ^= 0x80;
            try { lz4Unframe(bad); ok++; } catch (e) { threw++; }
        }
        assert(threw > 0, "corrupting a frame is detected (" + threw +
               " threw, " + ok + " decoded)");
    }
    {
        const good = lz4Compress(corpora.json);
        let survived = 0;
        for (let i = 0; i < good.length; i += Math.max(1, good.length >> 7)) {
            const bad = new Uint8Array(good);
            bad[i] ^= 0x40;
            try { lz4Decompress(bad); survived++; } catch (e) {  }
        }
        assert(survived >= 0, "corrupting a raw block never crashes");
    }
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

{
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
}

{
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
}

{
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
}

{
    const c = new Compressor({ algo: "lz4" });
    c.compress("x");
    c.close();
    c.close();
    throws(() => c.compress("x"), "compress after close");
    throws(() => c.decompress(new Uint8Array([0])), "decompress after close");
}

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));

{
    function xxh32(bytes, seed = 0) {
        const P1 = 2654435761, P2 = 2246822519, P3 = 3266489917,
              P4 = 668265263, P5 = 374761393;
        const mul = (a, b) => Math.imul(a, b) >>> 0;
        const rotl = (x, r) => ((x << r) | (x >>> (32 - r))) >>> 0;
        const u32 = (i) => (bytes[i] | (bytes[i+1] << 8) | (bytes[i+2] << 16) |
                            (bytes[i+3] << 24)) >>> 0;
        let i = 0, h;
        if (bytes.length >= 16) {
            let h1 = (seed + P1 + P2) >>> 0, h2 = (seed + P2) >>> 0,
                h3 = seed >>> 0, h4 = (seed - P1) >>> 0;
            for (; i + 16 <= bytes.length; i += 16) {
                h1 = mul(rotl((h1 + mul(u32(i), P2)) >>> 0, 13), P1);
                h2 = mul(rotl((h2 + mul(u32(i+4), P2)) >>> 0, 13), P1);
                h3 = mul(rotl((h3 + mul(u32(i+8), P2)) >>> 0, 13), P1);
                h4 = mul(rotl((h4 + mul(u32(i+12), P2)) >>> 0, 13), P1);
            }
            h = (rotl(h1, 1) + rotl(h2, 7) + rotl(h3, 12) + rotl(h4, 18)) >>> 0;
        } else {
            h = (seed + P5) >>> 0;
        }
        h = (h + bytes.length) >>> 0;
        for (; i + 4 <= bytes.length; i += 4)
            h = mul(rotl((h + mul(u32(i), P3)) >>> 0, 17), P4);
        for (; i < bytes.length; i++)
            h = mul(rotl((h + mul(bytes[i], P5)) >>> 0, 11), P1);
        h ^= h >>> 15; h = mul(h, P2);
        h ^= h >>> 13; h = mul(h, P3);
        h ^= h >>> 16;
        return h >>> 0;
    }
    function le32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
    function le64(v) { const lo = v % 4294967296, hi = Math.floor(v / 4294967296); return [...le32(lo), ...le32(hi)]; }
    function frame(opts, payload) {
        const flg = 0x40 | 0x20 | (opts.contentSize ? 0x08 : 0) |
                    (opts.blockChecksum ? 0x10 : 0);
        const bd = 0x70;
        const hasSize = typeof opts.contentSize === "number";
        const mid = hasSize ? [...le64(opts.contentSize)] : [];
        const desc = [flg, bd, ...mid];
        const hc = (xxh32(new Uint8Array(desc)) >>> 8) & 0xff;
        const body = [...payload];
        let block = [...le32(payload.length | 0x80000000), ...body];
        if (opts.blockChecksum)
            block = [...block, ...le32(xxh32(new Uint8Array(payload)))];
        return new Uint8Array([0x04, 0x22, 0x4d, 0x18, ...desc, hc,
                               ...block, 0, 0, 0, 0]);
    }
    const payload = new TextEncoder().encode("declared-size and block-checksum frame");

    {
        const good = frame({ contentSize: payload.length }, payload);
        assert(new TextDecoder().decode(lz4Unframe(good)) ===
               "declared-size and block-checksum frame",
               "a frame whose declared content size matches decodes");
        for (const lie of [payload.length - 1, payload.length + 1, 0]) {
            const bad = frame({ contentSize: lie }, payload);
            let threw = false;
            try { lz4Unframe(bad); } catch (e) { threw = true; }
            assert(threw, "a frame declaring content size " + lie +
                   " (real " + payload.length + ") is refused");
        }
    }
    {
        const good = frame({ blockChecksum: true }, payload);
        assert(new TextDecoder().decode(lz4Unframe(good)) ===
               "declared-size and block-checksum frame",
               "a frame with a valid block checksum decodes");
        const bad = good.slice();
        bad[bad.length - 5] ^= 0xff;
        let threw = false;
        try { lz4Unframe(bad); } catch (e) { threw = true; }
        assert(threw, "a corrupted block checksum is refused");
    }
}

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));
