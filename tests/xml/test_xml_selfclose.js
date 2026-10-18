// A self-closing element is one open and one close, in push and pull form
// (XML 1.0 section 3.1: an empty-element tag is equivalent to a start-tag
// immediately followed by an end-tag).
import { suite } from "../lib/kit.js";
import { SAXParser } from "dyna:xml";

const t = suite("xml_selfclose");
const DOC = "<a><b/><c></c><d x='1'/></a>";

function pull(doc, chunk) {
    const p = new SAXParser();
    const out = [];
    const drain = () => { let e; while ((e = p.next())) out.push(e); };
    if (chunk) for (let i = 0; i < doc.length; i += chunk) { p.write(doc.slice(i, i + chunk)); drain(); }
    else p.write(doc);
    p.end();
    drain();
    return out;
}

t.test("pull form pairs every open with a close", (a) => {
    const ev = pull(DOC).map((e) => e.event + " " + e.name);
    a.deep(ev, ["open a", "open b", "close b", "open c", "close c", "open d", "close d", "close a"]);
});

t.test("push form calls onClose for a self-closing element", (a) => {
    const ev = [];
    const p = new SAXParser({ onOpen: (n) => ev.push("open " + n), onClose: (n) => ev.push("close " + n) });
    p.write(DOC);
    p.end();
    a.deep(ev, ["open a", "open b", "close b", "open c", "close c", "open d", "close d", "close a"]);
});

t.test("an element stack stays balanced", (a) => {
    const stack = [];
    let maxDepth = 0;
    const p = new SAXParser({
        onOpen: (n) => { stack.push(n); maxDepth = Math.max(maxDepth, stack.length); },
        onClose: (n) => { a.eq(stack.pop(), n, "close matches the innermost open"); },
    });
    p.write("<r><x/><y><z/><z/></y><x/></r>");
    p.end();
    a.eq(stack.length, 0);
    a.eq(maxDepth, 3);
});

t.test("a self-closing root is open then close", (a) => {
    a.deep(pull("<only/>").map((e) => e.event), ["open", "close"]);
});

t.test("the synthetic close survives any chunk boundary", (a) => {
    const want = JSON.stringify(pull(DOC).map((e) => [e.event, e.name]));
    for (let n = 1; n <= DOC.length; n++)
        a.eq(JSON.stringify(pull(DOC, n).map((e) => [e.event, e.name])), want, "chunk size " + n);
});

t.test("the close offset points at the /> of its tag", (a) => {
    const ev = pull("<a><b/></a>");
    const close = ev.find((e) => e.event === "close" && e.name === "b");
    a.eq(close.offset, 5);
    a.eq("<a><b/></a>".slice(close.offset, close.offset + 2), "/>");
});

t.test("attributes belong to the open only", (a) => {
    const ev = pull("<a><d x='1'/></a>");
    a.deep(ev[1].attrs, { x: "1" });
    a.eq(ev[2].event, "close");
    a.eq(ev[2].attrs, undefined);
});

t.test("iterate yields the same pairs", async (a) => {
    const got = [];
    for await (const e of SAXParser.iterate([DOC])) got.push(e.event + " " + e.name);
    a.deep(got, ["open a", "open b", "close b", "open c", "close c", "open d", "close d", "close a"]);
});

t.test("a handler throwing on the open does not leak a stray close", (a) => {
    const ev = [];
    const p = new SAXParser({ onOpen: (n) => { if (n === "b") throw new Error("boom"); }, onClose: (n) => ev.push(n) });
    a.throws(() => p.write("<a><b/></a>"), "boom");
    a.deep(ev, []);
});

await t.run();
