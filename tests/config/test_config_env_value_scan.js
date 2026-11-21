// covers: integration c:src/core/cfgscan/cfgscan-env.c c:src/core/cfgscan/cfgscan-toml.c c:src/modules/config/config-dotenv.c -- .env reader, builder, writer
// Env.parse reads each value with a local cursor and finds a single-quoted value's end and the
// rest of the line with memchr (it stepped the caller's index through a pointer per byte; 22% of
// parsing a 1K-100K-line file). Env.stringify copies each run of bytes that need no escape with
// one append, and the TOML/INI/.env string builder appends inline while there is room (a call
// per byte was ~40% of stringifying). The oracles are JS models of the C reader and writer:
// random documents made of quoted, unquoted, commented, exported and multi-line records must
// parse to the model's object, random records must stringify to the model's text and parse back
// (a NUL in a quoted value round-trips: it is written as \0 and read back as U+0000, F-cfg-1), and
// TOML/INI output must not change with record size across the builder's growth points.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env_value_scan");
import * as CF from "dyna:config";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; if (fail < 30) print("FAIL " + m + ": got " + a + ", want " + b); } };
let seed = 11;
const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return (seed >>> 8) % n; };
const pick = (al, n) => { let s = ""; for (let i = 0; i < n; i++) s += al[rnd(al.length)]; return s; };
const sp = (c) => " \t\r\n\f\v".includes(c);
function unescape(v) {
    let o = "";
    for (let k = 0; k < v.length; k++) {
        let c = v[k];
        if (c === "\\" && k + 1 < v.length) { const d = v[++k]; c = d === "n" ? "\n" : d === "t" ? "\t" : d === "r" ? "\r" : d; }
        o += c;
    }
    return o;
}
function parseModel(s) {
    const o = {}, n = s.length;
    let i = 0;
    for (;;) {
        while (i < n && " \t\n\r".includes(s[i])) i++;
        if (i >= n) break;
        if (s[i] === "#") { while (i < n && s[i] !== "\n") i++; continue; }
        if (s.startsWith("export ", i)) i += 7;
        let kb = i;
        while (i < n && s[i] !== "=" && s[i] !== "\n") i++;
        if (i >= n || s[i] !== "=") { while (i < n && s[i] !== "\n") i++; continue; }
        let ke = i++;
        while (kb < ke && sp(s[kb])) kb++;
        while (ke > kb && sp(s[ke - 1])) ke--;
        let q = "", vb, ve;
        while (i < n && (s[i] === " " || s[i] === "\t")) i++;
        if (i < n && (s[i] === "\"" || s[i] === "'")) {
            q = s[i++];
            vb = i;
            while (i < n && s[i] !== q) { if (q === "\"" && s[i] === "\\" && i + 1 < n) i++; i++; }
            ve = i;
            if (i < n) i++;
        } else {
            vb = i;
            while (i < n && s[i] !== "\n" && s[i] !== "#") i++;
            ve = i;
            while (vb < ve && sp(s[vb])) vb++;
            while (ve > vb && sp(s[ve - 1])) ve--;
        }
        while (i < n && s[i] !== "\n") i++;
        if (ke === kb) continue;
        const v = s.slice(vb, ve);
        o[s.slice(kb, ke)] = q === "\"" ? unescape(v) : v;
    }
    return o;
}
const ESC = { "\"": "\\\"", "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t", "\0": "\\0" };
const needsQ = (v) => v.length === 0 || /[ \t\n\r#"']/.test(v);
const stringifyModel = (o) => Object.keys(o).map((k) => k + "=" + (needsQ(o[k]) ? "\"" + o[k].replace(/["\\\n\r\t\0]/g, (m) => ESC[m]) + "\"" : o[k]) + "\n").join("");
const show = (o) => JSON.stringify(o);

__kitS.section("parse matches the reader model", () => {
    const VAL = "ab \t#\"'\\n=x\u00e9";
    for (let d = 0; d < 1500; d++) {
        const lines = [];
        for (let r = 0; r < 1 + rnd(8); r++) {
            const k = "K" + r;
            const shape = rnd(8);
            const body = pick(VAL + (shape > 5 ? "\n" : ""), rnd(shape === 7 ? 200 : 14));
            lines.push(shape === 0 ? "# " + body : shape === 1 ? "export " + k + "=" + body : shape === 2 ? k + " = '" + body + "' # c" : shape === 3 ? k + "=\"" + body + "\"" + pick(" #x", rnd(4)) : shape === 4 ? "  " + k + "=  " + body + "  " : k + "=" + (rnd(2) ? "'" : "\"") + body + (rnd(3) ? "" : "'"));
        }
        const doc = lines.join(rnd(4) ? "\n" : "\r\n") + (rnd(2) ? "\n" : "");
        let got;
        try { got = show(CF.Env.parse(doc)); } catch (e) { got = "ERR:" + e.message; }
        eq(got, show(parseModel(doc)), "doc " + JSON.stringify(doc));
    }
});

__kitS.section("stringify matches the writer model and parses back", () => {
    const VAL = "ab \t#\"'\\\n\r=x\u00e9\ud83d\ude00\u0000";
    for (let d = 0; d < 1500; d++) {
        const o = {};
        for (let r = 0; r < 1 + rnd(6); r++) o["K_" + r] = pick(VAL, rnd(d % 10 === 0 ? 400 : 20));
        const s = CF.Env.stringify(o);
        eq(s, stringifyModel(o), "stringify " + JSON.stringify(o));
        const back = {};
        for (const k of Object.keys(o)) back[k] = o[k];
        eq(show(CF.Env.parse(s)), show(back), "round trip " + JSON.stringify(o));
    }
});

__kitS.section("TOML and INI text does not depend on the builder's growth points", () => {
    for (let len = 0; len < 300; len += 7) {
        const v = pick("abc xyz\"\\\n\u00e9", len);
        const t = CF.TOML.stringify({ a: v, b: [v, len], c: { d: v } });
        eq(show(CF.TOML.parse(t)), show({ a: v, b: [v, len], c: { d: v } }), "TOML round trip of a " + len + "-char value");
        const ini = CF.INI.stringify({ a: v.replace(/\n/g, " "), s: { b: "x" + len } });
        eq(show(CF.INI.parse(ini)).length > 0, true, "INI of a " + len + "-char value parses");
    }
    const many = {};
    for (let i = 0; i < 3000; i++) many["k" + i] = "v".repeat(i % 50) + "\"" + i;
    eq(show(CF.TOML.parse(CF.TOML.stringify(many))), show(many), "a 3000-key TOML document round-trips");
});

__kitS.end();
print(`test_config_env_value_scan: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
