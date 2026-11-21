// flags: --std
// covers: dyna:compress.gunzip
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gunzip");
import { gzip, gunzip, zstd, unzstd, brotli, snappy, lz4Compress, lz4Frame, compressBound, Compressor, ZipPack, TarExtract } from "dyna:compress";
import * as std from "std";
import * as os from "os";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) return false;
    return true;
}

function readFileBytes(path) {
    const [st, serr] = os.stat(path);
    if (serr) throw new Error("stat failed: " + path);
    const size = st.size;
    const fd = os.open(path, os.O_RDONLY);
    if (fd < 0) throw new Error("open failed: " + path);
    const buf = new Uint8Array(size);
    let got = 0;
    while (got < size) {
        const r = os.read(fd, buf.buffer, got, size - got);
        if (r <= 0) break;
        got += r;
    }
    os.close(fd);
    if (got !== size) throw new Error("short read: " + path);
    return buf;
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

function u8(...vals) { return new Uint8Array(vals); }

const enc = new TextEncoder();
const englishText =
    "The quick brown fox jumps over the lazy dog. Pack my box with five " +
    "dozen liquor jugs. How vexingly quick daft zebras jump! Sphinx of " +
    "black quartz, judge my vow. The five boxing wizards jump quickly. ";
const corpora = {
    empty: new Uint8Array(0),
    oneByte: enc.encode("Z"),
    small: enc.encode("hello, gzip world!"),
    repeated: enc.encode("abcdefgh".repeat(4000)),
    newlines: enc.encode("line\n".repeat(1000)),
    english: enc.encode(englishText.repeat(80)),
};

__kitS.section("empty input throws", () => {
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    throws(() => gunzip(u8()), "empty input throws");
    throws(() => gunzip(u8(1, 2, 3)), "too-short input throws");
    throws(() => gunzip(u8(0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff)),
           "header-only (no deflate/trailer) throws");
    throws(() => gunzip(u8(0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0xff, 1, 2, 3, 4,
                          5, 6, 7, 8)), "bad magic throws");

    const good = gzip(corpora.small);
    for (let cut = 0; cut < good.length; cut++) {
        throws(() => gunzip(good.slice(0, cut)),
               "truncated @" + cut + " throws");
    }

    for (let i = 0; i < good.length; i++) {
        const bad = good.slice();
        bad[i] ^= 0xff;
        try { gunzip(bad); } catch {  }
    }
    const big = gzip(corpora.repeated);
    for (let i = 12; i < big.length - 8; i += 7) {
        const bad = big.slice();
        bad[i] ^= 0x55;
        try { gunzip(bad); } catch {  }
    }
});

__kitS.section("first member;", () => {
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    {
        const a = enc.encode("first member;"),
              b = enc.encode("second member"),
              c = enc.encode("third");
        const multi = new Uint8Array(
            [...gzip(a), ...gzip(b), ...gzip(c)]);
        const out = new TextDecoder().decode(gunzip(multi));
        assert(out === "first member;second memberthird",
               "a concatenated multi-member file decodes whole (got " +
               JSON.stringify(out) + ")");
    }
    {
        const padded = new Uint8Array([...gzip(corpora.small), ...new Uint8Array(512)]);
        assert(new TextDecoder().decode(gunzip(padded)) === "hello, gzip world!",
               "trailing zero padding is tolerated");
    }
    throws(() => gunzip(new Uint8Array([...gzip(corpora.small), ...enc.encode("junk")])),
           "trailing non-member garbage throws");
    {
        function crc32(bytes, off, len) {
            let c = 0xffffffff;
            for (let i = off; i < off + len; i++) {
                c ^= bytes[i];
                for (let k = 0; k < 8; k++)
                    c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
            }
            return (c ^ 0xffffffff) >>> 0;
        }
        const payload = enc.encode("header-checked");
        const full = gzip(payload);
        const body = full.slice(10, full.length - 8);
        const hdr = new Uint8Array(12);
        hdr.set([0x1f, 0x8b, 0x08, 0x02, 0, 0, 0, 0, 0, 0xff]);
        const hc = crc32(hdr, 0, 10) & 0xffff;
        hdr[10] = hc & 0xff;
        hdr[11] = (hc >>> 8) & 0xff;
        const member = new Uint8Array([...hdr, ...body, ...full.slice(full.length - 8)]);
        assert(new TextDecoder().decode(gunzip(member)) === "header-checked",
               "a valid FHCRC header decodes");
        const bad = member.slice();
        bad[6] ^= 0x01;
        throws(() => gunzip(bad), "a corrupted FHCRC header throws");
        const badhc = member.slice();
        badhc[10] ^= 0x01;
        throws(() => gunzip(badhc), "a wrong FHCRC value throws");
    }
});

__kitS.section("(got", () => {
    function throwsMsg(fn, re, msg) {
        let threw = null;
        try { fn(); } catch (e) { threw = e; }
        assert(threw !== null && threw instanceof TypeError && re.test(threw.message),
               msg + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    }
    const msg = "hello CC-6";
    throwsMsg(() => gzip(msg, { levle: 9 }), /^unknown option "levle" \(valid: level\)$/, "gzip bag");
    throwsMsg(() => gunzip(msg, { asStrng: true }), /unknown option "asStrng" \(valid: asString, maxOutputBytes\)/, "gunzip bag");
    throwsMsg(() => lz4Frame(msg, { cheksum: true }), /unknown option "cheksum" \(valid: checksum, level, dict\)/, "lz4Frame bag (superset table)");
    throwsMsg(() => lz4Compress(msg, { dikt: null }), /unknown option "dikt" \(valid: level, dict\)/, "lz4Compress bag");
    throwsMsg(() => unzstd(msg, { asString2: 1 }), /unknown option "asString2" \(valid: asString, maxOutputBytes\)/, "unzstd bag");
    throwsMsg(() => compressBound(10, { algo2: "lz4" }), /unknown option "algo2" \(valid: algo\)/, "compressBound bag");
    throwsMsg(() => new Compressor({ alg: "lz4" }), /unknown option "alg" \(valid: algo, checksum, level, dict\)/, "Compressor ctor bag");
    throwsMsg(() => new Compressor({ algo: "lz4" }).decompress(msg, { asStr: 1 }), /unknown option "asStr" \(valid: asString, maxOutputBytes\)/, "decompress bag");
    assert(gunzip(gzip(msg, { level: 1 }), { asString: true }) === msg, "valid gzip/gunzip bags still work");
    assert(gunzip(gzip(msg, 1), null) instanceof Uint8Array, "null bag accepted as absent");
    throwsMsg(() => gunzip(msg, ["asString"]), /unknown option "0"/, "array bag throws");
});

__kitS.section("(got (L174)", () => {
    function eqErr(fn, check, msg) {
        let e = null;
        try { fn(); } catch (err) { e = err; }
        assert(e !== null && check(e),
               msg + " (got " + (e ? e.name + ": " + e.message : "no throw") + ")");
    }
    const data = gzip("adv", 1);
    eqErr(() => gunzip(data, new Proxy({}, { ownKeys() { throw new RangeError("ownKeys trap"); } })),
          (e) => e instanceof RangeError && e.message === "ownKeys trap",
          "Proxy ownKeys trap propagates");
    const symBag = {};
    symBag[Symbol("asStrng")] = true;
    assert(gunzip(data, symBag) instanceof Uint8Array, "symbol keys invisible");
    assert(gunzip(data, Object.create({ asStrng: true })) instanceof Uint8Array,
           "inherited keys invisible");
    const ne = {};
    Object.defineProperty(ne, "asStrng", { value: true, enumerable: false });
    assert(gunzip(data, ne) instanceof Uint8Array, "non-enumerable own keys invisible");
    const tg = {};
    Object.defineProperty(tg, "asString", { get() { throw new RangeError("getter boom"); }, enumerable: true });
    eqErr(() => gunzip(data, tg),
          (e) => e instanceof RangeError && e.message === "getter boom",
          "throwing getter on known key propagates");
    assert(gunzip(data, 42) instanceof Uint8Array, "primitive bag accepted as absent");
});
__kitS.end();

print("test_compress: all tests passed (" + n + " assertions)");
