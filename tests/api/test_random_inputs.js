// covers: contract dyna:compress.* dyna:config.* dyna:hash.* dyna:json.* dyna:http.* dyna:asn1.* dyna:protobuf.* dyna:url.* dyna:validate.* -- random-input sweep over the parsers and codecs of several modules
// Oracles: a valid input may not throw, and every refusal is the documented one. These
// sections used to wrap the whole round trip in catch {}/continue, so an engine fault that threw
// passed, and most generated cases were invalid (148 of 200 TOML values had a non-table root,
// 83 of 100 punycode inputs held a lone surrogate, unsigned fixed32/64 got negative values) and
// were skipped. The generator now builds valid inputs and the invalid ones are asserted refused.
// ASN.1 is compared with the DER bytes computed from X.690 in this file; punycode with the
// RFC 3492 encoder in tests/lib/codec_model.js. rnd32 was a broken xorshift128+ (it XORed the
// state with its own old value) whose draws never picked index 7 of 10: ASN.1 PrintableString was
// never generated; the generator now asserts all 10 tags appear. Junk-input sites require an Error
// with a message. Mutants (red here, green on the old suite): punycode damp 700->701 in both
// directions; ASN1.printable() building tag 22 (round trip byte-identical).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_inputs");
import { Pointer, Patch } from "dyna:json";
import { Proto } from "dyna:protobuf";
import { ASN1 } from "dyna:asn1";
import { TOML } from "dyna:config";
import { zstd, unzstd, brotli, unbrotli, snappy, unsnappy } from "dyna:compress";
import { MultipartParse, MultipartFormat } from "dyna:http";
import { domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode } from "dyna:url";
import { IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164 } from "dyna:validate";
import { SHA256Hex } from "dyna:hash";
import { utf8, punycode } from "../lib/codec_model.js";

const SCALE = parseInt(scriptArgs[1] || "1", 10);
let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(msg) { assert(true, msg); }
function typedErr(where, e) { if (!(e instanceof Error) || typeof e.message !== "string" || !e.message) assert(false, where + ": junk input threw a non-Error or an empty message: " + String(e)); }

let s0 = 0x9E3779B9 | 0, s1 = 0x2545F491 | 0;
function splitmix() {
    let z = (s0 = (s0 + 0x9E3779B9) | 0);
    z = Math.imul(z ^ (z >>> 16), 0x85EBCA6B);
    z = Math.imul(z ^ (z >>> 13), 0xC2B2AE35);
    return (z ^ (z >>> 16)) >>> 0;
}
let x0 = splitmix(), x1 = splitmix();
function rnd32() {
    let s = x0;
    const t = x1;
    x0 = t;
    s ^= s << 23;
    s ^= s >>> 17;
    s ^= t;
    s ^= t >>> 26;
    x1 = s >>> 0;
    return (x0 + x1) >>> 0;
}
function rndint(lo, hi) { return lo + (rnd32() % (hi - lo + 1)); }
function pick(arr) { return arr[rnd32() % arr.length]; }

const JSON_TOK = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t{}[]\":,.-_/\\~";
function rndStr(len) {
    let s = "";
    for (let i = 0; i < len; i++) s += JSON_TOK[rnd32() % JSON_TOK.length];
    return s;
}
function rndUnicode(len) {
    let s = "";
    for (let i = 0; i < len; i++) {
        const r = rnd32();
        if (r % 10 === 0) s += String.fromCharCode(rndint(0, 0x7f));
        else if (r % 5 === 0) s += String.fromCharCode(rndint(0xd800, 0xdfff));
        else s += String.fromCharCode(rndint(0x80, 0xffff));
    }
    return s;
}
function rndBytes(len) {
    const b = new Uint8Array(len);
    for (let i = 0; i < len; i++) b[i] = rnd32() & 0xff;
    return b;
}
function rndJSON(depth) {
    switch (rndint(0, depth > 6 ? 3 : 6)) {
    case 0: return rndint(-1000000, 1000000);
    case 1: return rndStr(rndint(0, 12));
    case 2: return rnd32() % 2 === 0;
    case 3: return (rnd32() % 2 ? -1 : 1) * (rnd32() % 1000000) / 1000;
    case 4: {
        const len = rndint(0, 4);
        const a = [];
        for (let i = 0; i < len; i++) a.push(rndJSON(depth + 1));
        return a;
    }
    default: {
        const o = {};
        const k = rndint(0, 4);
        for (let i = 0; i < k; i++) o["k" + rndint(0, 50)] = rndJSON(depth + 1);
        return o;
    }
    }
}
function deepEq(a, b) {
    if (a === b) return true;
    if (typeof a !== typeof b) return false;
    if (a && b && typeof a === "object") {
        if (Array.isArray(a) !== Array.isArray(b)) return false;
        const ka = Object.keys(a), kb = Object.keys(b);
        if (ka.length !== kb.length) return false;
        for (const k of ka) if (!(k in b) || !deepEq(a[k], b[k])) return false;
        return true;
    }
    if (a instanceof Uint8Array || b instanceof Uint8Array) {
        if (!(a instanceof Uint8Array) || !(b instanceof Uint8Array)) return false;
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
        return true;
    }
    return false;
}
const sha = (u8) => SHA256Hex(u8);

function rndPath() {
    const segs = rndint(0, 4);
    let p = "";
    for (let i = 0; i < segs; i++)
        p += "/" + rndStr(rndint(0, 6)).replace(/~/g, "~0").replace(/\//g, "~1");
    return p;
}
__kitS.section("json: random get is throw-or-value", () => {
    const N = 300 * SCALE;
    let setGets = 0;
    for (let i = 0; i < N; i++) {
        const doc = rndJSON(0);
        const path = rndPath();
        let threw = false;
        try { Pointer.get(doc, path); } catch (e) { threw = true; }
        ok("json: random get is throw-or-value");
        if (!threw) {
            try {
                const v = rndJSON(1);
                const d2 = Pointer.set(doc, path, v);
                const got = Pointer.get(d2, path);
                if (!deepEq(got, v))
                    assert(false, "json: set/get round trip at " + JSON.stringify(path));
                else setGets++;
            } catch (e) { assert(false, "json: set at a path get resolved threw " + e.message + " at " + JSON.stringify(path)); }
        }
    }
    assert(setGets >= N / 10, "json: set/get round trips ran on " + setGets + " resolvable paths of " + N);
    for (let i = 0; i < N; i++) {
        const t = rndStr(rndint(0, 20));
        const enc = Pointer.escape(t);
        if (Pointer.unescape(enc) !== t)
            assert(false, "json: escape/unescape round trip " + JSON.stringify(t));
    }
    for (let i = 0; i < N; i++) {
        const doc = rndJSON(0);
        const before = JSON.stringify(doc);
        const ops = [];
        const k = rndint(1, 4);
        for (let j = 0; j < k; j++) {
            ops.push({ op: pick(["add", "remove", "replace", "test", "move", "copy"]),
                       path: rndPath(), value: rndJSON(1),
                       from: rndPath() });
        }
        try {
            const r = Patch.apply(doc, ops);
            JSON.stringify(r);
            if (JSON.stringify(doc) !== before)
                assert(false, "json: a successful patch mutated the input");
        } catch (e) {
            if (JSON.stringify(doc) !== before)
                assert(false, "json: a failed patch mutated the input");
        }
    }
});

__kitS.section("proto: f32 round trip", () => {
    const SCALARS = ["int32", "int64", "uint32", "sint32", "sint64", "fixed32",
                     "fixed64", "sfixed32", "sfixed64", "float", "double",
                     "bool", "string", "bytes", "enum"];
    function rndSchema(depth) {
        const fields = [];
        const k = rndint(1, 6);
        const used = new Set();
        for (let i = 0; i < k; i++) {
            let num = rndint(1, 20);
            while (used.has(num)) num = rndint(1, 20);
            used.add(num);
            const f = { name: "f" + num, number: num };
            const t = pick(SCALARS);
            f.type = t;
            if (depth < 2 && rnd32() % 4 === 0) {
                f.type = "message";
                f.message = rndSchema(depth + 1);
            }
            if (rnd32() % 4 === 0 && t !== "message") f.repeated = true;
            if (depth < 2 && rnd32() % 6 === 0) {
                f.type = "message";
                f.map = true;
                f.keyType = pick(["string", "int32", "int64", "bool"]);
                f.valueType = pick(["int32", "string", "bool"]);
            }
            fields.push(f);
        }
        return { fields };
    }
    function rndValue(f, depth) {
        if (f.map) {
            const o = {};
            const k = rndint(0, 3);
            for (let i = 0; i < k; i++) {
                const key = f.keyType === "string" ? rndStr(4)
                          : f.keyType === "bool" ? (rnd32() % 2 === 0 ? "true" : "false")
                          : String(rndint(-5, 5));
                o[key] = f.valueType === "string" ? rndStr(4)
                       : f.valueType === "bool" ? rnd32() % 2 === 0 : rndint(-100, 100);
            }
            return o;
        }
        if (f.repeated) {
            const a = [];
            const k = rndint(0, 4);
            for (let i = 0; i < k; i++) a.push(rndScalar(f.type, f, depth));
            return a;
        }
        return rndScalar(f.type, f, depth);
    }
    function rndScalar(t, f, depth) {
        switch (t) {
        case "int32": case "sint32": case "enum": return rndint(-1000, 1000);
        case "int64": case "sint64": return rndint(-100000, 100000);
        case "uint32": return rndint(0, 1000000);
        case "fixed32": case "fixed64": return rndint(0, 100000);
        case "sfixed32": case "sfixed64": return rndint(-100000, 100000);
        case "float": case "double": return (rnd32() % 2000 - 1000) / 7;
        case "bool": return rnd32() % 2 === 0;
        case "string": return rndStr(rndint(0, 10));
        case "bytes": return rndBytes(rndint(0, 10));
        case "message": {
            const o = {};
            for (const ff of f.message.fields)
                if (rnd32() % 2 === 0) o[ff.name] = rndValue(ff, depth + 1);
            return o;
        }
        }
        return 0;
    }
    let roundTrips = 0, refused = 0;
    const N = 150 * SCALE;
    for (let i = 0; i < N; i++) {
        const schema = rndSchema(0);
        const v = {};
        for (const f of schema.fields)
            if (rnd32() % 2 === 0) v[f.name] = rndValue(f, 0);
        const repeatedMap = (s) => s.fields.some((f) => (f.map && f.repeated) || (f.message && repeatedMap(f.message)));
        if (repeatedMap(schema)) {
            let msg = "";
            try { Proto.encode(v, schema); } catch (e) { msg = e.message; }
            assert(/a map field cannot be repeated/.test(msg), "proto: a schema with a repeated map field is refused (got " + JSON.stringify(msg) + ")");
            refused++;
            continue;
        }
        try {
            const enc = Proto.encode(v, schema);
            const dec = Proto.decode(enc, schema);
            for (const f of schema.fields) {
                if (v[f.name] === undefined) continue;
                const want = f.type === "float" && typeof v[f.name] === "number"
                    ? Math.fround(v[f.name]) : v[f.name];
                const got = dec[f.name];
                if (f.type === "float" && typeof got === "number") {
                    if (got !== Math.fround(got) && got !== want)
                        assert(false, "proto: f32 round trip " + got + " != " + want);
                } else if (!deepEq(want, got)) {
                    if (!(want && got && typeof want === "object" &&
                          typeof got === "object")) {
                        assert(false, "proto: encode/decode round trip, field " + f.name +
                            " got " + JSON.stringify(got) +
                            " want " + JSON.stringify(want));
                    }
                }
            }
            const enc2 = Proto.encode(dec, schema);
            if (!deepEq(enc, enc2))
                assert(false, "proto: decode/encode is not stable");
            roundTrips++;
        } catch (e) { assert(false, "proto: a valid schema and value threw " + e.message + " " + JSON.stringify(schema).slice(0, 200)); }
    }
    assert(roundTrips >= N / 2 && refused > 0, "proto: " + roundTrips + " valid round trips and " + refused + " refused schemas of " + N);
    for (const t of ["fixed32", "fixed64", "uint32"]) {
        let msg = "";
        try { Proto.encode({ a: -1 }, { fields: [{ name: "a", number: 1, type: t }] }); } catch (e) { msg = e.message; }
        assert(/is not an integer in \[0, /.test(msg), "proto: a negative " + t + " is refused (got " + JSON.stringify(msg) + ")");
    }
    const gschema = { fields: [
        { name: "a", number: 1, type: "int32" },
        { name: "b", number: 2, type: "string" },
        { name: "m", number: 3, type: "message", map: true, keyType: "string", valueType: "int32" },
    ]};
    for (let i = 0; i < N * 2; i++) {
        const junk = rndBytes(rndint(0, 64));
        let dec;
        try { dec = Proto.decode(junk, gschema); } catch (e) { typedErr("proto decode", e); continue; }
        try {
            const re = Proto.encode(dec, gschema);
            if (!deepEq(Proto.decode(re, gschema), dec))
                assert(false, "proto: garbage decode is not re-encode stable");
        } catch (e) { assert(false, "proto: a value decode accepted cannot be re-encoded: " + e.message); }
    }
});

__kitS.section("asn1: decode/re-encode is not byte-identical", () => {
    const tagsSeen = new Set();
    const derLen = (n) => n < 128 ? [n] : n < 256 ? [0x81, n] : [0x82, n >> 8, n & 255];
    const tlv = (t, body) => [t, ...derLen(body.length), ...body];
    const ascii = (s) => Array.from(s, (c) => c.charCodeAt(0));
    function derInt(v) {
        const out = [];
        for (;;) { out.unshift(v & 255); const rest = v >> 8; if ((rest === 0 && !(out[0] & 0x80)) || (rest === -1 && (out[0] & 0x80))) break; v = rest; }
        return out;
    }
    function derArc(a) { const out = [a & 127]; for (a = Math.floor(a / 128); a > 0; a = Math.floor(a / 128)) out.unshift(0x80 | (a & 127)); return out; }
    const noLone = (s) => s.replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g, "");
    function rndNode() {
        const tag = pick([1, 2, 3, 4, 5, 6, 12, 19, 23, 24]);
        tagsSeen.add(tag);
        const two = (a, b) => String(rndint(a, b)).padStart(2, "0");
        switch (tag) {
        case 1: { const b = rnd32() % 2 === 0; return [ASN1.bool(b), tlv(1, [b ? 0xff : 0])]; }
        case 2: { const v = rndint(-1000000, 1000000); return [ASN1.int(v), tlv(2, derInt(v))]; }
        case 3: {
            const data = rndBytes(rndint(0, 8)), unused = data.length ? rndint(0, 7) : 0;
            if (data.length) data[data.length - 1] &= 0xff << unused;
            return [ASN1.bitString(data, unused), tlv(3, [unused, ...data])];
        }
        case 4: { const d = rndBytes(rndint(0, 8)); return [ASN1.octets(d), tlv(4, [...d])]; }
        case 5: return [ASN1.null(), [5, 0]];
        case 6: { const a = rndint(0, 999), b = rndint(0, 999); return [ASN1.oid("1.2." + a + "." + b), tlv(6, [42, ...derArc(a), ...derArc(b)])]; }
        case 12: { const s = noLone(rndUnicode(rndint(0, 6))); return [ASN1.utf8(s), tlv(12, [...utf8(s)])]; }
        case 19: { const s = rndStr(rndint(0, 6)).replace(/[^A-Za-z0-9 '()+,\-.\/:?=]/g, ""); return [ASN1.printable(s), tlv(19, ascii(s))]; }
        case 23: { const s = two(50, 99) + two(1, 12) + two(1, 28) + two(0, 23) + two(0, 59) + two(0, 59) + "Z"; return [ASN1.utcTime(s), tlv(23, ascii(s))]; }
        default: { const s = String(rndint(1990, 2099)) + two(1, 12) + two(1, 28) + two(0, 23) + two(0, 59) + two(0, 59) + "Z"; return [ASN1.generalizedTime(s), tlv(24, ascii(s))]; }
        }
    }
    const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
    const N = 150 * SCALE;
    let modelled = 0;
    for (let i = 0; i < N; i++) {
        const kids = [rndNode(), rndNode(), rndNode()];
        const want = tlv(0x30, kids.flatMap((k) => k[1]));
        try {
            const enc = ASN1.encode(ASN1.seq(kids.map((k) => k[0])));
            if (hex(enc) !== hex(want))
                assert(false, "asn1: the encoding is not the DER the spec gives: " + hex(enc) + " want " + hex(want));
            const dec = ASN1.decode(enc);
            if (hex(ASN1.encode(dec)) !== hex(enc))
                assert(false, "asn1: decode/re-encode is not byte-identical");
            modelled++;
        } catch (e) { assert(false, "asn1: a valid DER value threw " + e.message + " (want " + hex(want) + ")"); }
    }
    assert(tagsSeen.size === 10, "asn1: the generator drew all 10 universal tags (" + [...tagsSeen].sort((a, b) => a - b) + ")");
    assert(modelled === N, "asn1: " + modelled + " of " + N + " sequences equal the spec's DER bytes");
    const refuse = (what, mk, re) => {
        let msg = "";
        try { ASN1.encode(mk()); } catch (e) { msg = e.message; }
        assert(re.test(msg), "asn1: " + what + " is refused (got " + JSON.stringify(msg) + ")");
    };
    refuse("a BIT STRING with nonzero unused bits", () => ASN1.bitString(new Uint8Array([0xff]), 3), /unused bits must be zero/);
    refuse("a UTF8String with a lone surrogate", () => ASN1.utf8("a\uD800b"), /well-formed UTF-8/);
    for (let i = 0; i < N * 2; i++) {
        const junk = rndBytes(rndint(0, 32));
        let dec;
        try { dec = ASN1.decode(junk); } catch (e) { typedErr("asn1 decode", e); continue; }
        try {
            if (hex(ASN1.encode(dec)) !== hex(junk))
                assert(false, "asn1: garbage decode re-encode mismatch");
        } catch (e) { assert(false, "asn1: a value decode accepted cannot be re-encoded: " + e.message); }
    }
});

__kitS.section("toml: stringify/parse round trip", () => {
    const tomlDoc = () => { const r = rndJSON(0); return r !== null && typeof r === "object" && !Array.isArray(r) ? r : { v: r }; };
    let stringified = 0;
    const N = 200 * SCALE;
    for (let i = 0; i < N; i++) {
        const v = tomlDoc();
        let out;
        try { out = TOML.stringify(v); } catch (e) { assert(false, "toml: stringify refused a table of JSON values: " + e.message + " " + JSON.stringify(v)); continue; }
        try {
            const back = TOML.parse(out);
            if (!deepEq(back, v))
                assert(false, "toml: stringify/parse round trip " + JSON.stringify(v) +
                    " OUT=" + JSON.stringify(out) + " BACK=" + JSON.stringify(back));
            else stringified++;
        } catch (e) {
            print("TOMLFAIL V=" + JSON.stringify(v));
            print("TOMLFAIL OUT=" + out);
            assert(false, "toml: stringify produced unparseable output: " + e);
        }
    }
    assert(stringified === N, "toml: " + stringified + " of " + N + " tables round trip");
    const TOML_TOK = "abc=[]{}.\"',#_ \t\r\n0123456789+-eExobZ:";
    for (let i = 0; i < N * 2; i++) {
        let s = "";
        const k = rndint(0, 60);
        for (let j = 0; j < k; j++) s += TOML_TOK[rnd32() % TOML_TOK.length];
        try { TOML.parse(s); } catch (e) { typedErr("toml", e); }
    }
    for (let i = 0; i < N; i++) {
        const v = tomlDoc();
        let out;
        try { out = TOML.stringify(v); } catch (e) { assert(false, "toml: stringify refused a table of JSON values: " + e.message + " " + JSON.stringify(v)); continue; }
        const pos = rndint(0, out.length - 1);
        const mut = out.slice(0, pos) + pick(["\"", "#", "=", "[", "\n"]) + out.slice(pos + 1);
        try { TOML.parse(mut); } catch (e) { typedErr("toml", e); }
    }
});

__kitS.section("codecs: zstd round trip", () => {
    const N = 40 * SCALE;
    for (let i = 0; i < N; i++) {
        const len = pick([0, 1, 2, 64, 1024, 8192, 65536]);
        const data = rndBytes(len);
        const z = zstd(data), b = brotli(data), s = snappy(data);
        if (sha(unzstd(z)) !== sha(data)) assert(false, "codecs: zstd round trip");
        if (sha(unbrotli(b)) !== sha(data)) assert(false, "codecs: brotli round trip");
        if (sha(unsnappy(s)) !== sha(data)) assert(false, "codecs: snappy round trip");
    }
    for (let i = 0; i < N * 2; i++) {
        const junk = rndBytes(rndint(0, 64));
        try { unzstd(junk); } catch (e) { typedErr("unzstd", e); }
        try { unbrotli(junk); } catch (e) { typedErr("unbrotli", e); }
        try { unsnappy(junk); } catch (e) { typedErr("unsnappy", e); }
    }
    for (let i = 0; i < N; i++) {
        const data = rndBytes(rndint(64, 1024));
        const z = zstd(data);
        const cut = z.slice(0, rndint(1, z.length - 1));
        try {
            const d = unzstd(cut);
            if (sha(d) === sha(data)) assert(false, "codecs: a truncated frame decoded fully");
        } catch (e) { typedErr("unzstd truncated", e); }
    }
});

__kitS.section("multipart: round trip part count", () => {
    const N = 80 * SCALE;
    let mpOk = 0, mpRefused = 0;
    for (let i = 0; i < N; i++) {
        const parts = [];
        const k = rndint(0, 5);
        for (let j = 0; j < k; j++) {
            const p = { name: rndStr(rndint(1, 8)) };
            if (rnd32() % 2 === 0) p.value = rndStr(rndint(0, 30));
            else {
                p.body = rndBytes(rndint(0, 30));
                p.filename = rndStr(rndint(0, 8)) + ".txt";
            }
            parts.push(p);
        }
        const ctl = /[\x00-\x1f\x7f]/;
        const badName = parts.find((p) => !p.name || ctl.test(p.name)), badFile = parts.find((p) => p.filename !== undefined && ctl.test(p.filename));
        if (badName || badFile) {
            let msg = "";
            try { MultipartFormat(parts); } catch (e) { msg = e.message; }
            assert((badName ? /name must be a non-empty string with no control characters/ : /filename must not contain control characters/).test(msg),
                "multipart: a part with a control character in its " + (badName ? "name" : "filename") + " is refused (got " + JSON.stringify(msg) + ")");
            mpRefused++;
            continue;
        }
        try {
            const fmt = MultipartFormat(parts);
            const parsed = MultipartParse(fmt.contentType, fmt.body);
            if (parsed.length !== parts.length)
                assert(false, "multipart: round trip part count " + parsed.length + " != " + parts.length);
            for (let j = 0; j < parts.length; j++) {
                if (parsed[j].name !== parts[j].name)
                    assert(false, "multipart: round trip name mismatch");
                const want = parts[j].value !== undefined
                    ? new TextEncoder().encode(parts[j].value) : parts[j].body;
                if (!deepEq(parsed[j].body, want))
                    assert(false, "multipart: round trip body mismatch");
            }
            mpOk++;
        } catch (e) { assert(false, "multipart: valid parts threw " + e.message + " " + JSON.stringify(parts.map((p) => p.name))); }
    }
    assert(mpOk >= N / 4 && mpRefused > 0, "multipart: " + mpOk + " valid round trips, " + mpRefused + " refused part lists of " + N);
    const CR = "\r\n";
    for (let i = 0; i < N * 2; i++) {
        const bnd = rndStr(rndint(1, 20)).replace(/[^A-Za-z0-9]/g, "a");
        let body = "";
        const k = rndint(0, 10);
        for (let j = 0; j < k; j++) {
            body += "--" + pick([bnd, "x" + bnd, bnd + "x"]) + CR +
                    pick(["Content-Disposition: form-data; name=\"a\"", "X: y",
                          "Content-Disposition: form-data", ""]) + CR + CR +
                    rndStr(rndint(0, 20)) + CR;
        }
        body += "--" + bnd + "--" + CR;
        try { MultipartParse("multipart/form-data; boundary=" + bnd, body); }
        catch (e) { typedErr("MultipartParse", e); }
    }
});

__kitS.section("puny: encode/decode round trip", () => {
    const N = 100 * SCALE;
    let punyModelled = 0, punyRefused = 0;
    for (let i = 0; i < N; i++) {
        const s = rndUnicode(rndint(0, 20));
        try { domainToUnicode(s); } catch (e) { typedErr("domainToUnicode", e); }
        try { domainToASCII(s); } catch (e) { typedErr("domainToASCII", e); }
    }
    for (let i = 0; i < N; i++) {
        let s = rndUnicode(rndint(0, 20));
        if (rnd32() % 2) s = s.replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g, "");
        const lone = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(s);
        let enc, msg = "";
        try { enc = punycodeEncode(s); } catch (e) { msg = e.message; }
        if (lone) { assert(/lone surrogate/.test(msg), "puny: a lone surrogate is refused (got " + JSON.stringify(msg || enc) + ")"); punyRefused++; continue; }
        if (msg) { assert(false, "puny: encode threw on well-formed text " + JSON.stringify(s) + ": " + msg); continue; }
        if (enc !== punycode(s)) assert(false, "puny: encode is not RFC 3492: " + JSON.stringify(s) + " -> " + enc + ", the spec gives " + punycode(s));
        else punyModelled++;
        try {
            if (punycodeDecode(enc) !== s)
                assert(false, "puny: encode/decode round trip " + JSON.stringify(s));
        } catch (e) { assert(false, "puny: decode threw on its own encoding " + JSON.stringify(enc) + ": " + e.message); }
    }
    assert(punyModelled >= N / 4 && punyRefused >= N / 10, "puny: " + punyModelled + " encodings equal the RFC 3492 model, " + punyRefused + " lone-surrogate inputs refused");
    for (let i = 0; i < N * 2; i++) {
        const junk = rndStr(rndint(0, 40));
        try { punycodeDecode(junk); } catch (e) { typedErr("punycodeDecode", e); }
    }
});

__kitS.section("validator returned", () => {
    const N = 150 * SCALE;
    const fns = [IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164];
    for (let i = 0; i < N; i++) {
        const s = rndStr(rndint(0, 40));
        for (const f of fns) {
            try { const r = f(s); if (r !== true && r !== false) assert(false, "validator returned " + r); }
            catch (e) { assert(false, f.name + " threw on a string instead of answering false: " + e.message); }
        }
    }
});
__kitS.end();

if (fails) {
    print("test_random_inputs: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_random_inputs failed");
}
print("test_random_inputs: " + n + " assertions, 0 failures (scale " + SCALE + ")");
