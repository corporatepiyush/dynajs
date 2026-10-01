// Parametric black-box contract test, generated from dynajs.d.ts lines 6138-6248. Engine sources not consulted.
// Covers: dyna:xml — XMLParse, XMLStringify, XMLToObject, XmlWriter, SAXParser (push, pull, iterate).
// Every expectation below is derived from the dynajs.d.ts contract text only. DOC-TENSION rows are marked inline.

import { XMLParse, XMLStringify, XMLToObject, XmlWriter, SAXParser } from "dyna:xml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

// ---- local extras ----
function canon(v) {
  if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
  if (v !== null && typeof v === "object") {
    const ks = Object.keys(v).sort();
    return "{" + ks.map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
  }
  return JSON.stringify(v);
}
function assertCanonEq(a, b, msg) { n++; if (canon(a) !== canon(b)) throw new Error("assertion failed (canon deep): " + msg + " — got " + canon(a) + " expected " + canon(b)); }
function check(got, expected, label) {
  if (typeof expected === "function") { n++; if (!expected(got)) throw new Error("assertion failed (predicate): " + label + " — got |" + got + "|"); return; }
  if (Array.isArray(expected)) { assertDeepEq(got, expected, label); return; }
  if (expected !== null && typeof expected === "object") { assertCanonEq(got, expected, label); return; }
  assertEq(got, expected, label);
}

/* ================================================================== *
 * X1: XMLParse default options [label, xml, expected tree].
 * ================================================================== */
const X1 = [
  // d.ts: default mode "demand[s] exactly one" root and returns a single XMLElement;
  // only multiple:true returns an array (the X1b row exercises that).
  ["empty element", "<r/>", { name: "r", attrs: {}, children: [] }],
  ["open-close form", "<r></r>", { name: "r", attrs: {}, children: [] }],
  ["text child", "<r>hi</r>", { name: "r", attrs: {}, children: ["hi"] }],
  ["nested element", "<a><b>x</b></a>", { name: "a", attrs: {}, children: [{ name: "b", attrs: {}, children: ["x"] }] }],
  ["attributes", '<r id="1" ok="y"/>', { name: "r", attrs: { id: "1", ok: "y" }, children: [] }],
  ["sibling elements", "<a><b/><c/></a>", { name: "a", attrs: {}, children: [{ name: "b", attrs: {}, children: [] }, { name: "c", attrs: {}, children: [] }] }],
  ["mixed text and elements", "<r>a<b/>c</r>", { name: "r", attrs: {}, children: ["a", { name: "b", attrs: {}, children: [] }, "c"] }],
  // Comments cannot be represented in the XMLElement shape (children: (string|XMLElement)[]),
  // so the tree drops them — inferred from the documented type, marked here.
  ["comment dropped from tree (inferred: type cannot hold comments)", "<r><!--c--><b/></r>", { name: "r", attrs: {}, children: [{ name: "b", attrs: {}, children: [] }] }],
  ["cdata becomes text", "<r><![CDATA[<raw>]]></r>", { name: "r", attrs: {}, children: ["<raw>"] }],
  ["xml declaration tolerated", '<?xml version="1.0"?><r/>', { name: "r", attrs: {}, children: [] }],
  ["entity amp", "<r>a &amp; b</r>", { name: "r", attrs: {}, children: ["a & b"] }],
  ["entity lt gt", "<r>&lt;b&gt;</r>", { name: "r", attrs: {}, children: ["<b>"] }],
  ["entity quot apos in attribute", '<r t="&quot;q&quot; &apos;a&apos;"/>', { name: "r", attrs: { t: '"q" \'a\'' }, children: [] }],
  ["numeric decimal reference", "<r>&#65;</r>", { name: "r", attrs: {}, children: ["A"] }],
  ["numeric hex reference", "<r>&#x41;</r>", { name: "r", attrs: {}, children: ["A"] }],
  ["trim defaults to true (doc)", "<r>  hi  </r>", { name: "r", attrs: {}, children: ["hi"] }],
];
for (const [label, xml, expected] of X1) assertCanonEq(XMLParse(xml), expected, label);

// X1b: option-variant rows [label, xml, opts, expected tree].
const X1b = [
  ["trim:false keeps whitespace (doc: trim default true)", "<r>  hi  </r>", { trim: false }, { name: "r", attrs: {}, children: ["  hi  "] }],
  ["multiple:true returns EVERY root as an array (doc)", "<a/><b/>", { multiple: true }, [{ name: "a", attrs: {}, children: [] }, { name: "b", attrs: {}, children: [] }]],
  // API.md: "keep passes an UNKNOWN `&foo;` through" — the five predefined
  // entities still decode in keep mode, so the probe uses an unknown one.
  ["entities:keep passes an unknown entity through (doc)", "<r>&nope;</r>", { entities: "keep" }, { name: "r", attrs: {}, children: ["&nope;"] }],
  ["entities:keep still decodes predefined entities", "<r>&amp;</r>", { entities: "keep" }, { name: "r", attrs: {}, children: ["&"] }],
];
for (const [label, xml, opts, expected] of X1b) assertCanonEq(XMLParse(xml, opts), expected, label);

/* ================================================================== *
 * X2: XMLParse refusal rows (doc: malformed input throws; a document with
 * no element at all throws in both modes; the options bag is strict).
 * ================================================================== */
const X2 = [
  ["two roots without multiple:true refused", () => XMLParse("<a/><b/>")],
  ["no element at all refused", () => XMLParse("just text")],
  ["no element refused even with multiple:true", () => XMLParse("just text", { multiple: true })],
  ["unclosed child element", () => XMLParse("<r><b></r>")],
  ["mismatched close tag", () => XMLParse("<r></x>")],
  ["unclosed element at EOF", () => XMLParse("<r>")],
  ["unknown entity under strict entities", () => XMLParse("<r>&nope;</r>")],
];
for (const [label, thunk] of X2) assertThrows(thunk, label);
assertThrows(() => XMLParse("<r/>", { bogus: 1 }), "strict options bag names the unknown key", TypeError, /bogus/);

/* ================================================================== *
 * X3: XMLStringify [label, thunk-or-tuple...]. indent 0..16 (doc); nesting
 * beyond 256 throws (doc); the options bag is strict.
 * ================================================================== */
const X3 = [
  ["indent 0 flat exact", () => XMLStringify({ name: "r", attrs: {}, children: ["x"] }, { indent: 0 }), "<r>x</r>"],
  ["indent 0 nested exact", () => XMLStringify({ name: "r", attrs: {}, children: [{ name: "b", attrs: {}, children: ["t"] }] }, { indent: 0 }), "<r><b>t</b></r>"],
  ["round trip compact document", () => XMLStringify(XMLParse('<r id="1"><b>t</b></r>'), { indent: 0 }), '<r id="1"><b>t</b></r>'],
  // API.md: "options.indent (number, default 0) ... default is minified" — the default is 0, not 2.
  ["default indent is 0/minified (API.md)", () => XMLStringify({ name: "r", attrs: {}, children: [{ name: "b", attrs: {}, children: ["t"] }] }), "<r><b>t</b></r>"],
  ["indent 4 spaces", () => XMLStringify({ name: "r", attrs: {}, children: [{ name: "b", attrs: {}, children: ["t"] }] }, { indent: 4 }), (s) => /\n {4}<b>/.test(s)],
  ["attributes serialized", () => XMLStringify({ name: "r", attrs: { id: "1" }, children: [] }, { indent: 0 }), (s) => s.startsWith("<r") && s.includes('id="1"') && s.endsWith(">")],
];
for (const [label, thunk, expected] of X3) check(thunk(), expected, label);

// X3b: nesting-cap boundary rows (doc: "nesting beyond 256 throws").
// deepNode(k) builds k wrappers around the leaf, so TOTAL depth is k + 1.
function deepNode(depth) { let node = { name: "leaf", attrs: {}, children: [] }; for (let i = 0; i < depth; i++) node = { name: "l", attrs: {}, children: [node] }; return node; }
assertThrows(() => XMLStringify(deepNode(256), { indent: 0 }), "nesting beyond 256 throws (doc): total depth 257");
check(typeof XMLStringify(deepNode(255), { indent: 0 }), "string", "nesting exactly 256 is fine (boundary: total depth 256)");
assertThrows(() => XMLStringify({ name: "r", attrs: {}, children: [] }, { indnt: 2 }), "strict options bag names the unknown key", TypeError, /indnt/);

/* ================================================================== *
 * X4: XMLToObject — the doc pins only "collapses an element into a plain
 * object keyed by element name"; the collapse shape is undocumented, so
 * rows are presence-only (structural).
 * ================================================================== */
const X4 = [
  // API.md: "a plain object keyed by element name" — the ROOT's name is the top key,
  // the collapse of its children lives beneath it (repeats -> array, attrs -> @attr).
  ["repeated and attribute-carrying children keyed by element name", () => { const o = XMLToObject(XMLParse('<r><b>1</b><b>2</b><c x="1"/></r>')); const inner = o.r; return typeof o === "object" && inner && Array.isArray(inner.b) && inner.c["@x"] === "1"; }, true],
  ["text-only child present", () => { const o = XMLToObject(XMLParse("<r><b>t</b></r>")); return o.r && o.r.b === "t"; }, true],
];
for (const [label, thunk, expected] of X4) check(thunk(), expected, label);

/* ================================================================== *
 * X5: XmlWriter [label, thunk, expected]. Doc pins: escaped text (& < >,
 * quotes legal raw), escaped attribute values (quotes included), toString
 * without closing, idempotent finish, innermost-first close, 65536 depth
 * guard (RangeError beyond), invalid names throw, strict options bag.
 * ================================================================== */
const X5 = [
  ["basic document exact", () => { const w = new XmlWriter({ indent: 0 }); w.open("r"); w.text("hi"); w.close(); return w.finish(); }, "<r>hi</r>"],
  ["nested attrs and text escaping exact", () => { const w = new XmlWriter({ indent: 0 }); w.open("r"); w.open("a", { id: "1", t: '"q&<x"' }); w.text("5 < 6 & 7 > 2"); w.close(); w.close(); return w.finish(); }, '<r><a id="1" t="&quot;q&amp;&lt;x&quot;">5 &lt; 6 &amp; 7 &gt; 2</a></r>'],
  ["quotes legal raw in text (doc)", () => { const w = new XmlWriter({ indent: 0 }); w.open("r"); w.text('say "hi"'); w.close(); return w.finish(); }, '<r>say "hi"</r>'],
  ["attribute value quotes escaped (doc: quotes included)", () => { const w = new XmlWriter({ indent: 0 }); w.open("a", { t: 'x"y' }); w.close(); return w.finish(); }, '<a t="x&quot;y"></a>'],
  ["toString does not close; finish closes (doc)", () => { const w = new XmlWriter({ indent: 0 }); w.open("r"); const s = w.toString(); const f = w.finish(); return s === "<r>" && f === "<r></r>"; }, true],
  ["finish is idempotent (buffer never truncates; doc)", () => { const w = new XmlWriter({ indent: 0 }); w.open("r"); const f1 = w.finish(); const f2 = w.finish(); return f1 === f2 && f1 === "<r></r>"; }, true],
  ["finish closes remaining elements innermost-first (doc)", () => { const w = new XmlWriter({ indent: 0 }); w.open("a"); w.open("b"); return w.finish(); }, "<a><b></b></a>"],
  ["indent 2 layout (structural: exact newline placement unpinned)", () => { const w = new XmlWriter({ indent: 2 }); w.open("r"); w.open("b"); w.text("t"); w.close(); w.close(); return w.finish(); }, (s) => s.includes("\n  <b>") && /<\/r>\s*$/.test(s)],
];
for (const [label, thunk, expected] of X5) check(thunk(), expected, label);

// X5b: XmlWriter refusal rows (all documented error classes).
const X5b = [
  ["close with nothing open throws (doc)", () => new XmlWriter().close()],
  ["invalid name with space throws (doc)", () => new XmlWriter().open("bad name")],
  ["invalid name with < throws (doc)", () => new XmlWriter().open("<x")],
  ["depth beyond 65536 throws RangeError (doc)", () => { const w = new XmlWriter({ indent: 0 }); for (let i = 0; i <= 65536; i++) w.open("a"); }, RangeError],
];
for (const [label, thunk, ErrType] of X5b) assertThrows(thunk, label, ErrType);
check(typeof (() => { const w = new XmlWriter({ indent: 0 }); for (let i = 0; i < 65536; i++) w.open("a"); return w.toString(); })(), "string", "depth exactly 65536 is accepted (boundary)");
assertThrows(() => new XmlWriter({ bogus: 1 }), "strict options bag names the unknown key", TypeError, /bogus/);

/* ================================================================== *
 * X6: SAXParser push form [label, thunk, expected]. A handlers object
 * (even {}) is a PUSH parser (doc); handlers fire during write()/end().
 * ================================================================== */
function pushRun(xml) {
  const out = [];
  const p = new SAXParser({
    onOpen: (name) => out.push(["open", name]),
    onClose: (name) => out.push(["close", name]),
    onText: (t) => out.push(["text", t]),
    onCData: (t) => out.push(["cdata", t]),
    onComment: (t) => out.push(["comment", t]),
    onPI: (target, data) => out.push(["pi", target, data]),
  });
  p.write(xml);
  p.end();
  return out;
}
const X6 = [
  ["push event sequence", () => pushRun("<r><b>t</b></r>"), [["open", "r"], ["open", "b"], ["text", "t"], ["close", "b"], ["close", "r"]]],
  ["push cdata event", () => pushRun("<r><![CDATA[x]]></r>"), [["open", "r"], ["cdata", "x"], ["close", "r"]]],
  ["push comment event", () => pushRun("<r><!--note--></r>"), [["open", "r"], ["comment", "note"], ["close", "r"]]],
  ["push pi event (target, data)", () => pushRun("<?tgt dta?><r/>"), [["pi", "tgt", "dta"], ["open", "r"]]],
  ["push attrs passed to onOpen", () => { let got; const p = new SAXParser({ onOpen: (name, attrs) => { got = attrs; } }); p.write('<r id="1"/>'); p.end(); return got; }, { id: "1" }],
  ["push next() is legal but has nothing to hand out (doc)", () => { const p = new SAXParser({ onOpen() {} }); p.write("<r/>"); return p.next(); }, null],
];
for (const [label, thunk, expected] of X6) check(thunk(), expected, label);

// X6b: push refusals — malformed input throws from write()/end() (doc).
assertThrows(() => { const p = new SAXParser({ onOpen() {} }); p.write("<r></q>"); }, "push malformed input throws from write()");
assertThrows(() => { const p = new SAXParser({ onOpen() {} }); p.write("<r/>x"); p.end(); }, "push trailing content throws from end() (doc)");

/* ================================================================== *
 * X7: SAXParser pull form (no handlers argument). write() only buffers;
 * next() returns one SAXEvent at a time; end() is lazy; errors are sticky.
 * Offsets are absolute starting byte offsets in the stream as fed (doc).
 * ================================================================== */
function pullRun(chunks) {
  const p = new SAXParser();
  for (const c of chunks) p.write(c);
  const out = [];
  let ev;
  while ((ev = p.next()) !== null) out.push([ev.event, ev.name !== undefined ? ev.name : (ev.text !== undefined ? ev.text : ""), ev.offset]);
  p.end();
  return out;
}
const X7 = [
  ["pull events with absolute byte offsets", () => pullRun(["<r><b>x</b></r>"]), [["open", "r", 0], ["open", "b", 3], ["text", "x", 6], ["close", "b", 7], ["close", "r", 11]]],
  ["chunk boundaries resume mid-token, offsets accumulate (doc)", () => pullRun(["<r><b", ">x</b", "></r>"]), [["open", "r", 0], ["open", "b", 3], ["text", "x", 6], ["close", "b", 7], ["close", "r", 11]]],
  // A selfclosing tag emits open only — no separate close event (parity with the
  // push rows above: "<?tgt dta?><r/>" yields pi + open, no close).
  ["byte views accepted as input (BytesInput)", () => { const p = new SAXParser(); p.write(new Uint8Array([60, 114, 47, 62])); const e1 = p.next(); const e2 = p.next(); return [e1.event, e1.name, e1.offset, e2 === null ? null : e2.event]; }, ["open", "r", 0, null]],
  ["pull next() returns null when drained (doc)", () => { const p = new SAXParser(); p.write("<r/>"); p.next(); p.next(); return p.next(); }, null],
  ["pull end() is lazy: no throw on a clean document", () => { const p = new SAXParser(); p.write("<r/>"); p.next(); p.next(); p.end(); return "ok"; }, "ok"],
  // The open event legitimately precedes the malformed close, so the error first
  // surfaces from the SECOND next(); stickiness = the failure persists (same
  // class + message), which is what "every later next() re-throws it" can pin.
  ["pull error is STICKY: every next() after the failure re-throws the same error (doc)", () => { const p = new SAXParser(); p.write("<r></q>"); p.end(); const seen = []; let e1 = null, e2 = null; for (;;) { try { const ev = p.next(); if (ev === null) { seen.push("null"); break; } seen.push(ev.event); } catch (e) { e1 = e; break; } } try { p.next(); } catch (e) { e2 = e; } return seen.length === 1 && seen[0] === "open" && e1 !== null && e2 !== null && e1.constructor === e2.constructor && e1.message === e2.message; }, true],
];
for (const [label, thunk, expected] of X7) check(thunk(), expected, label);
// The open event precedes the malformed close, so the throw surfaces from the second next().
assertThrows(() => { const p = new SAXParser(); p.write("<r></q>"); p.end(); p.next(); p.next(); }, "pull malformed input surfaces from next() (doc)");

/* ================================================================== *
 * X8: SAXParser.iterate — async generator over any sync/async iterable
 * source (top-level await is legal in this ESM file).
 * ================================================================== */
async function iterEvents(chunks) { const out = []; for await (const ev of SAXParser.iterate(chunks)) out.push(ev.event); return out; }
async function iterMalformed(chunks) { try { for await (const ev of SAXParser.iterate(chunks)) { /* drain */ } return "no-throw"; } catch (e) { return "threw"; } }
const X8 = [
  ["iterate over a sync string source", await iterEvents(["<r><b>x</b></r>"]), ["open", "open", "text", "close", "close"]],
  ["iterate resumes across chunk boundaries", await iterEvents(["<r><b", ">x</b></r>"]), ["open", "open", "text", "close", "close"]],
  ["iterate rejects on malformed input (doc)", await iterMalformed(["<r></q>"]), "threw"],
];
for (const [label, got, expected] of X8) check(got, expected, label);

print("bb_xml: all tests passed (" + n + " assertions)");
