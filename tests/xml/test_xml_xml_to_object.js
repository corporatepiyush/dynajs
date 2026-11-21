// covers: dyna:xml.XMLToObject
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_to_object");
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

__kitS.section("text-only collapses to a string", () => {
eq(j(XMLToObject(XMLParse("<a>x</a>"))), '{"a":"x"}', "text-only collapses to a string");
});

__kitS.section("children become keys", () => {
eq(j(XMLToObject(XMLParse("<a><b>1</b><c>2</c></a>"))), '{"a":{"b":"1","c":"2"}}',
   "children become keys");
});

__kitS.section("a repeated name becomes an array", () => {
eq(j(XMLToObject(XMLParse("<a><b>1</b><b>2</b></a>"))), '{"a":{"b":["1","2"]}}',
   "a repeated name becomes an array");
});

__kitS.section("and keeps growing", () => {
eq(j(XMLToObject(XMLParse("<a><b>1</b><b>2</b><b>3</b></a>"))),
   '{"a":{"b":["1","2","3"]}}', "and keeps growing");
});

__kitS.section("<a id=\"7\">x</a>", () => {
eq(j(XMLToObject(XMLParse('<a id="7">x</a>'))), '{"a":{"@id":"7","#text":"x"}}',
   "attributes are @-prefixed and text lands in #text");
});

__kitS.section("an empty element is an empty object", () => {
eq(j(XMLToObject(XMLParse("<a/>"))), '{"a":{}}', "an empty element is an empty object");
});

__kitS.section("toObject needs an element", () => {
throws(() => XMLToObject("x"), "toObject needs an element");
});
__kitS.end();

if (fails) {
    print("test_xml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_xml failed");
}
print("test_xml: " + n + " assertions, 0 failures");
