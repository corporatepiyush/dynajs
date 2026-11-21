// covers: c:src/modules/encoding/encoding-json5.c
// JSON5Parse takes three fast paths (1.6-2.4x on object-heavy text): a quoted string or key with no
// backslash and no newline before its closing quote becomes a string / atom straight from the source
// bytes (no scratch buffer, no string->atom round trip), and a decimal token that is an optional sign
// plus at most 15 digits is accumulated exactly instead of going through string->number conversion.
// Everything else (escapes, fractions, exponents, 16+ digits, hex, Infinity/NaN) keeps the old path.
// Each case is checked against an independent reference: JSON.parse for JSON-subset text and a small
// JS JSON5 reader for single quotes, unquoted keys, signs, leading zeros and trailing commas. Key
// order is compared with Object.keys, numbers with Object.is (so -0 counts).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_json5_fastpath_oracle");
import { JSON5Parse } from "dyna:encoding";

let pass = 0, fail = 0;
const ok = (c, m, d) => { if (c) pass++; else { fail++; print("FAIL " + m + (d ? "  [" + d + "]" : "")); } };

function same(a, b) {
    if (typeof a !== typeof b) return false;
    if (typeof a === "number") return Object.is(a, b);
    if (a === null || typeof a !== "object") return a === b;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (let i = 0; i < ka.length; i++) if (ka[i] !== kb[i] || !same(a[ka[i]], b[kb[i]])) return false;
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    return true;
}

function ref(text) {
    let i = 0;
    const ws = () => { while (i < text.length && " \t\n\r".includes(text[i])) i++; };
    const str = () => {
        const q = text[i++]; let out = "";
        while (text[i] !== q) {
            if (i >= text.length) throw new SyntaxError("unterminated");
            let c = text[i++];
            if (c === "\\") {
                c = text[i++];
                const m = { n: "\n", t: "\t", r: "\r", b: "\b", f: "\f", v: "\v", 0: "\0" }[c];
                if (m !== undefined) out += m;
                else if (c === "u") { out += String.fromCharCode(parseInt(text.slice(i, i + 4), 16)); i += 4; }
                else if (c === "x") { out += String.fromCharCode(parseInt(text.slice(i, i + 2), 16)); i += 2; }
                else out += c;
            } else out += c;
        }
        i++;
        return out;
    };
    const num = () => {
        const m = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?/.exec(text.slice(i));
        i += m[0].length;
        const s = m[0], neg = s[0] === "-";
        const body = s[0] === "+" || s[0] === "-" ? s.slice(1) : s;
        const v = Number(body);
        return neg ? -v : v;
    };
    const val = () => {
        ws();
        const c = text[i];
        if (c === "{") {
            i++; const o = {};
            for (;;) {
                ws();
                if (text[i] === "}") { i++; return o; }
                let k;
                if (text[i] === '"' || text[i] === "'") k = str();
                else { const m = /^[A-Za-z_$][A-Za-z0-9_$]*/.exec(text.slice(i)); k = m[0]; i += k.length; }
                ws(); i++;
                Object.defineProperty(o, k, { value: val(), writable: true, enumerable: true, configurable: true });
                ws();
                if (text[i] === ",") { i++; continue; }
                i++; return o;
            }
        }
        if (c === "[") {
            i++; const a = [];
            for (;;) {
                ws();
                if (text[i] === "]") { i++; return a; }
                a.push(val()); ws();
                if (text[i] === ",") { i++; continue; }
                i++; return a;
            }
        }
        if (c === '"' || c === "'") return str();
        if (text.startsWith("true", i)) { i += 4; return true; }
        if (text.startsWith("false", i)) { i += 5; return false; }
        if (text.startsWith("null", i)) { i += 4; return null; }
        return num();
    };
    const v = val(); ws();
    if (i !== text.length) throw new SyntaxError("trailing");
    return v;
}

__kitS.section("numbers", () => {
    const toks = ["0", "-0", "+0", "+5", "-5", "007", "-007", "1", "9", "10", "99", "123456789",
        "999999999999999", "-999999999999999", "100000000000000", "1000000000000000", "9999999999999999",
        "-9999999999999999", "9007199254740993", "12345678901234567", "123456789012345678901234567890",
        "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295", "4294967296",
        "1.5", "-1.5", "1e3", "1E-3", "5.", ".5", "-0.0", "0.1", "1e400", "+1e2"];
    for (const t of toks) {
        const got = JSON5Parse(t), want = ref(t);
        ok(Object.is(got, want), "number " + t, String(got) + " vs " + String(want));
        const g2 = JSON5Parse("[" + t + "]")[0];
        ok(Object.is(g2, want), "number in array " + t, String(g2));
        const g3 = JSON5Parse("{a:" + t + "}").a;
        ok(Object.is(g3, want), "number in object " + t, String(g3));
    }
    for (let d = 1; d <= 18; d++) {
        const s = "9".repeat(d), s1 = "1" + "0".repeat(d - 1);
        for (const t of [s, "-" + s, s1, "+" + s1])
            ok(Object.is(JSON5Parse(t), ref(t)), "digits " + d + " " + t, String(JSON5Parse(t)));
    }
    let seed = 7;
    const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff);
    for (let k = 0; k < 3000; k++) {
        const len = 1 + (rnd() % 17);
        let s = "";
        for (let j = 0; j < len; j++) s += String(rnd() % 10);
        const t = ["", "-", "+"][rnd() % 3] + s;
        if (!Object.is(JSON5Parse(t), ref(t))) { ok(false, "random number " + t, String(JSON5Parse(t))); break; }
    }
    pass++;
});

__kitS.section("strings and keys", () => {
    const texts = [
        '"plain"', "'single'", '""', "''", '"has \' single"', "'has \" double'", '"a\\nb"', "'a\\'b'",
        '"tab\\t"', '"\\u00e9t\\u00e9"', '"café 中文 😀"', '"esc at end\\\\"',
        '{"0":1,"42":2,"01":3,"-1":4,"4294967295":5,"4294967294":6,"":7,"__proto__":8,"x":9,"x":10,"é":11}',
        "{'0':1,'42':2,'01':3,'-1':4,'':5,'__proto__':6,'k\\u0041':7,\"q'q\":8,'d\"d':9}",
        '{"a\\"b":1,"c\\\\d":2,"e\\nf":3}',
        '[{"id":1,"name":"alpha1","tags":["a","b"]},{"id":2,"name":"beta2","tags":[]}]',
        "[{id:1,name:'alpha1',tags:['a','b',],},{id:-0,name:'',ok:true,},]",
        '{"nested":{"deeper":{"k":"v"}},"arr":[["x"],[]]}',
    ];
    for (const t of texts) {
        let got, want, ge = null, we = null;
        try { got = JSON5Parse(t); } catch (e) { ge = e; }
        try { want = ref(t); } catch (e) { we = e; }
        ok(!ge && !we && same(got, want), "text " + t, ge ? String(ge) : JSON.stringify(got) + " vs " + JSON.stringify(want));
        if (!/'|,\s*[}\]]|\{[a-z]|,[a-z]|\[-0|:-0|id:-0/.test(t)) {
            const j = JSON.parse(t);
            ok(same(got, j), "JSON.parse agrees " + t, JSON.stringify(got));
        }
    }
    const o = JSON5Parse('{"__proto__":{"polluted":1}}');
    ok(Object.getPrototypeOf(o) === Object.prototype && Object.keys(o)[0] === "__proto__" && o.polluted === undefined,
        "__proto__ key is an own data property");
    const W = ["alpha", "beta", "été", "q'q", 'd"d', "", "0", "17", "x y"];
    const objs = [];
    for (let i = 0; i < 400; i++) {
        const o2 = {};
        o2[W[i % W.length]] = W[(i * 7) % W.length] + i;
        o2["k" + (i % 13)] = i % 5 === 0 ? -i : i * 1000003;
        o2[String(i % 11)] = [W[i % 3], i % 2 === 0, null];
        objs.push(o2);
    }
    const big = JSON.stringify(objs);
    ok(same(JSON5Parse(big), JSON.parse(big)), "400 generated objects match JSON.parse");
});

__kitS.section("errors keep their text", () => {
    const bad = [['"abc', /unterminated string/], ["'abc", /unterminated string/], ['"a\nb"', /unescaped newline/],
        ["{\"a\n\":1}", /unescaped newline/], ['{"abc', /unterminated string/], ['"a\\1"', /bad escape/],
        ["{'k':1", /unterminated object|expected/], ["[1,", /unterminated array|unexpected end/]];
    for (const [t, re] of bad) {
        let msg = "no throw";
        try { JSON5Parse(t); } catch (e) { msg = String(e.message); }
        ok(re.test(msg), "error " + JSON.stringify(t), msg);
    }
});

__kitS.end();
print(`test_encoding_json5_fastpath_oracle: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
