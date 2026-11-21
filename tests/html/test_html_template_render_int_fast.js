// covers: c:src/modules/html/html-template.c
// Template.render prints an int-tagged value straight from its digits instead
// of building a string first, and reads an array section's length through an
// atom and property cache made once per render (not a fresh "length" atom per
// section). Modperf round 2026-11-16 (with the inline output buffer): 10K-item
// list 3.10 ms -> 2.70 ms. Oracle: a JS renderer for the same template
// language ({{x}} escaped, {{{x}}} and {{&x}} raw, {{#s}}..{{/s}} over arrays
// and truthy values, {{^s}} inverted, {{.}}, dotted paths over own data
// properties with the outer scopes searched until a path's first key is
// found), and String(v) for numbers: 0, -0, +-2^31 edges, doubles, NaN.
import { suite } from "../lib/kit.js";
import { Template } from "dyna:html";

const t = suite("test_html_template_render_int_fast");

const esc = (s) => s.replace(/[<>&"']/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;", "'": "&#39;" })[c]);
const ownData = (o, k) => {
    if (o === null || (typeof o !== "object" && typeof o !== "function")) return [false];
    const d = Object.getOwnPropertyDescriptor(o, k);
    return d && "value" in d ? [true, d.value] : [false];
};
function lookup(scopes, path) {
    if (path === ".") return scopes[scopes.length - 1];
    const segs = path.split(".");
    for (let d = scopes.length - 1; d >= 0; d--) {
        let cur = scopes[d], ok = true;
        for (let i = 0; i < segs.length; i++) {
            const [has, v] = ownData(cur, segs[i]);
            if (!has) {
                ok = false;
                if (i > 0) return undefined;
                break;
            }
            cur = v;
        }
        if (ok) return cur;
    }
    return undefined;
}
function parse(src) {
    const re = /\{\{(\{?)([#^\/&]?)\s*([^}]*?)\s*\}?\}\}/g;
    const root = [], stack = [root];
    let last = 0, m;
    while ((m = re.exec(src))) {
        if (m.index > last) stack[stack.length - 1].push(["text", src.slice(last, m.index)]);
        last = re.lastIndex;
        const kind = m[1] ? "raw" : m[2] === "&" ? "raw" : m[2] === "#" ? "sec" : m[2] === "^" ? "inv" : m[2] === "/" ? "end" : "val";
        if (kind === "sec" || kind === "inv") {
            const node = [kind, m[3], []];
            stack[stack.length - 1].push(node);
            stack.push(node[2]);
        } else if (kind === "end") stack.pop();
        else stack[stack.length - 1].push([kind, m[3]]);
    }
    if (last < src.length) root.push(["text", src.slice(last)]);
    return root;
}
function render(nodes, scopes, escape) {
    let out = "";
    for (const n of nodes) {
        if (n[0] === "text") out += n[1];
        else if (n[0] === "val" || n[0] === "raw") {
            const v = lookup(scopes, n[1]);
            if (v === undefined || v === null) continue;
            const s = String(v);
            out += n[0] === "val" && escape ? esc(s) : s;
        } else {
            const v = lookup(scopes, n[1]);
            const truthy = Array.isArray(v) ? v.length > 0 : !!v;
            if (n[0] === "inv") {
                if (!truthy) out += render(n[2], scopes, escape);
            } else if (truthy) {
                if (Array.isArray(v)) for (const el of v) out += render(n[2], scopes.concat([el]), escape);
                else out += render(n[2], scopes.concat([v]), escape);
            }
        }
    }
    return out;
}

const NUMS = [0, -0, 1, -1, 7, 10, -10, 99, 100, 123456789, 2147483647, -2147483647, -2147483648, 2147483648, -2147483649, 1.5, -0.25, 1e21, 1e-7, NaN, Infinity, -Infinity, 4294967295];
const TPLS = ["{{a}}|{{{a}}}|{{&a}}", "<i>{{a}}</i>{{#xs}}[{{.}}]{{/xs}}{{^xs}}none{{/xs}}", "{{#o}}{{n}}/{{o.n}}/{{a}}{{/o}}", "{{#xs}}{{#ys}}{{.}},{{/ys}};{{/xs}}", "{{o.n}}{{o.m.k}}{{z.q}}"];
let seed = 3;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
const pick = () => (rnd(4) ? NUMS[rnd(NUMS.length)] : ["s<&>'\"", true, false, null, undefined, "", "0"][rnd(7)]);

t.test("ints, int edges and doubles print as String(v)", ({ eq }) => {
    const tp = new Template("{{a}}");
    for (const v of NUMS) eq(tp.render({ a: v }), String(v), "value " + String(v) + (Object.is(v, -0) ? " (-0)" : ""));
    const arr = new Template("{{#xs}}{{.}},{{/xs}}");
    eq(arr.render({ xs: NUMS }), NUMS.map(String).join(",") + ",", "every value in one section");
});

t.test("random data agrees with the JS renderer, escaped and raw", ({ eq }) => {
    const compiled = TPLS.map((s) => [s, new Template(s), new Template(s, { escape: false }), parse(s)]);
    for (let r = 0; r < 500; r++) {
        const xs = rnd(4) === 0 ? pick() : Array.from({ length: rnd(5) }, pick);
        const d = { a: pick(), xs, o: rnd(5) ? { n: pick(), m: { k: pick() } } : pick(), n: pick(), ys: Array.from({ length: rnd(4) }, pick) };
        for (const [src, te, tr, ast] of compiled) {
            eq(te.render(d), render(ast, [d], true), "escaped " + r + " " + src);
            eq(tr.render(d), render(ast, [d], false), "raw " + r + " " + src);
        }
    }
});

t.test("array sections of many shapes and sizes", ({ eq }) => {
    const tp = new Template("{{#xs}}<{{i}}:{{#t}}{{.}}{{/t}}{{^t}}-{{/t}}>{{/xs}}");
    const ast = parse("{{#xs}}<{{i}}:{{#t}}{{.}}{{/t}}{{^t}}-{{/t}}>{{/xs}}");
    for (const n of [0, 1, 16, 17, 1000, 5000]) {
        const xs = [];
        for (let i = 0; i < n; i++) xs.push(i % 3 ? { i, t: [i, -i] } : { t: [], i: -i });
        eq(tp.render({ xs }), render(ast, [{ xs }], true), "n=" + n);
    }
});

await t.run();
