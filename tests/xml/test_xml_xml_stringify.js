// covers: dyna:xml.XMLStringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_stringify");
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const j = (v) => JSON.stringify(v);

__kitS.section("text is re-escaped, so the output re-parses", () => {
eq(XMLStringify(XMLParse("<a>&lt;&amp;</a>")), "<a>&lt;&amp;</a>",
   "text is re-escaped, so the output re-parses");
});

__kitS.section("<a b=\"&quot;&lt;\"/>", () => {
eq(XMLStringify(XMLParse('<a b="&quot;&lt;"/>')), '<a b="&quot;&lt;"/>',
   "and so is an attribute value");
});

__kitS.section("<a x=\"1\">t</a>", () => {
eq(XMLStringify({ name: "a", attrs: { x: "1" }, children: ["t"] }), '<a x="1">t</a>',
   "a hand-built node serializes");
});

__kitS.section("attrs and children are optional", () => {
eq(XMLStringify({ name: "a" }), "<a/>", "attrs and children are optional");
});

__kitS.section("<a> <b/> <c/> </a>", () => {
eq(XMLStringify(XMLParse("<a><b/><c/></a>"), { indent: 2 }),
   "<a>\n  <b/>\n  <c/>\n</a>", "indent puts each element on its own line");
});

__kitS.section("a node needs a string name", () => {
throws(() => XMLStringify({ name: 42 }), "a node needs a string name");
});

__kitS.section("and a non-empty one", () => {
throws(() => XMLStringify({ name: "" }), "and a non-empty one");
});

__kitS.section("that is a valid element name", () => {
throws(() => XMLStringify({ name: "1bad" }), "that is a valid element name");
});

__kitS.section("stringify needs a node", () => {
throws(() => XMLStringify(), "stringify needs a node");
});

__kitS.section("indent is bounded", () => {
throws(() => XMLStringify({ name: "a" }, { indent: 99 }), "indent is bounded");
});
__kitS.end();

if (fails) {
    print("test_xml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_xml failed");
}
print("test_xml: " + n + " assertions, 0 failures");
