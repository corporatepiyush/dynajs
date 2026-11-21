// covers: c:src/modules/html/html-rewrite.c
// rewriteLinks walks the tree with the four property atoms (name, attrs, children, length) made once per
// call, reads attribute names through a borrowed ASCII view (a C-string copy only for non-ASCII names),
// lower-cases the tag on the stack, builds the tag string once per element and hands the attribute's own
// string to fn (no C-string round trip): 1.9x on a 10K-link document. Every case runs the native walk and
// an independent JS walk on two identical clones and compares the call log (url, tag, attr, in order),
// the returned count and the rewritten tree. Cases: mixed-case tags and names, every single-URL attribute,
// srcset candidates, numeric and non-ASCII attribute names, lone surrogates and non-ASCII values,
// non-string values, fn returning undefined / null / string / number / object, fn deleting a later
// attribute, nested arrays, text children and long tag names (heap path of the tag buffer).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_rewrite_links_oracle");
import * as H from "dyna:html";

let pass = 0, fail = 0;
const ok = (c, m, d) => { if (c) pass++; else { fail++; print("FAIL " + m + (d ? "  [" + d + "]" : "")); } };

const URLS = new Set(["href", "src", "action", "formaction", "cite", "data", "poster", "background", "ping", "xlink:href", "longdesc"]);
const lower = (s) => s.replace(/[A-Z]/g, (c) => c.toLowerCase());

function refSrcset(val, tag, attr, fn) {
    let i = 0, out = "", first = true, local = 0;
    const n = val.length, sp = (c) => c === " " || c === "\t" || c === "\n" || c === "\r";
    while (i < n) {
        while (i < n && (sp(val[i]) || val[i] === ",")) i++;
        if (i >= n) break;
        const us = i;
        while (i < n && !sp(val[i])) i++;
        const ue = i;
        let ee = ue, commaEnded = false;
        while (ee > us && val[ee - 1] === ",") { ee--; commaEnded = true; }
        let de = ue;
        if (!commaEnded) {
            let depth = 0, j = ue;
            while (j < n) {
                const ch = val[j];
                if (ch === "(") depth++;
                else if (ch === ")") { if (depth > 0) depth--; }
                else if (ch === "," && depth === 0) break;
                j++;
            }
            de = j;
            i = j < n ? j + 1 : n;
        }
        if (us >= ee) continue;
        const r = fn(val.slice(us, ee), tag, attr);
        let nu = val.slice(us, ee);
        if (r !== undefined && r !== null) { nu = String(r); local++; }
        if (!first) out += ", ";
        first = false;
        out += nu;
        if (de > ue) out += val.slice(ue, de);
    }
    return [local, out];
}

function refRewrite(doc, fn) {
    let count = 0;
    const node = (nd) => {
        if (nd === null || (typeof nd !== "object" && typeof nd !== "function")) return;
        if (Array.isArray(nd)) { const len = nd.length; for (let i = 0; i < len; i++) node(nd[i]); return; }
        if (typeof nd.name !== "string") return;
        const tag = lower(nd.name);
        const attrs = nd.attrs;
        if (attrs !== null && (typeof attrs === "object" || typeof attrs === "function")) {
            for (const k of Object.keys(attrs)) {
                const lk = lower(k);
                const isSet = lk === "srcset", isUrl = k.length < 32 && URLS.has(lk);
                if (!isSet && !isUrl) continue;
                const v = attrs[k];
                if (typeof v !== "string") continue;
                if (isSet) {
                    const [local, out] = refSrcset(v, tag, lk.slice(0, 15), fn);
                    if (local > 0) { Object.defineProperty(attrs, k, { value: out, writable: true, enumerable: true, configurable: true }); count += local; }
                    continue;
                }
                const r = fn(v, tag, lk.slice(0, 63));
                if (r === undefined || r === null) continue;
                Object.defineProperty(attrs, k, { value: String(r), writable: true, enumerable: true, configurable: true });
                count++;
            }
        }
        const kids = nd.children;
        if (Array.isArray(kids)) { const len = kids.length; for (let i = 0; i < len; i++) { const c = kids[i]; if (c !== null && typeof c === "object") node(c); } }
    };
    node(doc);
    return count;
}

function clone(v) {
    if (Array.isArray(v)) return v.map(clone);
    if (v !== null && typeof v === "object") { const o = {}; for (const k of Object.keys(v)) o[k] = clone(v[k]); return o; }
    return v;
}
function same(a, b) {
    if (Array.isArray(a)) return Array.isArray(b) && a.length === b.length && a.every((x, i) => same(x, b[i]));
    if (a !== null && typeof a === "object") {
        if (b === null || typeof b !== "object") return false;
        const ka = Object.keys(a), kb = Object.keys(b);
        return ka.length === kb.length && ka.every((k, i) => k === kb[i] && same(a[k], b[k]));
    }
    return Object.is(a, b);
}

function check(name, doc, mkFn) {
    const d1 = clone(doc), d2 = clone(doc);
    const log1 = [], log2 = [];
    const f1 = mkFn(log1, d1), f2 = mkFn(log2, d2);
    let c1, c2, e1 = null, e2 = null;
    try { c1 = H.rewriteLinks(d1, f1); } catch (e) { e1 = String(e); }
    try { c2 = refRewrite(d2, f2); } catch (e) { e2 = String(e); }
    ok(e1 === e2, name + ": same error", e1 + " vs " + e2);
    ok(c1 === c2, name + ": same count", c1 + " vs " + c2);
    ok(same(log1, log2), name + ": same calls", JSON.stringify(log1).slice(0, 300) + " vs " + JSON.stringify(log2).slice(0, 300));
    ok(same(d1, d2), name + ": same tree", JSON.stringify(d1).slice(0, 300) + " vs " + JSON.stringify(d2).slice(0, 300));
}

const ret = (u) => {
    let h = 0;
    for (let i = 0; i < u.length; i++) h = (h * 31 + u.charCodeAt(i)) >>> 0;
    switch (h % 5) {
        case 0: return undefined;
        case 1: return null;
        case 2: return u + "#r";
        case 3: return h % 1000;
        default: return { toString: () => "obj:" + u };
    }
};
const logging = (log) => (u, t, a) => { log.push([u, t, a]); return ret(u); };

__kitS.section("parsed documents", () => {
    let html = "<html><body>";
    for (let i = 0; i < 300; i++)
        html += '<p><a href="/p/' + i + '" HREF2="x">x</a><img src="i' + i + '.png" srcset="a' + i + '.png 1x, b' + i + '.png 2x"><form action="/f' + i + '"></form><BLOCKQUOTE cite="c' + i + '"></BLOCKQUOTE></p>';
    html += "</body></html>";
    check("parsed 300", H.HTMLParse(html), logging);
    check("parsed single node", H.HTMLParse('<A HREF="/x" Src="y" data="z" poster="p" background="b" ping="q" longdesc="l" formaction="fa">t</A>')[0], logging);
    check("identity fn", H.HTMLParse(html), (log) => (u, t, a) => { log.push([u, t, a]); return u; });
});

__kitS.section("hand-built trees", () => {
    const longTag = "X".repeat(70) + "Tag";
    const doc = [
        { name: "A", attrs: { HREF: "\ud800x\udc00", src: "café", 0: "zero", "éhref": "nonascii", "Xlink:Href": "xl", srcSet: "u1 1x,u2 2x,, u3", href: 5 }, children: [] },
        { name: longTag, attrs: { href: "long" }, children: ["text", { name: "b", attrs: { src: "inner" } }, null, 7] },
        [[{ name: "i", attrs: { href: "deep" }, children: [[{ name: "nested-array-child", attrs: { href: "skip" } }]] }]],
        { name: 42, attrs: { href: "nameless" } },
        { name: "q", attrs: null, children: [{ name: "s", attrs: "str", children: [{ name: "u", attrs: { cite: "c", srcset: "img(1,2).png 1x, x.png" } }] }] },
        { name: "span", attrs: { ["a".repeat(40)]: "longname", href: "" } },
    ];
    check("hand-built", doc, logging);
    check("deleting fn", doc, (log, d) => (u, t, a) => {
        log.push([u, t, a]);
        if (u === "\ud800x\udc00") delete d[0].attrs.src;
        return u + "!";
    });
    check("throwing fn", doc, (log) => (u, t, a) => { log.push([u, t, a]); if (u === "long") throw new Error("stop"); return null; });
});

__kitS.section("random trees", () => {
    let seed = 12345;
    const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
    const NAMES = ["href", "HREF", "src", "Src", "srcset", "data", "xlink:href", "class", "id", "onclick", "0", "7", "é", "cite", "ACTION", "ping", "style"];
    const VALS = ["/a", "b c", "éè", "😀", "\udc00", "", "x.png 1x, y.png 2x", "a,b", 3, true, null];
    const TAGS = ["a", "IMG", "Div", "p", "form", "x-y", "BLOCKQUOTE"];
    const mk = (d) => {
        const attrs = {};
        const na = rnd(5);
        for (let k = 0; k < na; k++) attrs[NAMES[rnd(NAMES.length)]] = VALS[rnd(VALS.length)];
        const kids = [];
        if (d < 4) { const nk = rnd(4); for (let k = 0; k < nk; k++) kids.push(rnd(4) ? mk(d + 1) : "txt"); }
        return { name: TAGS[rnd(TAGS.length)], attrs, children: kids };
    };
    for (let t = 0; t < 60; t++) check("random " + t, [mk(0), mk(0)], logging);
});

__kitS.end();
print(`test_html_rewrite_links_oracle: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
