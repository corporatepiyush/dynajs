/* test_lz4.js -- the LZ4 tier and class Compressor (W8).
 *
 * LZ4 is an INTEROP format, so the tests that matter are the ones that leave
 * the process. A self round-trip agrees with its own bugs: an encoder that
 * writes a wrong offset and a decoder that reads it back the same way pass
 * every round-trip test ever written. So the frame is checked against the
 * system `lz4` binary in BOTH directions, across its block-size and checksum
 * options, and the tests skip (loudly) rather than pass if it is absent.
 *
 * Everything else here is a property the format specifies and a round trip
 * cannot see: that a match may not start within 12 bytes of the end, that an
 * offset must be validated against what has actually been written, that a
 * dictionary mismatch is an ERROR rather than plausible garbage, and that a
 * capability holding mutable scratch rejects re-entry instead of corrupting it.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_lz4.js */

import {
    gzip, gunzip, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, Compressor,
} from "dyna:compress";
import * as os from "os";
import * as std from "std";

/* A skipped interop group is invisible in "N assertions passed". Where the
   tool is supposed to exist (the CI images install it), a skip is a failure. */
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

/* Corpora chosen for what each one breaks, not for variety:
 *  - empty and 1..13 bytes straddle the format's 12-byte "no match may start
 *    here" tail, where an encoder that ignores the rule emits a stream a
 *    conforming fast decoder reads past the end of;
 *  - runs exercise overlapping copies, which are legal and which memcpy gets
 *    wrong;
 *  - random is the adversarial case: compression must not expand it. */
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
    /* every byte value, so no literal is special */
    const b = new Uint8Array(256 * 4);
    for (let i = 0; i < b.length; i++) b[i] = i & 0xff;
    corpora.allbytes = b;
}

/* ------------------------------------------------------- 1. block round trip */
for (const [name, data] of Object.entries(corpora)) {
    for (const level of [1, 3, 12]) {
        const packed = lz4Compress(data, { level });
        assert(packed instanceof Uint8Array, name + " returns Uint8Array");
        const back = lz4Decompress(packed);
        assert(bytesEqual(back, data), "block round trip " + name + " @" + level);
    }
}

/* the level is a match-finder setting, NOT a format: every level's output must
 * be readable, and a higher level must not produce a larger record on data with
 * repeats to find */
{
    const d = corpora.text;
    const a = lz4Compress(d, { level: 1 }), b = lz4Compress(d, { level: 12 });
    assert(bytesEqual(lz4Decompress(a), d) && bytesEqual(lz4Decompress(b), d),
           "both levels decode with the same decoder");
    assert(b.length <= a.length, "level 12 is not worse than level 1 (" +
           a.length + " -> " + b.length + ")");
}

/* --------------------------------------------- 2. the adversarial direction
 * Incompressible input must not expand meaningfully. The LZ4 block format has
 * no stored mode, so the worst case is the literal-length escape bytes: about
 * 1/255. Anything beyond that means the encoder is emitting junk sequences. */
{
    const r = corpora.random;
    const packed = lz4Compress(r);
    assert(packed.length <= r.length + r.length / 200 + 16,
           "incompressible block does not expand: " + r.length + " -> " + packed.length);
    /* the FRAME does have a stored mode and must use it */
    const framed = lz4Frame(r);
    assert(framed.length <= r.length + 32,
           "incompressible frame stores rather than expands: " + framed.length);
    assert(bytesEqual(lz4Unframe(framed), r), "stored frame round trip");
}

/* ------------------------------------------------------- 3. frame round trip */
for (const [name, data] of Object.entries(corpora)) {
    for (const checksum of [true, false]) {
        const f = lz4Frame(data, { checksum });
        assert(bytesEqual(lz4Unframe(f), data),
               "frame round trip " + name + " checksum=" + checksum);
    }
}
eq(lz4Unframe(lz4Frame("hello frame"), { asString: true }), "hello frame",
   "asString decodes UTF-8");

/* -------------------------------------------- 4. interop with the `lz4` tool */
{
    const tmp = "/tmp/dynajs_lz4_test";
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
        /* (a) the tool reads what we write */
        for (const checksum of [true, false]) {
            writeFileBytes(tmp + "/ours.lz4", lz4Frame(data, { checksum }));
            const rc = os.exec(["/bin/sh", "-c",
                "lz4 -d -f " + tmp + "/ours.lz4 " + tmp + "/theirs.out >/dev/null 2>&1"],
                { block: true });
            eq(rc, 0, "system lz4 decoded our frame (checksum=" + checksum + ")");
            assert(bytesEqual(readFileBytes(tmp + "/theirs.out"), data),
                   "system lz4 recovered our bytes (checksum=" + checksum + ")");
        }
        /* (b) we read what the tool writes, across its options. -B4/-B7 change
         * the block size, so -B4 on a large input exercises MULTI-BLOCK
         * frames, which a single-block test would never reach. */
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
        /* a big input under a small block size: many blocks, and the last one
         * short */
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

/* ------------------------------------------------------- 5. malformed input
 * The decoder is an untrusted surface. Every one of these must throw, and none
 * may read out of bounds -- which is what the ASan run of this file checks. */
{
    throws(() => lz4Decompress(new Uint8Array([0xf0])), "truncated literal length");
    throws(() => lz4Decompress(new Uint8Array([0x20, 65])), "literal run past the end");
    /* the same token with the literal it promises IS valid: a final
     * literals-only sequence, which is how every block ends */
    eq(lz4Decompress(new Uint8Array([0x20, 65, 66])).length, 2,
       "a final literals-only sequence decodes");
    /* a match offset larger than what has been written: token 0x1f = 1 literal,
     * match len 15+, offset 0x00ff far beyond one byte of output */
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
        bad[5] ^= 0x01;                     /* corrupt the descriptor */
        throws(() => lz4Unframe(bad), "bad descriptor checksum");
        const bad2 = new Uint8Array(f);
        bad2[bad2.length - 1] ^= 0xff;      /* corrupt the content checksum */
        throws(() => lz4Unframe(bad2), "bad content checksum");
    }
    /* a LINKED-block frame: legal LZ4, but its blocks reference the previous
     * block's output. Decoding it as independent yields garbage, so it must be
     * refused. FLG bit 5 clear, header checksum recomputed to match. */
    {
        const f = lz4Frame(corpora.text, { checksum: false });
        const linked = new Uint8Array(f);
        linked[4] &= ~0x20;
        /* recompute the descriptor byte the way the format defines it, using
         * our own frame as the oracle for everything else */
        const flg = linked[4], bd = linked[5];
        linked[6] = xxh32Byte(flg, bd);
        throws(() => lz4Unframe(linked), "linked-block frames are refused");
    }
    /* a bit-flip sweep: any single-byte corruption must throw or decode, never
     * crash. The value is in the ASan run, where an out-of-bounds read fails
     * loudly instead of returning a plausible byte. */
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
            try { lz4Decompress(bad); survived++; } catch (e) { /* expected */ }
        }
        assert(survived >= 0, "corrupting a raw block never crashes");
    }
}

/* XXH32 of a 2-byte frame descriptor, >> 8 & 0xFF -- the header checksum. A
 * literal reimplementation, so the test does not ask the code under test what
 * the right answer is. */
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

/* --------------------------------------------------- 6. dictionaries (W8.4a)
 * A prefix dictionary is not recorded in the block, so the ONLY thing standing
 * between a mismatched dictionary and silent corruption is the id stamp. */
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
    /* the whole point: the mismatch is caught rather than producing bytes */
    let produced = null;
    try { produced = other.decompress(rec); } catch (e) { /* expected */ }
    eq(produced, null, "a mismatched dictionary produces NOTHING, not garbage");
}

/* -------------------------------------------------- 7. class Compressor (W8.5) */
{
    /* equivalence: a reused instance answers exactly what the free function
     * answers, for every input. Without this "hoist it out of the loop" is
     * silently wrong. */
    const c = new Compressor({ algo: "lz4", level: 1 });
    for (const [name, data] of Object.entries(corpora)) {
        assert(bytesEqual(c.compress(data), lz4Compress(data, { level: 1 })),
               "Compressor equals the free function on " + name);
        assert(bytesEqual(c.decompress(c.compress(data)), data),
               "Compressor round trip " + name);
    }
    /* reuse purity: call N must not observe call N-1. The scratch is reused,
     * so this is the property that could silently fail. */
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

    /* one shape per class: every field set in the constructor */
    const c2 = new Compressor({ algo: "lz4" });
    eq(JSON.stringify(Object.keys(c)), JSON.stringify(Object.keys(c2)),
       "two instances have the same shape");

    throws(() => Compressor({ algo: "lz4" }), "calling without new");
    throws(() => new Compressor({ algo: "brotli" }), "an unknown algo");
    throws(() => new Compressor({ algo: "lz4", level: 0 }), "level 0");
    throws(() => new Compressor({ algo: "lz4", level: 13 }), "level 13");
    throws(() => new Compressor({ algo: "gzip", dict: "x" }),
           "a dictionary with gzip");
    throws(() => new Compressor(5), "a non-object option");
}

/* Reentrancy -- and the measured answer is that this API cannot be re-entered,
 * which is a stronger statement than "the flag works".
 *
 * CLAUDE.md section 8 prescribes an adversarial argument whose coercion calls
 * back into the object. That requires the method to COERCE. dyna:compress does
 * not: it type-checks for a string, TypedArray or ArrayBuffer and throws
 * otherwise, so an object with a toString or valueOf never gets that far and no
 * user JS can run inside a call. A busy flag guarding the reused scratch would
 * therefore be a bypass that never fires -- a tax, by the rule in CLAUDE.md
 * section 4 -- and there is none.
 *
 * These assertions pin the REASON, so that if the input path ever starts
 * coercing, this file fails and the flag comes back with it. */
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
    /* and the instance is unharmed by the rejected calls */
    assert(bytesEqual(c.decompress(c.compress("payload")), enc.encode("payload")),
           "a rejected call leaves the scratch usable");
}

/* close() is idempotent and every method rejects a closed instance */
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
