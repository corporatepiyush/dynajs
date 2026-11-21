// covers: integration c:src/modules/xml/xml-collapse.c c:src/modules/xml/xml-serialize.c c:src/modules/xml/xml-element.c c:src/modules/xml/xml-module.c -- XMLToObject and XMLStringify share the atom table built in xml-element.c and inited in xml-module.c
// XMLToObject and XMLStringify look up name / attrs / children / length /
// #text through atoms made once per call instead of JS_GetPropertyStr (strlen
// + atom-table lookup + free) per node. XMLToObject also gathers every
// element's text in one buffer shared down the recursion (a child appends after
// its parent's partial text and truncates back to where it started) and builds
// "@name" keys in a stack buffer. Modperf round 2026-11-15, 10K items:
// XMLStringify -36%, XMLToObject -37%. Oracle: JS implementations of the
// documented collapse (attributes as "@k", repeated names become arrays, text
// joined across interleaved children; a text-only element collapses to its
// string) and of the serializer (escape & < > in text, & < > " in attributes,
// empty element as <x/>, indent per depth before element children).
// Bug fixed in the same round: XMLToObject swallowed an exception thrown by an
// `attrs` or `children` getter and returned {} with the exception pending.
import { suite } from "../lib/kit.js";
import * as xml from "dyna:xml";

const t = suite("test_xml_xml_to_object_shared_text");

function collapse(el) {
    const out = {};
    let text = "", nattr = 0, nel = 0;
    for (const [k, v] of Object.entries(el.attrs || {})) {
        out["@" + k] = v;
        nattr++;
    }
    for (const c of el.children || []) {
        if (typeof c === "string") text += c;
        else if (c && typeof c === "object") {
            const sub = collapse(c);
            nel++;
            if (!Object.prototype.hasOwnProperty.call(out, c.name)) out[c.name] = sub;
            else if (Array.isArray(out[c.name])) out[c.name].push(sub);
            else out[c.name] = [out[c.name], sub];
        }
    }
    if (text && !nel && !nattr) return text;
    if (text) out["#text"] = text;
    return out;
}
const esc = (s, attr) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(attr ? /"/g : /(?!)/g, "&quot;");
function ser(n, ind, d) {
    if (typeof n !== "object" || n === null) return n == null ? "" : esc(String(n), false);
    let s = "<" + n.name;
    for (const [k, v] of Object.entries(n.attrs || {})) s += " " + k + '="' + esc(String(v), true) + '"';
    let body = "", any = false;
    for (const c of n.children || []) {
        if (c && typeof c === "object") {
            any = true;
            if (ind) body += "\n" + " ".repeat((d + 1) * ind);
        }
        body += ser(c, ind, d + 1);
    }
    if (!body) return s + "/>";
    return s + ">" + body + (any && ind ? "\n" + " ".repeat(d * ind) : "") + "</" + n.name + ">";
}

let seed = 99;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);
const NAMES = ["a", "b", "item", "x-y", "n.1", "é"];
function gen(d) {
    const el = { name: NAMES[rnd() % NAMES.length], attrs: {}, children: [] };
    const na = rnd() % 3;
    for (let i = 0; i < na; i++) el.attrs[["k", "id", "z" + "q".repeat(rnd() % 140)][i]] = "v<&\"" + (rnd() % 10);
    const nc = d > 5 ? 0 : rnd() % 5;
    for (let i = 0; i < nc; i++) el.children.push(rnd() % 3 === 0 ? "t" + (rnd() % 100) + (rnd() & 1 ? " & <x>" : "") : gen(d + 1));
    return el;
}

t.test("XMLToObject equals the JS collapse on random trees and on parsed documents", ({ eq }) => {
    for (let i = 0; i < 300; i++) {
        const tree = gen(0);
        eq(JSON.stringify(xml.XMLToObject(tree)), JSON.stringify({ [tree.name]: collapse(tree) }), "random tree " + i);
    }
    const doc = xml.XMLParse("<r a=\"1\">x<b>1</b>y<b k=\"v\">2<c>3</c>4</b>z<d/></r>");
    eq(JSON.stringify(xml.XMLToObject(doc)), JSON.stringify({ r: collapse(doc) }), "interleaved text");
});

t.test("XMLStringify equals the JS serializer at indents 0, 2 and 5", ({ eq }) => {
    for (let i = 0; i < 300; i++) {
        const tree = gen(0);
        for (const ind of [0, 2, 5]) eq(xml.XMLStringify(tree, { indent: ind }), ser(tree, ind, 0), "tree " + i + " indent " + ind);
    }
});

t.test("errors still come from bad names, getters and depth", ({ throws }) => {
    throws(() => xml.XMLStringify({ name: "a", attrs: { "1x": "v" }, children: [] }), undefined, "bad attr name");
    throws(() => xml.XMLStringify({ name: 7 }), undefined, "non-string name");
    throws(() => xml.XMLToObject({ name: "a", get children() { throw new Error("boom"); } }), undefined, "children getter throws");
    throws(() => xml.XMLToObject({ name: "a", get attrs() { throw new Error("boom"); } }), undefined, "attrs getter throws");
    throws(() => xml.XMLToObject({ name: "a", attrs: {}, children: [{ name: "b", get children() { throw new Error("deep"); } }] }), undefined, "nested getter throws");
    let n = { name: "x", attrs: {}, children: [] };
    for (let i = 0; i < 300; i++) n = { name: "x", attrs: {}, children: [n] };
    throws(() => xml.XMLStringify(n), undefined, "depth");
    throws(() => xml.XMLToObject(n), undefined, "depth");
});

await t.run();
