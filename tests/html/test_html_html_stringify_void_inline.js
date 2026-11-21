// covers: integration c:src/core/htmlkit/htmlkit.h c:src/core/htmlkit/htmlkit-text.c c:src/modules/html/html-serialize.c -- the output buffer fast path and the void-element switch are header code every HTML writer shares
// HTMLStringify writes through hb_write/hb_put, now inline with a capacity
// fast path (the grow path stays out of line), and tells void elements apart
// with a switch on length and first letter instead of comparing against all
// 14 names. Modperf round 2026-11-16. Oracle: a JS serializer with the same
// rules: the name as written; void elements (area base br col embed hr img
// input link meta param source track wbr, any ASCII case) get no children and
// no close tag; attribute values are ToString'd and escape " < & > with NUL ->
// U+FFFD; text escapes < & > and NUL; script/style print only a string first child,
// raw except NUL;
// numbers print, null/undefined children print nothing. Trees are sized so the
// output crosses many buffer growth points.
import { suite } from "../lib/kit.js";
import { HTMLStringify } from "dyna:html";

const t = suite("test_html_html_stringify_void_inline");

const VOID = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"]);
const lowerA = (s) => s.replace(/[A-Z]/g, (c) => c.toLowerCase());
const escT = (s) => s.replace(/[<>&\0]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", "\0": "�" })[c]);
const escA = (s) => s.replace(/[<>&"\0]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;", "\0": "�" })[c]);
function ser(n, raw) {
    if (typeof n === "string") return raw ? n.replace(/\0/g, "�") : escT(n);
    if (typeof n === "number") return String(n);
    if (!n || typeof n !== "object" || raw) return "";
    let s = "<" + n.name;
    for (const k of Object.keys(n.attrs)) s += " " + k + '="' + escA(String(n.attrs[k])) + '"';
    s += ">";
    const ln = lowerA(n.name);
    if (VOID.has(ln)) return s;
    if (ln === "script" || ln === "style") {
        const c0 = n.children[0];
        return s + (typeof c0 === "string" ? c0.replace(/\0/g, "\uFFFD") : "") + "</" + n.name + ">";
    }
    for (const c of n.children) s += ser(c);
    return s + "</" + n.name + ">";
}

const NAMES = ["br", "BR", "Br", "hr", "img", "IMG", "input", "inputs", "input-x", "source", "sourcE", "sources", "col", "cols", "co", "wbr", "area", "areas", "base", "link", "meta", "embed", "param", "track", "tracks", "b", "i", "a", "div", "span", "script", "Style", "textarea", "x1", "hrx", "brr", "img2"];
let seed = 0x77;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
const BITS = ["a", "<", ">", "&", '"', "'", "\0", "é", "名", " ", "&amp;", "\n", "text "];
const txt = (k) => {
    let s = "";
    for (let i = 0; i < k; i++) s += BITS[rnd(BITS.length)];
    return s;
};
function gen(d) {
    const el = { name: NAMES[rnd(NAMES.length)], attrs: {}, children: [] };
    for (let i = rnd(3); i > 0; i--) el.attrs[["href", "src", "class", "data-x"][rnd(4)]] = rnd(5) ? txt(rnd(8)) : rnd(4) === 0 ? null : 42;
    if (d < 5) for (let i = rnd(6); i > 0; i--) el.children.push(rnd(3) ? gen(d + 1) : rnd(6) ? txt(rnd(12)) : rnd(2) ? 7 : null);
    return el;
}

t.test("every void name, its case variants and near misses", ({ eq }) => {
    for (const nm of NAMES) {
        const n = { name: nm, attrs: { a: "1" }, children: ["kid", { name: "i", attrs: {}, children: [] }] };
        eq(HTMLStringify(n), ser(n), nm);
    }
});

t.test("random trees agree with the JS serializer", ({ eq }) => {
    for (let r = 0; r < 400; r++) {
        const doc = [gen(0), txt(3), gen(1)];
        eq(HTMLStringify(doc[0]), ser(doc[0]), "node " + r);
        eq(HTMLStringify(doc), doc.map((x) => ser(x)).join(""), "list " + r);
    }
});

t.test("large outputs cross every buffer growth point", ({ eq }) => {
    for (const n of [10, 100, 1000, 20000]) {
        const kids = [];
        for (let i = 0; i < n; i++) kids.push(i % 3 ? { name: i % 2 ? "br" : "b", attrs: { k: "v" + i }, children: ["x" + i] } : "t&" + i);
        const root = { name: "div", attrs: {}, children: kids };
        const out = HTMLStringify(root);
        eq(out.length, ser(root).length, "length at " + n);
        eq(out === ser(root), true, "content at " + n);
    }
});

await t.run();
