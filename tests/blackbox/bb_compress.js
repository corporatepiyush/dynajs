// Black-box contract test for dyna:compress, generated from dynajs.d.ts lines 499-668. Engine sources not consulted; every expectation cites the contract.
import {
    zstd, unzstd, brotli, unbrotli, snappy, unsnappy,
    lz4Compress, lz4Decompress, lz4Frame, lz4Unframe,
    gzip, gunzip, compressBound,
    TarPack, TarList, TarExtract,
    ZipPack, ZipList, ZipRead, ZipReadAt, ZipExtractAll,
    deflate, inflate, Compressor, Dictionary,
} from "dyna:compress";
import { CRC32, CRC32C } from "dyna:hash";
import { fromUtf8 } from "dyna:bytes";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function eqArr2(a, b) { return eqArr(Array.from(a), Array.from(b)); }
function u8(...bytes) { return new Uint8Array(bytes); }
function pseudoRandomBytes(len) { const a = new Uint8Array(len); let s = 0x9E3779B9 >>> 0; for (let i = 0; i < len; i++) { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; a[i] = s & 0xFF; } return a; } // xorshift32 over all 32 bits: an LCG's LOW byte has period 256 and is compressible
function rep(byteVal, len) { const a = new Uint8Array(len); a.fill(byteVal); return a; }
function leU32(buf, off) { return ((buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0); }

/* Table runners: rows are [args..., expected] (expected may be a predicate) or [args..., ErrorClass]. */
function labelOf(name, args) {
    return name + "(" + args.map(a => typeof a === "string" ? JSON.stringify(a.length > 24 ? a.slice(0, 24) + "…" : a) : (a instanceof Uint8Array ? "u8[" + a.length + "]" : String(a))).join(", ") + ")";
}
function J(v) { return typeof v === "bigint" ? v + "n" : JSON.stringify(v); }
function assertCases(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const want = row[row.length - 1];
        n++;
        let got, threw = null;
        try { got = fn(...args); } catch (e) { threw = e; }
        const label = labelOf(name, args);
        if (threw) throw new Error("case threw " + threw + ": " + label);
        if (typeof want === "function") { if (!want(got)) throw new Error("case predicate failed: " + label + " — got |" + J(got) + "|"); }
        else if (J(got) !== J(want)) throw new Error("case failed: " + label + " — got |" + J(got) + "| expected |" + J(want) + "|");
    }
}
function assertCasesThrow(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const Err = row[row.length - 1];
        n++;
        let threw = false, err = null;
        try { fn(...args); } catch (e) { threw = true; err = e; }
        const label = labelOf(name, args);
        if (!threw) throw new Error("expected throw: " + label);
        if (Err && !(err instanceof Err)) throw new Error("wrong error class " + (err && err.constructor ? err.constructor.name : String(err)) + ": " + label);
    }
}

/* ---------------- round-trip matrix: every codec pair the module exports ---------------- */
{
    // d.ts exports these one-shot pairs; each must invert over the payload matrix
    // (empty / 1 byte / repetitive text / one-value / pseudo-random).
    const payloads = [
        ["empty", u8()],
        ["1-byte", u8(0x41)],
        ["text", fromUtf8("hello hello hello hello hello hello hello hello")],
        ["repeat", rep(0x5A, 4096)],
        ["random", pseudoRandomBytes(1024)],
    ];
    const codecs = [
        ["zstd", (p) => zstd(p), (p) => unzstd(p)],
        ["brotli", (p) => brotli(p), (p) => unbrotli(p)],
        ["snappy", (p) => snappy(p), (p) => unsnappy(p)],
        ["lz4", (p) => lz4Compress(p), (p) => lz4Decompress(p)],
        ["lz4frame", (p) => lz4Frame(p), (p) => lz4Unframe(p)],
        ["gzip", (p) => gzip(p), (p) => gunzip(p)],
        ["deflate", (p) => deflate(p), (p) => inflate(p)],
    ];
    for (const [nm, comp, decomp] of codecs) {
        for (const [pn, p] of payloads) {
            assert(eqArr2(decomp(comp(p)), p), "round trip: " + nm + " / " + pn + " (d.ts codec pair)");
        }
    }
    // the documented {asString} decode doors return the original text
    const s = "hello hello hello hello hello";
    assertCases((comp, decomp) => decomp(comp(s), { asString: true }), [
        // lambdas must FORWARD the opts argument (the original (p) => unzstd(p) dropped it,
        // so the decoders never saw asString and returned bytes)
        [(p, o) => zstd(p), (p, o) => unzstd(p, o), s],
        [(p, o) => brotli(p), (p, o) => unbrotli(p, o), s],
        [(p, o) => snappy(p), (p, o) => unsnappy(p, o), s],
        [(p, o) => lz4Frame(p), (p, o) => lz4Unframe(p, o), s],
        [(p, o) => gzip(p), (p, o) => gunzip(p, o), s],
        [(p, o) => deflate(p), (p, o) => inflate(p, o), s],
    ], "asString decode");
    // refusal rows for the decoders (d.ts: malformed input is refused)
    assertCasesThrow((...a) => unzstd(...a), [[u8(1, 2, 3), null]], "unzstd");
    assertCasesThrow((...a) => unbrotli(...a), [[u8(9, 9, 9), null]], "unbrotli");
    assertCasesThrow((...a) => unsnappy(...a), [[u8(0x10, 1, 2, 3), null]], "unsnappy");   // a length prefix beyond the input (dynajs.d.ts)
    assertCasesThrow((...a) => lz4Decompress(...a), [[u8(0x50, 1, 2), null]], "lz4Decompress");
    assertCasesThrow((...a) => gunzip(...a), [[u8(1, 2, 3), null]], "gunzip");
    assertCasesThrow((...a) => inflate(...a), [[u8(0x07, 0x00), null]], "inflate");
}

/* ---------------- level parameters at their documented boundaries ---------------- */
{
    const text = "level boundary payload level boundary payload";
    // d.ts: zstd "level 1..22"; brotli "level 0..11" (0 is in range); deflate "level 1..12"
    assertCases((lvl) => eqArr2(inflate(deflate(text, { level: lvl }), { asString: true }), text), [
        [1, true], [6, true], [12, true],
    ], "deflate level");
    assertCases((lvl) => new TextDecoder().decode(unbrotli(brotli(text, { level: lvl }))) === text, [
        [0, true], [5, true], [11, true],
    ], "brotli level");
    assertCases((lvl) => new TextDecoder().decode(unzstd(zstd(text, { level: lvl }))) === text, [
        [1, true], [3, true], [22, true],
    ], "zstd level");
    // d.ts on lz4: "level 1..12 (acceleration); values outside the range are clamped by the codec"
    assertCases((lvl) => new TextDecoder().decode(lz4Decompress(lz4Compress(text, { level: lvl }))) === text, [
        [1, true], [12, true], [-5, true], [999, true],
    ], "lz4 level clamping");
}

/* ---------------- lz4 raw block / dictionary / frame ---------------- */
{
    const dict = fromUtf8("the shared dictionary bytes");
    // d.ts: "`dict` seeds the match window"; decompress "dict must be the dictionary used at
    // compress time" — the same dict round trips.
    assertCases((d) => { const p = fromUtf8("dictionary payload dictionary payload"); return eqArr2(lz4Decompress(lz4Compress(p, { dict: d }), { dict: d }), p); }, [
        [dict, true],
        [undefined, true],   // the no-dict window still round trips
    ], "lz4 dict");
    // d.ts: lz4Frame "LZ4 frame format with optional content checksum"
    const frame = lz4Frame("frame payload frame payload");
    assertCases(() => [frame[0], frame[1], frame[2], frame[3]], [
        [undefined, [0x04, 0x22, 0x4D, 0x18]],   // LZ4 frame magic
    ], "lz4Frame magic");
    assertCases((opts) => new TextDecoder().decode(lz4Unframe(lz4Frame("frame payload frame payload", opts))) === "frame payload frame payload", [
        // (decode before comparing: eqArr2 over a Uint8Array vs a string can never hold)
        [undefined, true],
        [{ checksum: true }, true],
        [{ checksum: false }, true],
    ], "lz4Frame round trip");
    assertCases((opts) => lz4Frame("frame payload frame payload", opts).length, [
        [{ checksum: true }, (len) => len > lz4Frame("frame payload frame payload", { checksum: false }).length],   // the checksum adds bytes
    ], "lz4Frame checksum size");
    // d.ts: lz4Unframe "a bad checksum or structure is refused"
    assertCases((flipLast) => { const f = Array.from(lz4Frame("frame payload frame payload", { checksum: true })); if (flipLast) f[f.length - 1] ^= 0xFF; try { lz4Unframe(new Uint8Array(f)); return "decoded"; } catch (e) { return "refused"; } }, [
        [false, "decoded"],
        [true, "refused"],
    ], "lz4Unframe corruption");
}

/* ---------------- gzip framing (RFC 1952) ---------------- */
{
    const input = "hello hello hello";
    const gz = gzip(input);
    // dynajs.d.ts: "RFC 1952 framing (magic `1f 8b`, mtime 0) around a real DEFLATE stream"
    assertCases((i) => [gz[i], gz[i + 1], gz[i + 2], gz[i + 3], gz[i + 4], gz[i + 5], gz[i + 6], gz[i + 7]], [
        [0, [0x1F, 0x8B, 8, 0, 0, 0, 0, 0]],   // magic, CM=8 deflate, FLG=0, mtime 0
    ], "gzip header");
    // d.ts: "gzip with the level in an options object (same encoder as the positional form)"
    assertCases((lvl) => J(Array.from(gzip(input, { level: lvl }))), [
        [1, J(Array.from(gzip(input, 1)))],
        [6, J(Array.from(gzip(input, 6)))],
        [9, J(Array.from(gzip(input, 9)))],
    ], "gzip arity parity");
    assertCasesThrow((...a) => gzip(...a), [
        [input, "9", TypeError],               // a non-number level throws TypeError either way (dynajs.d.ts)
        [input, { level: "9" }, TypeError],
    ], "gzip level type");
    // d.ts: "below 6 fixed-Huffman, 6+ dynamic-Huffman" — the level THRESHOLD policy
    // (level < 6 always uses the fixed-Huffman encoder; 6+ prefers dynamic and only
    // falls back to fixed when the dynamic tree costs more than it saves, as any
    // RFC 1951 encoder may). Observing BTYPE needs a payload where dynamic wins.
    const huffText = ("the quick brown fox jumps over the lazy dog. ").repeat(120);
    const gz1 = gzip(input, 1);
    const gz6 = gzip(huffText, 6);
    assertCases((which) => (which === 1 ? gz1 : gz6)[10] & 0x06, [
        // BTYPE bits of the first DEFLATE block (byte 10; 01 fixed, 10 dynamic)
        [1, 0x02],
        [6, 0x04],
    ], "gzip Huffman strategy");
    assertCases((lvl) => gunzip(gzip(input, lvl), { asString: true }), [
        [1, input], [6, input], [9, input],
    ], "gzip level round trip");
    // RFC 1952 trailer: CRC-32 then ISIZE (LE), parametric over input length
    assertCases((text) => { const g = gzip(text); const l = g.length; return [leU32(g, l - 4), leU32(g, l - 8) === CRC32(text)]; }, [
        ["", [0, true]],
        [input, [17, true]],
        ["x".repeat(1000), [1000, true]],
    ], "gzip trailer (ISIZE + CRC-32)");
    // d.ts: gunzip "Full RFC 1951 inflate with trailer validation"
    assertCasesThrow((...a) => gunzip(...a), [
        [gz.subarray(0, gz.length - 4), null],   // truncated trailer
    ], "gunzip");
    assertCases((t) => gunzip(gzip(t), { asString: true }), [
        ["", ""],
        [input, input],
    ], "gunzip");
}

/* ---------------- compressBound ---------------- */
{
    // dynajs.d.ts formula: lz4 = n + n/255 + 16 (integer division); d.ts: "lz4 the raw-block bound"
    assertCases((nIn) => compressBound(nIn, { algo: "lz4" }), [
        [0, 16],
        [1, 17],
        [254, 270],
        [255, 272],
        [65535, 65808],
    ], "compressBound lz4");
    assertCasesThrow((nIn) => compressBound(nIn, { algo: "lz4" }), [
        [-1, RangeError],
    ], "compressBound");
    // d.ts: "lz4frame adds the frame headers and per-4 MiB-block size fields"
    assertCases((nIn) => compressBound(nIn, { algo: "lz4frame" }) > compressBound(nIn, { algo: "lz4" }), [
        [1024, true],
        [0, true],
    ], "compressBound lz4frame > lz4");
    // d.ts: "The bound always fits the codec's output on any input" — parametric over algos x payloads
    for (const algo of ["gzip", "lz4", "lz4frame", "zstd", "brotli", "snappy"]) {
        for (const [pn, p] of [["random", pseudoRandomBytes(1024)], ["repeat", rep(0x41, 2048)], ["empty", u8()]]) {
            let packed;
            switch (algo) {
                case "gzip": packed = gzip(p); break;
                case "lz4": packed = lz4Compress(p); break;
                case "lz4frame": packed = lz4Frame(p); break;
                case "zstd": packed = zstd(p); break;
                case "brotli": packed = brotli(p); break;
                case "snappy": packed = snappy(p); break;
            }
            assert(packed.length <= compressBound(p.length, { algo }), "bound fits output: " + algo + " / " + pn + " (d.ts)");
        }
    }
    assertCasesThrow(() => compressBound(10, { algo: "bogus" }), [
        [undefined, null],
    ], "compressBound algo");
}

/* ---------------- Tar: ustar pack / list / extract ---------------- */
{
    const fileData = fromUtf8("hello tar");
    // d.ts: type "Absent: 'directory' iff there is no data"; "this writer emits no link metadata"
    const tar = TarPack([{ name: "a.txt", data: fileData }, { name: "dir" }]);
    assertCases((probe) => probe(), [
        [() => String.fromCharCode(tar[0], tar[1], tar[2], tar[3], tar[4]), "a.txt"],          // name is the first header field
        [() => String.fromCharCode(tar[257], tar[258], tar[259], tar[260], tar[261]), "ustar"], // dynajs.d.ts: "Returns a ustar archive"
        [() => TarList(tar).length, 2],
        [() => TarList(tar)[0].type, "file"],
        [() => TarList(tar)[0].size, fileData.length],
        [() => TarList(tar)[1].type, "directory"],          // no data -> directory (d.ts)
        [() => eqArr2(TarExtract(tar)[0].data, fileData), true],   // data added to non-directory entries (d.ts)
        [() => TarExtract(tar)[1].data, undefined],         // directory entries carry no data
        [() => Array.isArray(TarList(tar)), true],
    ], "Tar pack/list/extract");
    // dynajs.d.ts: safe names — "no `..` segments, no leading `/`, no drive letters"; sizes/times not negative
    assertCasesThrow((name) => TarPack([{ name, data: fileData }]), [
        ["../evil", null],
        ["/abs", null],
        ["C:plain", null],
    ], "TarPack safe names");
    assertCasesThrow(() => TarPack([{ name: "lnk", type: "symlink" }]), [
        [undefined, null],               // a link type is refused (d.ts)
    ], "TarPack link type");
    assertCasesThrow((mt) => TarPack([{ name: "x", data: fileData, mtime: mt }]), [
        [-1, null],                      // times must not be negative (dynajs.d.ts)
    ], "TarPack mtime");
    assertCasesThrow((b) => TarList(b), [
        [u8(1, 2, 3, 4), null],          // a malformed archive is refused (d.ts)
    ], "TarList");
}

/* ---------------- Zip: pack / list / read / readAt / extractAll ---------------- */
{
    const text = fromUtf8("zip content zip content zip content zip content");
    const rnd = pseudoRandomBytes(512);
    const zip = ZipPack([{ name: "f.txt", data: text }]);
    const z2 = ZipPack([{ name: "a.txt", data: text, mtime: 1700000000, mode: 0o600, comment: "greeting" }], { method: "store" });
    const zr = ZipPack([{ name: "r.bin", data: rnd, method: "deflate" }]);
    const zc = ZipPack([{ name: "c.txt", data: text, method: "deflate" }]);
    const longComment = ZipPack([{ name: "c.txt", data: text, comment: "c".repeat(2000) }]);
    const corrupt = Array.from(z2); corrupt[corrupt.length - 3] ^= 0xFF;

    assertCases((probe) => probe(), [
        // local file header signature
        [() => [zip[0], zip[1], zip[2], zip[3]], [0x50, 0x4B, 0x03, 0x04]],   // PK\x03\x04
        // d.ts: "Central-directory listing"; dynajs.d.ts: "The CRC of every member is written into the directory"
        [() => ZipList(zip).length, 1],
        [() => ZipList(zip)[0].name, "f.txt"],
        [() => ZipList(zip)[0].size, text.length],
        [() => ZipList(zip)[0].crc32, CRC32(text)],
        [() => ZipList(zip)[0].method, "deflate"],                            // pack-level default (dynajs.d.ts)
        [() => typeof ZipList(zip)[0].compressedSize, "number"],
        // d.ts: ZipRead "Extracts one member by exact name"
        [() => eqArr2(ZipRead(zip, "f.txt"), text), true],
        // d.ts: ZipReadAt "Entry by-index in central-directory order"
        [() => eqArr2(ZipReadAt(zip, 0).data, text), true],
        [() => ZipReadAt(zip, 0).mode, 0o644],                                // default mode 0644 (dynajs.d.ts)
        // per-entry fields (d.ts: mtime unix seconds; mode stored; comment truncated at 1024)
        [() => ZipList(z2)[0].method, "store"],                               // pack-level store honored
        [() => ZipList(z2)[0].mode, 0o600],
        [() => ZipList(z2)[0].mtime, 1700000000],
        [() => ZipList(z2)[0].comment, "greeting"],
        [() => ZipList(z2)[0].compressedSize, text.length],                   // stored: member bytes are the raw input
        // d.ts: method "deflate" is ADVISORY — silently stored when it would not shrink
        [() => ZipList(zr)[0].method, "store"],
        [() => ZipList(zc)[0].method, "deflate"],
        [() => ZipList(longComment)[0].comment.length, 1024],
        // d.ts: ZipExtractAll without a destination — "absent = no writes", records with data
        [() => ZipExtractAll(z2).length, 1],
        [() => eqArr2(ZipExtractAll(z2)[0].data, text), true],
        [() => ZipExtractAll(z2, { allowUnsafeNames: false }).length, 1],     // an object argument is always the bag (API-pinned)
    ], "Zip pack/list/read");
    assertCasesThrow((...a) => ZipRead(...a), [
        [zip, "nope.txt", null],                       // unknown member name
        [new Uint8Array(corrupt), "a.txt", null],      // CRC/structure damage refused (d.ts)
    ], "ZipRead");
    assertCasesThrow((...a) => ZipReadAt(...a), [
        [zip, 5, RangeError],                          // "out of range is a RangeError" (d.ts)
    ], "ZipReadAt");
    assertCasesThrow((...a) => ZipExtractAll(...a), [
        // d.ts: "Unknown keys throw a TypeError naming the key and the valid set."
        [z2, { dirx: 1 }, TypeError],
    ], "ZipExtractAll");
}

/* ---------------- Compressor (compiled codec) ---------------- */
{
    const input = fromUtf8("compressor payload compressor payload");
    // d.ts: "A compiled codec: configuration and scratch owned once, reused across calls";
    // CompressorAlgo = gzip | lz4 | lz4frame | zstd | brotli | snappy.
    for (const algo of ["gzip", "lz4", "lz4frame", "zstd", "brotli", "snappy"]) {
        const c = new Compressor({ algo });
        assertEq(c.algo, algo, "Compressor(" + algo + "): algo getter (d.ts)");
        assertEq(c.dictId, null, "Compressor(" + algo + "): dictId null without a dict (d.ts)");
        assert(eqArr2(c.decompress(c.compress(input)), input), "Compressor(" + algo + "): round trip");
        c.close();
        assertEq(c.closed, true, "Compressor(" + algo + "): closed after close (d.ts)");
        assertThrows(() => c.compress(input), "Compressor(" + algo + "): use after close must refuse");
    }
    // d.ts: dictId is the "CRC-32C of the dictionary" — cross-checked against dyna:hash CRC32C
    const dict = fromUtf8("dictionary-bytes");
    const c1 = new Compressor({ algo: "lz4", dict });
    assertEq(c1.dictId, CRC32C(dict), "Compressor: dictId is the CRC-32C of the dictionary (d.ts)");
    assert(eqArr2(c1.decompress(c1.compress(input)), input), "Compressor(lz4, dict): round trip");
    // dynajs.d.ts: `decompress` throws "dictionary mismatch" on a foreign record — never silent corruption
    const c2 = new Compressor({ algo: "lz4", dict: fromUtf8("a different dictionary!") });
    let msg = null;
    try { c2.decompress(c1.compress(input)); } catch (e) { msg = String(e); }
    assert(msg !== null && /dictionary mismatch/.test(msg), "Compressor: foreign-dictionary record refused ('dictionary mismatch'), got |" + msg + "|");
    c1.close(); c2.close();
    // dynajs.d.ts: dict "applies to lz4 only; refused elsewhere"
    assertCasesThrow(() => new Compressor({ algo: "gzip", dict }), [
        [undefined, null],
    ], "Compressor dict scope");
    // d.ts: Compressor implements DynResource (dispose surface)
    const d = new Compressor({ algo: "snappy" });
    assert(typeof d[Symbol.dispose] === "function", "Compressor: [Symbol.dispose] present (DynResource)");
    d.dispose();
    assertEq(d.closed, true, "Compressor: dispose closes (d.ts)");
}

/* ---------------- Dictionary (Aho-Corasick) ---------------- */
{
    // d.ts: "Aho-Corasick automaton replacing known phrases with codes"; id "a stable hash of the
    // phrase list"; size "The number of phrases".
    const d1 = new Dictionary(["hello", "world"]);
    const d2 = new Dictionary(["hello", "world"]);
    const d3 = new Dictionary(["hello", "world", "!"]);
    assertCases((dd) => dd.size, [
        [d1, 2],
        [d3, 3],
    ], "Dictionary.size");
    assertCases((dd) => dd.id === d1.id, [
        [d2, true],      // the same phrase list -> the same stable id
        [d3, false],     // a different list hashes differently
    ], "Dictionary.id");
    const msg = "hello world hello";
    assertCases((dd) => String.fromCharCode.apply(null, Array.from(dd.decompress(dd.compress(msg)))), [
        [d1, msg],
        [d2, msg],
    ], "Dictionary round trip");
    {
        let err = null;
        try { d3.decompress(d1.compress(msg)); } catch (e) { err = e; }
        assert(err !== null && /not a record from this dictionary/.test(String(err)), "Dictionary: a foreign record is refused ('not a record from this dictionary'), got |" + err + "|");
    }
    d1.close();
    assertEq(d1.closed, true, "Dictionary: closed after close (d.ts)");
}

print("bb_compress: all tests passed (" + n + " assertions)");
