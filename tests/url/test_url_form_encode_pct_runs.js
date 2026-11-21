// covers: integration c:src/core/dyn-pct.h c:src/core/urlrec/urlrec-search.c c:src/core/urlrec/urlrec.h -- the shared percent coder, its inline sink and URLSearchParams parsing are one path under formEncode, encodeURIComponentStrict and URLSearchParams
// The shared percent encoder and decoder hand each run of bytes that need no
// change to the sink in one call (the encoder stops only at a byte to escape,
// the decoder only at '%' or, in form mode, '+'), and URLSearchParams parsing
// decodes each name and value straight into one exact-size allocation found
// with memchr instead of a growing buffer plus a copy; the string-builder
// sink is inline in urlrec.h so those calls compile to direct appends.
// Modperf round 2026-11-16 (median of 7, 1K/10K/100K): runs -> formEncode
// -11..-15%, encodeURIComponentStrict -24..-28%, URLSearchParams toString
// -4..-12%; inline sink -> encodeURIComponentStrict another -25%, toString
// -5%; exact-size parse -> new URLSearchParams -32..-38%, sort/set/append
// round -25..-29%. Oracle: JS form-urlencoded encoder and decoder over UTF-8 bytes with
// the safe sets spelled out (form: alnum *-._~, strict: alnum *-._), '+' for
// space, %XX upper-case, and decoding that keeps '%' when two hex digits do
// not follow and maps a bad UTF-8 sequence to U+FFFD. URLSearchParams keeps
// names and values as C strings, so U+0000 ends one and a bad UTF-8 escape
// is re-encoded byte for byte by toString (pre-existing, finding F12); the
// URLSearchParams cases here therefore carry no U+0000 and round-trip only
// valid UTF-8.
import { suite } from "../lib/kit.js";
import * as U from "dyna:url";

const t = suite("test_url_form_encode_pct_runs");

const ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
const FORM_SAFE = new Set((ALNUM + "*-._~").split(""));
const STRICT_SAFE = new Set((ALNUM + "*-._").split(""));
const HEX = "0123456789ABCDEF";
const utf8 = (s) => [...new TextEncoder().encode(s)];
function enc(s, safe, plus) {
    let o = "";
    for (const b of utf8(s)) {
        const c = String.fromCharCode(b);
        if (b < 0x80 && safe.has(c)) o += c;
        else if (plus && b === 0x20) o += "+";
        else o += "%" + HEX[b >> 4] + HEX[b & 15];
    }
    return o;
}
const hexv = (c) => (c >= 48 && c <= 57 ? c - 48 : c >= 65 && c <= 70 ? c - 55 : c >= 97 && c <= 102 ? c - 87 : -1);
function dec(s) {
    const bytes = utf8(s), out = [];
    for (let i = 0; i < bytes.length; i++) {
        const b = bytes[i];
        if (b === 0x25 && i + 2 < bytes.length && hexv(bytes[i + 1]) >= 0 && hexv(bytes[i + 2]) >= 0) {
            out.push(hexv(bytes[i + 1]) * 16 + hexv(bytes[i + 2]));
            i += 2;
        } else out.push(b === 0x2b ? 0x20 : b);
    }
    return new TextDecoder().decode(new Uint8Array(out));
}
function pairs(q) {
    const r = [];
    for (const seg of q.split("&")) {
        if (seg === "") continue;
        const e = seg.indexOf("=");
        r.push(e < 0 ? [dec(seg), ""] : [dec(seg.slice(0, e)), dec(seg.slice(e + 1))]);
    }
    return r;
}

let seed = 0x2611;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) % n;
const PIECES = ["a", "Zz9", " ", "+", "%", "%2", "%41", "%zz", "%e2%9c%93", "%C3", "&", "=", "~", "*-._", "!'()", "é", "€", "😀", "\u0000", "\t", "/?#[]@", "%2B", "x".repeat(70)];
const word = () => Array.from({ length: rnd(6) }, () => PIECES[rnd(PIECES.length)]).join("");
const QPIECES = PIECES.filter((p) => p !== "\u0000");
const qword = () => Array.from({ length: rnd(6) }, () => QPIECES[rnd(QPIECES.length)]).join("");
const ALL = [];
for (let c = 0; c < 0x80; c++) ALL.push(String.fromCharCode(c));
ALL.push("é", "€", "😀", "ÿ", "߿", "ࠀ", "￿");

t.test("every single char encodes as the oracle in all three encoders", ({ eq }) => {
    for (const ch of ALL) {
        eq(U.formEncode({ k: ch }), "k=" + enc(ch, FORM_SAFE, true), "formEncode " + JSON.stringify(ch));
        eq(U.encodeURIComponentStrict(ch), enc(ch, STRICT_SAFE, false), "strict " + JSON.stringify(ch));
        if (ch !== "\u0000") eq(new URLSearchParams([["k", ch]]).toString(), "k=" + enc(ch, FORM_SAFE, true), "params " + JSON.stringify(ch));
    }
});

t.test("runs of safe bytes around escapes, at both ends and long", ({ eq }) => {
    for (let r = 0; r < 600; r++) {
        const s = word();
        eq(U.encodeURIComponentStrict(s), enc(s, STRICT_SAFE, false), "strict " + JSON.stringify(s));
        const o = { [word()]: s, z: word() };
        eq(U.formEncode(o), Object.keys(o).map((k) => enc(k, FORM_SAFE, true) + "=" + enc(o[k], FORM_SAFE, true)).join("&"), "form " + JSON.stringify(o));
    }
});

t.test("decoding keeps bad escapes and maps '+' to space", ({ eq }) => {
    for (let r = 0; r < 600; r++) {
        const q = Array.from({ length: rnd(5) }, () => (rnd(3) ? qword() + "=" + qword() : qword())).join(rnd(2) ? "&" : "&&");
        const want = pairs(q);
        eq(JSON.stringify([...new URLSearchParams(q)]), JSON.stringify(want), "params " + JSON.stringify(q));
        const fd = U.formDecode(q), wo = {};
        for (const [k, v] of want) wo[k] = v;
        eq(JSON.stringify(Object.entries(fd).sort()), JSON.stringify(Object.entries(wo).sort()), "formDecode " + JSON.stringify(q));
    }
});

t.test("edge strings: empty, only escapes, trailing %, lone '+'", ({ eq }) => {
    for (const q of ["", "&", "=", "+", "%", "%%", "%4", "a=%", "a=%4", "a=%41", "%41%42=%43", "+=+", "a=b&", "&a", "a==b", "%e2%9c%93=%e2%9c%93"]) {
        eq(JSON.stringify([...new URLSearchParams(q)]), JSON.stringify(pairs(q)), "params " + JSON.stringify(q));
        eq(new URLSearchParams(q).toString(), pairs(q).map(([k, v]) => enc(k, FORM_SAFE, true) + "=" + enc(v, FORM_SAFE, true)).join("&"), "round " + JSON.stringify(q));
    }
});

await t.run();
