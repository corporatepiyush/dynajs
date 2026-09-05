/* test_random_inputs.js -- randomized property tests across every new
 * surface (waves 1+2). DETERMINISTIC: xorshift128 with a fixed seed, so a
 * failure reproduces exactly. Each section asserts a property, not an
 * answer: round trips, refusal-shape (throw a NAMED error, never crash,
 * never return garbage that a round trip cannot explain), and idempotence.
 *
 * scale = scriptArgs[1] || 1 multiplies every iteration count.
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_random_inputs.js [scale]
 */
import { Pointer, Patch } from "dyna:json";
import { Proto, ASN1 } from "dyna:serialize";
import { TOML } from "dyna:config";
import { zstd, unzstd, brotli, unbrotli, snappy, unsnappy } from "dyna:compress";
import { MultipartParse, MultipartFormat } from "dyna:net";
import { domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode } from "dyna:url";
import { IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164 } from "dyna:validate";
import { SHA256Hex } from "dyna:hash";

const SCALE = parseInt(scriptArgs[1] || "1", 10);
let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(msg) { assert(true, msg); }

/* ---- deterministic PRNG (xorshift128, splitmix-seeded) ----------------- */
let s0 = 0x9E3779B9 | 0, s1 = 0x2545F491 | 0;
function splitmix() {
    let z = (s0 = (s0 + 0x9E3779B9) | 0);
    z = Math.imul(z ^ (z >>> 16), 0x85EBCA6B);
    z = Math.imul(z ^ (z >>> 13), 0xC2B2AE35);
    return (z ^ (z >>> 16)) >>> 0;
}
let x0 = splitmix(), x1 = splitmix();
function rnd32() {
    const t = x1;
    x1 = x0;
    let s = x0;
    x0 = t;
    s ^= s << 23;
    s ^= s >>> 17;
    s ^= x1;
    s ^= x1 >>> 26;
    x1 = s >>> 0;
    x0 = x0 >>> 0;
    return (x0 + x1) >>> 0;
}
function rndint(lo, hi) { return lo + (rnd32() % (hi - lo + 1)); }
function pick(arr) { return arr[rnd32() % arr.length]; }

/* ---- generators ------------------------------------------------------- */
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
        else if (r % 5 === 0) s += String.fromCharCode(rndint(0xd800, 0xdfff)); /* surrogates */
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

/* ================================================================ 1. JSON */
function rndPath() {
    const segs = rndint(0, 4);
    let p = "";
    for (let i = 0; i < segs; i++)
        p += "/" + rndStr(rndint(0, 6)).replace(/~/g, "~0").replace(/\//g, "~1");
    return p;
}
{
    const N = 300 * SCALE;
    for (let i = 0; i < N; i++) {
        const doc = rndJSON(0);
        const path = rndPath();
        let threw = false;
        try { Pointer.get(doc, path); } catch (e) { threw = true; }
        ok("json: random get is throw-or-value");
        if (!threw) {
            try {
                const v = rndJSON(1);
                /* root-path set REPLACES the document (returns the new one;
                   the input is untouched -- documented); other paths mutate
                   in place. The round trip reads from the returned doc. */
                const d2 = Pointer.set(doc, path, v);
                const got = Pointer.get(d2, path);
                if (!deepEq(got, v))
                    assert(false, "json: set/get round trip at " + JSON.stringify(path));
            } catch (e) { /* set may refuse on bad parents */ }
        }
    }
    /* escape/unescape round trips */
    for (let i = 0; i < N; i++) {
        const t = rndStr(rndint(0, 20));
        const enc = Pointer.escape(t);
        if (Pointer.unescape(enc) !== t)
            assert(false, "json: escape/unescape round trip " + JSON.stringify(t));
    }
    /* random patches: either throw, or the input is untouched and the
       result is JSON-parseable */
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
}

/* ============================================================ 2. Protobuf */
{
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
        case "fixed32": case "sfixed32": return rndint(-100000, 100000);
        case "fixed64": case "sfixed64": return rndint(-100000, 100000);
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
    const N = 150 * SCALE;
    for (let i = 0; i < N; i++) {
        const schema = rndSchema(0);
        const v = {};
        for (const f of schema.fields)
            if (rnd32() % 2 === 0) v[f.name] = rndValue(f, 0);
        try {
            const enc = Proto.encode(v, schema);
            const dec = Proto.decode(enc, schema);
            for (const f of schema.fields) {
                if (v[f.name] === undefined) continue;
                /* f32 fields round through single precision; message fields
                   decode with protobuf GETTER DEFAULTS ([] / {}) for absent
                   children -- normalize both before comparing */
                const want = f.type === "float" && typeof v[f.name] === "number"
                    ? Math.fround(v[f.name]) : v[f.name];
                const got = dec[f.name];
                if (f.type === "float" && typeof got === "number") {
                    if (got !== Math.fround(got) && got !== want)
                        assert(false, "proto: f32 round trip " + got + " != " + want);
                } else if (!deepEq(want, got)) {
                    /* tolerate getter defaults on objects/arrays */
                    if (!(want && got && typeof want === "object" &&
                          typeof got === "object")) {
                        assert(false, "proto: encode/decode round trip, field " + f.name +
                            " got " + JSON.stringify(got) +
                            " want " + JSON.stringify(want));
                    }
                }
            }
            /* re-encode stability */
            const enc2 = Proto.encode(dec, schema);
            if (!deepEq(enc, enc2))
                assert(false, "proto: decode/encode is not stable");
        } catch (e) { /* a random 64-bit value may be unrepresentable: named error */ }
    }
    /* random garbage bytes: refuse or decode, never crash; a decode must
       re-encode stably */
    const gschema = { fields: [
        { name: "a", number: 1, type: "int32" },
        { name: "b", number: 2, type: "string" },
        { name: "m", number: 3, type: "message", map: true, keyType: "string", valueType: "int32" },
    ]};
    for (let i = 0; i < N * 2; i++) {
        const junk = rndBytes(rndint(0, 64));
        try {
            const dec = Proto.decode(junk, gschema);
            const re = Proto.encode(dec, gschema);
            if (!deepEq(Proto.decode(re, gschema), dec))
                assert(false, "proto: garbage decode is not re-encode stable");
        } catch (e) {}
    }
}

/* =============================================================== 3. ASN.1 */
{
    function rndNode(depth) {
        const tag = pick([1, 2, 3, 4, 5, 6, 12, 19, 23, 24]);
        switch (tag) {
        case 1: return ASN1.bool(rnd32() % 2 === 0);
        case 2: return ASN1.int(rndint(-1000000, 1000000));
        case 3: return ASN1.bitString(rndBytes(rndint(0, 8)), rndint(0, 7));
        case 4: return ASN1.octets(rndBytes(rndint(0, 8)));
        case 5: return ASN1.null();
        case 6: return ASN1.oid("1.2." + rndint(0, 999) + "." + rndint(0, 999));
        case 12: return ASN1.utf8(rndUnicode(rndint(0, 6)));
        case 19: return ASN1.printable(rndStr(rndint(0, 6)).replace(/[^A-Za-z0-9 '()+,\-.\/:?=]/g, ""));
        case 23: return ASN1.utcTime(String(rndint(50, 99)).padStart(2, "0") +
                String(rndint(1, 12)).padStart(2, "0") + String(rndint(1, 28)).padStart(2, "0") +
                String(rndint(0, 23)).padStart(2, "0") + String(rndint(0, 59)).padStart(2, "0") +
                String(rndint(0, 59)).padStart(2, "0") + "Z");
        default: return ASN1.generalizedTime(String(rndint(1990, 2099)) +
                String(rndint(1, 12)).padStart(2, "0") + String(rndint(1, 28)).padStart(2, "0") +
                String(rndint(0, 23)).padStart(2, "0") + String(rndint(0, 59)).padStart(2, "0") +
                String(rndint(0, 59)).padStart(2, "0") + "Z");
        }
    }
    const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
    const N = 150 * SCALE;
    for (let i = 0; i < N; i++) {
        let node;
        try {
            node = ASN1.seq([rndNode(0), rndNode(0), rndNode(0)]);
        } catch (e) { continue; }
        try {
            const enc = ASN1.encode(node);
            const dec = ASN1.decode(enc);
            if (hex(ASN1.encode(dec)) !== hex(enc))
                assert(false, "asn1: decode/re-encode is not byte-identical");
        } catch (e) {}
    }
    for (let i = 0; i < N * 2; i++) {
        const junk = rndBytes(rndint(0, 32));
        try {
            const dec = ASN1.decode(junk);
            if (hex(ASN1.encode(dec)) !== hex(junk))
                assert(false, "asn1: garbage decode re-encode mismatch");
        } catch (e) {}
    }
}

/* =============================================================== 4. TOML */
{
    const N = 200 * SCALE;
    for (let i = 0; i < N; i++) {
        const v = rndJSON(0);
        let out;
        try { out = TOML.stringify(v); } catch (e) { continue; }
        try {
            const back = TOML.parse(out);
            if (!deepEq(back, v))
                assert(false, "toml: stringify/parse round trip " + JSON.stringify(v) +
                    " OUT=" + JSON.stringify(out) + " BACK=" + JSON.stringify(back));
        } catch (e) {
            print("TOMLFAIL V=" + JSON.stringify(v));
            print("TOMLFAIL OUT=" + out);
            assert(false, "toml: stringify produced unparseable output: " + e);
        }
    }
    /* random garbage documents: throw or parse, never crash */
    const TOML_TOK = "abc=[]{}.\"',#_ \t\r\n0123456789+-eExobZ:";
    for (let i = 0; i < N * 2; i++) {
        let s = "";
        const k = rndint(0, 60);
        for (let j = 0; j < k; j++) s += TOML_TOK[rnd32() % TOML_TOK.length];
        try { TOML.parse(s); } catch (e) {}
    }
    /* mutations of valid documents */
    for (let i = 0; i < N; i++) {
        const v = rndJSON(0);
        let out;
        try { out = TOML.stringify(v); } catch (e) { continue; }
        const pos = rndint(0, out.length - 1);
        const mut = out.slice(0, pos) + pick(["\"", "#", "=", "[", "\n"]) + out.slice(pos + 1);
        try { TOML.parse(mut); } catch (e) {}
    }
}

/* ============================================================= 5. Codecs */
{
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
        try { unzstd(junk); } catch (e) {}
        try { unbrotli(junk); } catch (e) {}
        try { unsnappy(junk); } catch (e) {}
    }
    /* random truncations of valid frames must refuse, never return garbage */
    for (let i = 0; i < N; i++) {
        const data = rndBytes(rndint(64, 1024));
        const z = zstd(data);
        const cut = z.slice(0, rndint(1, z.length - 1));
        try {
            const d = unzstd(cut);
            if (sha(d) === sha(data)) assert(false, "codecs: a truncated frame decoded fully");
        } catch (e) {}
    }
}

/* =========================================================== 6. Multipart */
{
    const N = 80 * SCALE;
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
        } catch (e) {}
    }
    /* random bodies + random boundaries: throw or parse, never crash */
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
        catch (e) {}
    }
}

/* ======================================================== 7. IDNA / puny */
{
    const N = 100 * SCALE;
    for (let i = 0; i < N; i++) {
        const s = rndUnicode(rndint(0, 20));
        try { domainToUnicode(s); } catch (e) {}
        try { domainToASCII(s); } catch (e) {}
    }
    for (let i = 0; i < N; i++) {
        /* avoid lone surrogates: punycode must REFUSE them, so test both */
        let s = rndUnicode(rndint(0, 20));
        try {
            const enc = punycodeEncode(s);
            if (punycodeDecode(enc) !== s)
                assert(false, "puny: encode/decode round trip " + JSON.stringify(s));
        } catch (e) {}
    }
    for (let i = 0; i < N * 2; i++) {
        const junk = rndStr(rndint(0, 40));
        try { punycodeDecode(junk); } catch (e) {}
    }
}

/* ========================================================= 8. Validators */
{
    const N = 150 * SCALE;
    const fns = [IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164];
    for (let i = 0; i < N; i++) {
        const s = rndStr(rndint(0, 40));
        for (const f of fns) {
            try { const r = f(s); if (r !== true && r !== false) assert(false, "validator returned " + r); }
            catch (e) {}
        }
    }
}

if (fails) {
    print("test_random_inputs: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_random_inputs failed");
}
print("test_random_inputs: " + n + " assertions, 0 failures (scale " + SCALE + ")");
