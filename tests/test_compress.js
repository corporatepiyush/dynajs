// flags: --std
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
{
    const r = new Uint8Array(5000);
    let x = 0x12345678 >>> 0;
    for (let i = 0; i < r.length; i++) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        r[i] = (x >>> 16) & 0xff;
    }
    corpora.random = r;
}
{
    const b = new Uint8Array(3000);
    for (let i = 0; i < b.length; i++)
        b[i] = (i % 7 === 0) ? 0x00 : ((i * 31 + (i >> 3)) & 0xff);
    corpora.binaryNul = b;
}

for (const [name, data] of Object.entries(corpora)) {
    const packed = gzip(data);
    assert(packed instanceof Uint8Array, "gzip returns Uint8Array (" + name + ")");
    assert(packed.length >= 18, "gzip output has header+trailer (" + name + ")");
    assert(packed[0] === 0x1f && packed[1] === 0x8b && packed[2] === 0x08,
           "gzip magic + deflate method (" + name + ")");
    const back = gunzip(packed);
    assert(back instanceof Uint8Array, "gunzip returns Uint8Array (" + name + ")");
    assert(bytesEqual(back, data), "round-trip preserves bytes (" + name + ")");
}

{
    function ratio(name) {
        const data = corpora[name];
        const packed = gzip(data);
        const r = packed.length / data.length;
        print("  ratio[" + name + "]: " + data.length + " -> " +
              packed.length + " bytes (" + r.toFixed(4) + ")");
        return r;
    }
    assert(ratio("repeated") < 0.2, "repeated data compresses hard (<0.2)");
    assert(ratio("newlines") < 0.2, "newline runs compress hard (<0.2)");
    assert(ratio("english") < 0.6, "English text actually shrinks (<0.6)");
    const rr = ratio("random");
    assert(rr < 1.02, "random data does not blow up (stored fallback)");
}

{
    const data = corpora.english;
    const mb = data.length / (1024 * 1024);
    let iters = 40, best = Infinity;
    for (let pass = 0; pass < 5; pass++) {
        const t0 = performance.now();
        for (let i = 0; i < iters; i++) gzip(data);
        const dt = (performance.now() - t0) / iters;
        if (dt < best) best = dt;
    }
    print("  gzip throughput: " + (mb / (best / 1000)).toFixed(1) +
          " MB/s (" + best.toFixed(3) + " ms / " + data.length + " bytes)");
}

{
    const text = "The quick brown fox jumps over the lazy dog. ".repeat(50);
    const packedStr = gzip(text);
    const decoded = gunzip(packedStr, { asString: true });
    assert(typeof decoded === "string", "asString yields a string");
    assert(decoded === text, "string round-trip via asString");

    const ab = enc.encode(text).buffer;
    const packedAb = gunzip(gzip(ab), { asString: true });
    assert(packedAb === text, "ArrayBuffer input round-trips");
}

for (const name of ["repeated", "english", "binaryNul", "random", "small"]) {
    const data = corpora[name];
    const packed = gzip(data);
    const gzPath = "tmp_compress_a.gz";
    const outPath = "tmp_compress_a.out";
    writeFileBytes(gzPath, packed);
    const rc = os.exec(["/bin/sh", "-c", "gzip -dc " + gzPath + " > " + outPath],
                       { usePath: false });
    assert(rc === 0, "system gzip -dc decoded our output (" + name + ", rc=" +
           rc + ")");
    const sysOut = readFileBytes(outPath);
    assert(bytesEqual(sysOut, data),
           "system gzip bytes match original (" + name + ")");
    os.remove(gzPath);
    os.remove(outPath);
}

{
    const data = corpora.newlines;
    const inPath = "tmp_compress_b.in";
    const gzPath = "tmp_compress_b.gz";
    writeFileBytes(inPath, data);
    const rc = os.exec(["/bin/sh", "-c",
                        "gzip -c " + inPath + " > " + gzPath],
                       { usePath: false });
    assert(rc === 0, "system gzip produced a file (rc=" + rc + ")");
    const sysGz = readFileBytes(gzPath);
    const back = gunzip(sysGz);
    assert(bytesEqual(back, data), "our gunzip decoded system gzip output");
    os.remove(inPath);
    os.remove(gzPath);
}

{
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
}

{
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    const many = new Array(65536);
    for (let i = 0; i < many.length; i++)
        many[i] = { name: "f" + i, data: new Uint8Array(0) };
    throws(() => ZipPack(many),
           "65536 entries are refused (the EOCD count field is 16-bit)");
    const few = ZipPack([{ name: "a.txt", data: enc.encode("hi") }],
                        { method: "store" });
    assert(few instanceof Uint8Array, "a small ZipPack still works");
    const edge = new Array(65535);
    for (let i = 0; i < edge.length; i++)
        edge[i] = { name: "f" + i, data: "" };
    const t0 = performance.now();
    const big = ZipPack(edge, { method: "store" });
    assert(big instanceof Uint8Array && big.length > 65535 * 32,
           "65535 entries (the exact boundary) still pack");
    print("  ZipPack 65535 entries: " + (performance.now() - t0).toFixed(0) +
          " ms, " + (big.length / 1048576).toFixed(1) + " MB");
}

{
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    throws(() => gzip(corpora.small, "9"), "a string level is a TypeError");
    throws(() => gzip(corpora.small, { level: "9" }), "an options level that is a string is a TypeError");
    throws(() => gzip(corpora.small, { level: null }), "an options level of null is a TypeError");
    assert(bytesEqual(gzip(corpora.small, 9), gzip(corpora.small)),
           "a numeric level is accepted (tiny payload: same fixed bytes)");
    assert(bytesEqual(gzip(corpora.small), gzip(corpora.small, undefined)),
           "an absent level still works");
    assert(bytesEqual(gzip(corpora.small, { level: 9 }), gzip(corpora.small, 9)),
           "options {level:9} == positional 9");
}

{
    const deepcomb = (() => {
        const parts = [];
        let a = 1, b = 1;
        for (let i = 0; i < 28; i++) {
            parts.push(new Uint8Array(b).fill(i));
            const t = a + b; a = b; b = t;
        }
        const flat = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
        let off = 0;
        for (const p of parts) { flat.set(p, off); off += p.length; }
        let x = 0x2545f491 >>> 0;
        for (let i = flat.length - 1; i > 0; i--) {
            x = (Math.imul(x, 1103515245) + 12345) >>> 0;
            const j = ((x >>> 16) & 0x7fffffff) % (i + 1);
            const t = flat[i]; flat[i] = flat[j]; flat[j] = t;
        }
        return flat;
    })();

    for (const [name, data] of Object.entries(corpora)) {
        const lo = gzip(data, 1), hi = gzip(data, 6);
        assert(bytesEqual(gunzip(hi), data),
               "level 6 round-trips (" + name + ")");
        assert(hi.length <= lo.length,
               "level 6 never exceeds level 1 (" + name + ": " +
               hi.length + " vs " + lo.length + ")");
    }
    assert(bytesEqual(gunzip(gzip(deepcomb, 6)), deepcomb),
           "level 6 round-trips a >15-bit-deep Huffman tree");
    assert(gzip(deepcomb, 6).length <= gzip(deepcomb, 1).length,
           "level 6 never exceeds level 1 (deep tree)");
    {
        const lo = gzip(corpora.english, 1), hi = gzip(corpora.english, 6);
        print("  level 6 vs 1 [english]: " + lo.length + " -> " + hi.length +
              " (" + (hi.length / lo.length).toFixed(3) + ")");
        assert(hi.length < lo.length * 0.95,
               "dynamic Huffman beats fixed by >5% on English text");
    }
    for (const data of [corpora.english, deepcomb]) {
        const gzPath = "tmp_compress_l6.gz";
        const outPath = "tmp_compress_l6.out";
        writeFileBytes(gzPath, gzip(data, 6));
        const rc = os.exec(["/bin/sh", "-c",
                            "gzip -dc " + gzPath + " > " + outPath],
                           { usePath: false });
        assert(rc === 0, "system gzip -dc decoded our level-6 output (rc=" +
               rc + ")");
        assert(bytesEqual(readFileBytes(outPath), data),
               "system gzip level-6 bytes match original");
        os.remove(gzPath);
        os.remove(outPath);
    }
    {
        function throws(fn, msg) {
            let threw = false;
            try { fn(); } catch { threw = true; }
            assert(threw, msg);
        }
        const c6 = new Compressor({ algo: "gzip", level: 6 });
        const packed = c6.compress(corpora.english);
        assert(bytesEqual(c6.decompress(packed), corpora.english),
               "Compressor gzip level 6 round-trips");
        assert(packed.length === gzip(corpora.english, 6).length,
               "Compressor level 6 matches the one-shot size");
        throws(() => new Compressor({ algo: "gzip", level: 0 }),
               "gzip level 0 is out of bounds (1..9)");
        throws(() => new Compressor({ algo: "gzip", level: 10 }),
               "gzip level 10 is out of bounds (1..9)");
        c6.close();
    }
}

{
    const text = "The quick brown fox jumps over the lazy dog. ".repeat(50);
    for (const algo of ["gzip", "lz4", "lz4frame", "snappy"]) {
        const c = new Compressor({ algo });
        const packed = c.compress(text);
        assert(typeof c.decompress(packed, { asString: true }) === "string",
               algo + ": asString yields a string");
        assert(c.decompress(packed, { asString: true }) === text,
               algo + ": asString round-trips the text");
        assert(c.decompress(packed) instanceof Uint8Array,
               algo + ": default stays bytes");
    }
}

{
    const fr = new Uint8Array([
        0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x58, 0x29, 0x00, 0x00,
        0x68, 0x65, 0x6C, 0x6C, 0x6F]);
    let have = true;
    try {
        const dec = unzstd(fr);
        have = dec.length === 5 && dec[0] === 0x68;
    } catch { have = false; }
    if (!have) {
        print("  zstd not compiled in: skipping the codec-prefix check");
    } else {
        function msg(fn) {
            try { fn(); } catch (e) { return String(e); }
            return "";
        }
        const bad = fr.slice();
        bad[6] ^= 0xff;
        const m = msg(() => unzstd(bad));
        assert(m.indexOf("unzstd: zstd: ") >= 0,
               "a corrupt zstd stream is thrown as \"unzstd: zstd: <reason>\" (got " +
               JSON.stringify(m) + ")");
        const c = new Compressor({ algo: "zstd" });
        const m2 = msg(() => c.decompress(bad));
        assert(m2.indexOf("Compressor.decompress: zstd: ") >= 0,
               "the class path carries the same codec prefix (got " + JSON.stringify(m2) + ")");
    }
}


{
    const octal = (buf, off, n, val) => {
        const t = val.toString(8).padStart(n - 1, "0") + "\0";
        for (let i = 0; i < n; i++) buf[off + i] = t.charCodeAt(i);
    };
    const hdr = new Uint8Array(512);
    for (let i = 0; i < 3; i++) hdr[i] = "pax".charCodeAt(i);
    octal(hdr, 100, 8, 0o644);
    octal(hdr, 124, 12, 512);
    hdr[156] = 0x78;
    for (let i = 148; i < 156; i++) hdr[i] = 0x20;
    let sum = 0; for (const b of hdr) sum += b;
    const cs = sum.toString(8).padStart(6, "0") + "\0 ";
    for (let i = 0; i < 8; i++) hdr[148 + i] = cs.charCodeAt(i);
    const pax = new Uint8Array(512);
    pax.set([0x35,0x30,0x35,0x20,0x78,0x3d], 0);
    pax[504] = 0x0a;
    pax.set([0x37,0x20,0x73,0x69,0x7a,0x65,0x3d], 505);
    const tar = new Uint8Array(1024);
    tar.set(hdr, 0); tar.set(pax, 512);
    let got;
    try { got = TarExtract(tar).length; } catch (e) { got = -1; }
    assert(got === 0, "PAX '=' at the final byte is skipped, not overread (got " + got + ")");
}

{
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
}

{
    function eq(got, want, msg) {
        assert(got === want, msg + " (got " + got + ", want " + want + ")");
    }
    function ok(c, msg) { assert(c, msg); }
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    eq(compressBound(0, { algo: "gzip" }), 23, "gzip bound(0) = 18 + 5");
    eq(compressBound(65535, { algo: "gzip" }), 18 + 65535 + 5, "gzip bound(65535): one block");
    eq(compressBound(65536, { algo: "gzip" }), 18 + 65536 + 10, "gzip bound(65536): two blocks");
    eq(compressBound(131070, { algo: "gzip" }), 18 + 131070 + 10, "gzip bound(2 blocks exact)");
    eq(compressBound(0, { algo: "lz4" }), 16, "lz4 bound(0)");
    eq(compressBound(255, { algo: "lz4" }), 255 + 1 + 16, "lz4 bound(255)");
    eq(compressBound(510, { algo: "lz4" }), 510 + 2 + 16, "lz4 bound(510)");
    eq(compressBound(0, { algo: "lz4frame" }), 7 + 4 + 16 + 8 + 4, "lz4frame bound(0)");
    eq(compressBound(4 * 1024 * 1024, { algo: "lz4frame" }),
       7 + 1 * 4 + (4 * 1024 * 1024 + Math.floor(4 * 1024 * 1024 / 255) + 16) + 8 + 4,
       "lz4frame bound(one full block)");
    eq(compressBound(4 * 1024 * 1024 + 1, { algo: "lz4frame" }),
       7 + 2 * 4 + (4 * 1024 * 1024 + 1 + Math.floor((4 * 1024 * 1024 + 1) / 255) + 16) + 8 + 4,
       "lz4frame bound(block boundary + 1)");
    eq(compressBound(0, { algo: "snappy" }), 16, "snappy bound(0)");
    eq(compressBound(100, { algo: "snappy" }), 100 + 2 + 16, "snappy bound(100)");
    ok(compressBound(1000, { algo: "zstd" }) >= 1000, "zstd bound >= input");
    ok(compressBound(1000, { algo: "brotli" }) >= 1000, "brotli bound >= input");

    const incompressible = new Uint8Array(70000);
    let seed = 12345;
    for (let i = 0; i < incompressible.length; i++) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        incompressible[i] = seed & 0xff;
    }
    for (const algo of ["gzip", "lz4", "lz4frame", "zstd", "brotli", "snappy"]) {
        const bound = compressBound(incompressible.length, { algo });
        let out;
        if (algo === "gzip") out = gzip(incompressible);
        else if (algo === "lz4") out = lz4Compress(incompressible);
        else if (algo === "lz4frame") out = lz4Frame(incompressible);
        else if (algo === "zstd") out = zstd(incompressible);
        else if (algo === "brotli") out = brotli(incompressible);
        else out = snappy(incompressible);
        ok(out.length <= bound, algo + " output " + out.length + " fits bound " + bound);
    }

    throws(() => compressBound(-1, { algo: "gzip" }), "negative n throws");
    throws(() => compressBound(1e18, { algo: "gzip" }), "absurd n throws");
    throws(() => compressBound(10, { algo: "nope" }), "unknown algo throws");
    throws(() => compressBound(10, {}), "missing algo throws");
    throws(() => compressBound(10, "gzip"), "non-object opts throws");
}

{
    function eq(got, want, msg) {
        assert(got === want, msg + " (got " + got + ", want " + want + ")");
    }
    function ok(c, msg) { assert(c, msg); }
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    const msg = "the options object and the positional level drive the same encoder";
    eq(gunzip(gzip(msg, { level: 9 }), { asString: true }), msg, "gzip({level:9}) round-trips");
    eq(gunzip(gzip(msg, { level: 1 }), { asString: true }), msg, "gzip({level:1}) round-trips");
    eq(gunzip(gzip(msg, {}), { asString: true }), msg, "gzip({}) uses the default level");
    eq(gunzip(gzip(msg, { level: undefined }), { asString: true }), msg, "level: undefined is absent");
    const a = gzip(msg, 9), b = gzip(msg, { level: 9 });
    ok(a.length === b.length && a.join() === b.join(), "positional == options output");
    throws(() => gzip(msg, { level: "9" }), "options level must be a number");
    throws(() => gzip(msg, { level: null }), "options level null throws");
    throws(() => gzip(msg, "9"), "positional level must be a number");
}

{
    function throwsMsg(fn, re, msg) {
        let threw = null;
        try { fn(); } catch (e) { threw = e; }
        assert(threw !== null && threw instanceof TypeError && re.test(threw.message),
               msg + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    }
    const msg = "hello CC-6";
    throwsMsg(() => gzip(msg, { levle: 9 }), /^unknown option "levle" \(valid: level\)$/, "gzip bag");
    throwsMsg(() => gunzip(msg, { asStrng: true }), /unknown option "asStrng" \(valid: asString\)/, "gunzip bag");
    throwsMsg(() => lz4Frame(msg, { cheksum: true }), /unknown option "cheksum" \(valid: checksum, level, dict\)/, "lz4Frame bag (superset table)");
    throwsMsg(() => lz4Compress(msg, { dikt: null }), /unknown option "dikt" \(valid: level, dict\)/, "lz4Compress bag");
    throwsMsg(() => unzstd(msg, { asString2: 1 }), /unknown option "asString2" \(valid: asString\)/, "unzstd bag");
    throwsMsg(() => compressBound(10, { algo2: "lz4" }), /unknown option "algo2" \(valid: algo\)/, "compressBound bag");
    throwsMsg(() => new Compressor({ alg: "lz4" }), /unknown option "alg" \(valid: algo, checksum, level, dict\)/, "Compressor ctor bag");
    throwsMsg(() => new Compressor({ algo: "lz4" }).decompress(msg, { asStr: 1 }), /unknown option "asStr" \(valid: asString\)/, "decompress bag");
    assert(gunzip(gzip(msg, { level: 1 }), { asString: true }) === msg, "valid gzip/gunzip bags still work");
    assert(gunzip(gzip(msg, 1), null) instanceof Uint8Array, "null bag accepted as absent");
    throwsMsg(() => gunzip(msg, ["asString"]), /unknown option "0"/, "array bag throws");
}

{
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
}

print("test_compress: all tests passed (" + n + " assertions)");
