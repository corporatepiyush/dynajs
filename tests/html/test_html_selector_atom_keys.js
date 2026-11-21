// covers: integration c:src/modules/html/html-selector.c c:src/modules/html/html-module.c -- Selector.all/first/matches and HTMLText share the per-call key atoms and narrow name compare
// Selector.all/first/matches and HTMLText create the "name", "attrs",
// "children", "length", "class" and "id" atoms (and one atom per [attr] part)
// once per call, read them through a property cache, and compare an element
// name in place when it is a narrow ASCII string instead of copying it out.
// Modperf round 2026-11-16: 5 selectors over a 10K-item page 43.1 ms ->
// 21.9 ms; HTMLText 8.3 ms -> 4.4 ms. Oracles, computed here in JS: a
// matcher for tag / .class / #id / [attr] / [attr=|^=|$=|*=|~=] /
// :first-child / :last-child / descendant / child / groups (tag and attribute
// names are ASCII-case-insensitive on the selector side; attribute keys are
// own properties; class words split on space, tab, CR, LF and FF), and a text
// walker that skips script/style/template/noscript (name cut at a NUL).
// Element shapes alternate (key order, getters, inherited attrs, wide and
// Latin-1 names below and at 0xFF) so a property cache that served a stale slot would show.
import { suite } from "../lib/kit.js";
import { Selector, HTMLText } from "dyna:html";

const t = suite("test_html_selector_atom_keys");

const lowerA = (s) => s.replace(/[A-Z]/g, (c) => c.toLowerCase());
const own = (o, k) => (o && typeof o === "object" && Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined);
const WS = /[ \t\r\n\f]+/;

function parseSel(src) {
    return src.split(",").map((g) => {
        const units = [];
        let comb = null;
        for (const tok of g.trim().split(/\s+/)) {
            if (tok === ">") { comb = ">"; continue; }
            const u = { comb: units.length ? comb || " " : null, parts: [] };
            comb = null;
            const re = /([a-zA-Z -￿][\w -￿-]*)|\*|\.([\w-]+)|#([\w-]+)|\[([\w-]+)(?:([~^$*]?=)'([^']*)')?\]|:(first-child|last-child)/gy;
            let m;
            while (re.lastIndex < tok.length && (m = re.exec(tok))) {
                if (m[1]) u.parts.push(["tag", lowerA(m[1])]);
                else if (m[0] === "*") u.parts.push(["any"]);
                else if (m[2]) u.parts.push(["class", m[2]]);
                else if (m[3]) u.parts.push(["id", m[3]]);
                else if (m[4]) u.parts.push(["attr", lowerA(m[4]), m[5] || "has", m[6]]);
                else u.parts.push([m[7]]);
            }
            units.push(u);
        }
        return units;
    });
}
function partOk(p, el, pos) {
    switch (p[0]) {
        case "any": return true;
        case "tag": return typeof el.name === "string" && lowerA(el.name) === p[1];
        case "first-child": return pos.idx === 0;
        case "last-child": return pos.last;
        case "id": { const v = own(el.attrs, "id"); return v !== undefined && String(v) === p[1]; }
        case "class": { const v = own(el.attrs, "class"); return v !== undefined && String(v).split(WS).includes(p[1]); }
        default: {
            const v = own(el.attrs, p[1]);
            if (v === undefined) return false;
            const s = String(v), w = p[3];
            if (p[2] === "has") return true;
            if (p[2] === "=") return s === w;
            if (p[2] === "^=") return w !== "" && s.startsWith(w);
            if (p[2] === "$=") return w !== "" && s.endsWith(w);
            if (p[2] === "*=") return w !== "" && s.includes(w);
            return s.split(WS).includes(w);
        }
    }
}
function chainOk(units, u, stack, d) {
    const unit = units[u];
    if (!unit.parts.every((p) => partOk(p, stack[d].el, stack[d]))) return false;
    if (u === 0) return true;
    if (unit.comb === ">") return d > 0 && chainOk(units, u - 1, stack, d - 1);
    for (let k = d - 1; k >= 0; k--) if (chainOk(units, u - 1, stack, k)) return true;
    return false;
}
function oracleAll(sel, doc) {
    const out = [];
    const walk = (kids, stack) => {
        const els = kids.filter((k) => k && typeof k === "object");
        els.forEach((el, i) => {
            const st = stack.concat([{ el, idx: i, last: i === els.length - 1 }]);
            if (sel.some((units) => chainOk(units, units.length - 1, st, st.length - 1))) out.push(el);
            if (Array.isArray(el.children)) walk(el.children, st);
        });
    };
    walk(doc, []);
    return out;
}
function oracleText(n) {
    if (typeof n === "string") return n;
    if (!n || typeof n !== "object") return "";
    if (Array.isArray(n)) return n.map(oracleText).join("");
    if (typeof n.name === "string" && ["script", "style", "template", "noscript"].includes(lowerA(n.name.split("\0")[0]))) return "";
    return Array.isArray(n.children) ? n.children.map(oracleText).join("") : "";
}

let seed = 0xabc;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
const NAMES = ["div", "DIV", "p", "Span", "a", "li", "ul", "ÿx", "éx", "Éx", "名", "p\0x", "script", "STYLE", "noscript\0", "b"];
const CLASSES = ["a", "b", "a b", " a\tb ", "a\fhot", "hot", "x-y"];
function el(d) {
    const kind = rnd(6);
    const attrs = {};
    if (rnd(2)) attrs.class = CLASSES[rnd(CLASSES.length)];
    if (rnd(4) === 0) attrs.id = "i" + rnd(3);
    if (rnd(3) === 0) attrs.href = rnd(2) ? "/x/" + rnd(9) : "http://e/" + rnd(9);
    if (rnd(5) === 0) attrs["data-k"] = rnd(2) ? "v1" : 1;
    const kids = [];
    if (d < 4) for (let i = rnd(5); i > 0; i--) kids.push(rnd(3) ? el(d + 1) : rnd(2) ? "t" + rnd(9) : "wé名");
    const name = NAMES[rnd(NAMES.length)];
    if (kind === 0) return { children: kids, attrs, name };
    if (kind === 1) return { name, get attrs() { return attrs; }, children: kids };
    if (kind === 2) return { name, attrs: Object.assign(Object.create({ id: "i1", class: "a" }), attrs), children: kids };
    if (kind === 3) return Object.assign(Object.create({ name }), { attrs, children: kids });
    return { name, attrs, children: kids };
}
const SELS = ["div", "p", "span", "ÿx", "éx", "名", "*", ".a", ".b.hot", "#i1", "[href]", "[href^='/x']", "[href$='3']", "[href*='e/']", "[class~='b']", "[data-k='v1']", "[DATA-K='1']", "[href][class]", "li:first-child", "li:last-child", "ul > li:last-child", "div p", "div > span", "div, span", "div *", "span.a > b", "a[href^='/x'][data-k]"];

t.test("all() and first() agree with the JS matcher on mixed element shapes", ({ eq }) => {
    const compiled = SELS.map((s) => [s, new Selector(s), parseSel(s)]);
    for (let round = 0; round < 150; round++) {
        const doc = [el(0), el(0), rnd(2) ? "top" : el(1)];
        for (const [src, sel, ps] of compiled) {
            const want = oracleAll(ps, doc);
            const got = sel.all(doc);
            eq(got.length === want.length && got.every((g, i) => g === want[i]), true, "round " + round + " " + src + " got " + got.length + " want " + want.length);
            eq(sel.first(doc), want[0], "first " + src);
        }
    }
});

t.test("matches() on single nodes", ({ eq }) => {
    const simple = SELS.filter((s) => !/[ >,]/.test(s) && !s.includes(":"));
    const compiled = simple.map((s) => [s, new Selector(s), parseSel(s)]);
    for (let round = 0; round < 300; round++) {
        const n = el(3);
        for (const [src, sel, ps] of compiled) eq(sel.matches(n), ps[0][0].parts.every((p) => partOk(p, n, { idx: 0, last: true })), src + " round " + round);
    }
});

t.test("HTMLText skips raw-text elements by name, cut at a NUL", ({ eq }) => {
    for (let round = 0; round < 300; round++) {
        const doc = [el(0), "x", el(1), 7, null];
        eq(HTMLText(doc), oracleText(doc), "doc " + round);
        eq(HTMLText(doc[0]), oracleText(doc[0]), "node " + round);
    }
});

await t.run();
