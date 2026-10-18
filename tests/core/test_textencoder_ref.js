// TextEncoder.encode / encodeInto against an encoder written here from the WHATWG Encoding
// spec: lone surrogates become U+FFFD, encodeInto writes whole characters only and reports
// `read` in UTF-16 code units. The corpus crosses the ASCII-run fast path (runs of 0..40,
// split at every destination size) and the non-ASCII fallback in both orders.
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function refScalars(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const d = s.charCodeAt(i + 1);
            if (d >= 0xdc00 && d <= 0xdfff) { out.push([0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00), 2]); i++; continue; }
        }
        out.push([c >= 0xd800 && c <= 0xdfff ? 0xfffd : c, 1]);
    }
    return out;
}
function utf8(cp) {
    if (cp < 0x80) return [cp];
    if (cp < 0x800) return [0xc0 | (cp >> 6), 0x80 | (cp & 63)];
    if (cp < 0x10000) return [0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63)];
    return [0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63)];
}
function refEncode(s) { return refScalars(s).flatMap(([cp]) => utf8(cp)); }
function refEncodeInto(s, cap) {
    const bytes = []; let read = 0;
    for (const [cp, units] of refScalars(s)) {
        const b = utf8(cp);
        if (bytes.length + b.length > cap) break;
        bytes.push(...b); read += units;
    }
    return { read, written: bytes.length, bytes };
}

const enc = new TextEncoder();
const parts = ["a", "~", "\u0000", "\u007f", "\u0080", "é", "߿", "ࠀ", "中", "퟿", "", "￿", "\ud800", "\udfff", "😀", "\ud83d", "\ude00", "abcdefgh", "0123456789abcdef0123456789"];
let seed = 11;
const rnd = (n) => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed % n; };
let badEnc = 0, badInto = 0, cases = 0;
for (let t = 0; t < 3000; t++) {
    let s = "";
    const L = rnd(12);
    for (let i = 0; i < L; i++) s += parts[rnd(parts.length)];
    const want = refEncode(s), got = enc.encode(s);
    if (got.length !== want.length || got.some((b, i) => b !== want[i])) badEnc++;
    for (let cap = 0; cap <= want.length + 2; cap++) {
        const dst = new Uint8Array(cap + 2).fill(0xaa);
        const r = enc.encodeInto(s, dst.subarray(1, 1 + cap)), w = refEncodeInto(s, cap);
        cases++;
        if (r.read !== w.read || r.written !== w.written || dst[0] !== 0xaa || dst[cap + 1] !== 0xaa
            || w.bytes.some((b, i) => dst[1 + i] !== b) || dst.subarray(1 + w.written, 1 + cap).some((b) => b !== 0xaa)) badInto++;
    }
}
ok(badEnc === 0, `encode matches the reference (${badEnc} of 3000 differ)`);
ok(badInto === 0, `encodeInto matches the reference at every destination size (${badInto} of ${cases} differ)`);
ok(enc.encode("").length === 0 && enc.encodeInto("abc", new Uint8Array(0)).read === 0, "empty input and empty destination");
let threw = false;
try { enc.encodeInto("x", new Uint16Array(4)); } catch (e) { threw = e instanceof TypeError; }
ok(threw, "a non-Uint8Array destination is refused");
// TextDecoder.decode against the WHATWG "UTF-8 decoder" algorithm transcribed here (replacement
// per maximal subpart, BOM stripped unless ignoreBOM, fatal throws TypeError).
function refDecode(bytes, ignoreBOM, fatal) {
    let i = 0, out = "", cp = 0, need = 0, seen = 0, lo = 0x80, hi = 0xbf;
    if (!ignoreBOM && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) i = 3;
    const bad = () => { if (fatal) throw new TypeError("bad"); out += "\ufffd"; };
    for (; i <= bytes.length; i++) {
        if (i === bytes.length) { if (need) bad(); break; }
        const b = bytes[i];
        if (need === 0) {
            if (b <= 0x7f) { out += String.fromCharCode(b); continue; }
            if (b >= 0xc2 && b <= 0xdf) { need = 1; cp = b & 0x1f; }
            else if (b >= 0xe0 && b <= 0xef) { if (b === 0xe0) lo = 0xa0; if (b === 0xed) hi = 0x9f; need = 2; cp = b & 0xf; }
            else if (b >= 0xf0 && b <= 0xf4) { if (b === 0xf0) lo = 0x90; if (b === 0xf4) hi = 0x8f; need = 3; cp = b & 7; }
            else bad();
            continue;
        }
        if (b < lo || b > hi) { cp = need = seen = 0; lo = 0x80; hi = 0xbf; bad(); i--; continue; }
        lo = 0x80; hi = 0xbf; cp = (cp << 6) | (b & 0x3f); seen++;
        if (seen === need) { out += String.fromCodePoint(cp); cp = need = seen = 0; }
    }
    return out;
}
{
    const pieces = [[0x41], [0x00], [0x7f], [0xc3, 0xa9], [0xe4, 0xb8, 0xad], [0xf0, 0x9f, 0x98, 0x80], [0xef, 0xbb, 0xbf], [0xed, 0xa0, 0x80], [0xc0, 0x80], [0xff], [0x80], [0xe4, 0xb8], [0xf4, 0x90, 0x80, 0x80], [0xc3], [0xe0, 0x9f, 0x80], [0xf0, 0x8f, 0x80, 0x80]];
    let bad = 0, n = 0;
    for (const fatal of [false, true]) for (const ignoreBOM of [false, true]) {
        const d = new TextDecoder("utf-8", { fatal, ignoreBOM });
        for (let t = 0; t < 2000; t++) {
            const bytes = [];
            const L = rnd(12);
            for (let i = 0; i < L; i++) bytes.push(...pieces[rnd(pieces.length)]);
            let want, got;
            try { want = refDecode(bytes, ignoreBOM, fatal); } catch (e) { want = "THROW"; }
            try { got = d.decode(new Uint8Array(bytes)); } catch (e) { got = e instanceof TypeError ? "THROW" : "OTHER"; }
            n++;
            if (got !== want) { bad++; if (bad < 4) print("  decode " + JSON.stringify(bytes) + " fatal=" + fatal + " ignoreBOM=" + ignoreBOM + ": " + JSON.stringify(got) + " vs " + JSON.stringify(want)); }
        }
    }
    ok(bad === 0, `decode matches the WHATWG algorithm (${bad} of ${n} differ)`);
}
print(`test_textencoder_ref: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_textencoder_ref failed");
