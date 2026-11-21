// timeout: 180
// covers: dyna:xml.XMLParse
// Split from tests/test_data_w3_fixes.js: the xml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_parse_data_w3_fixes");
import { TOML } from "dyna:config";
import { XMLParse } from "dyna:xml";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}

__kitS.section("<!DOCTYPE a [<!ELEMENT a ANY>]><a/>", () => {
eq(XMLParse("<!DOCTYPE a [<!ELEMENT a ANY>]><a/>").name, "a", "a prolog DOCTYPE is still skipped");
});

await __kitS.section("<a/><!DOCTYPE a>", async () => {
await throws(() => XMLParse("<a/><!DOCTYPE a>"), "COMPAT-230: a DOCTYPE after the root is refused");
});

await __kitS.section("<!DOCTYPE a><!DOCTYPE a><a/>", async () => {
await throws(() => XMLParse("<!DOCTYPE a><!DOCTYPE a><a/>"), "COMPAT-230: a second DOCTYPE is refused");
});

__kitS.section("<?xml version=\"1.0\"?><!DOCTYPE a [<!ELEMENT a ANY>]><!-- c --><a/>", () => {
eq(XMLParse("<?xml version=\"1.0\"?><!DOCTYPE a [<!ELEMENT a ANY>]><!-- c --><a/>").name, "a",
   "decl, DOCTYPE and comment prolog order still parses");
});

__kitS.section("<!-- ok --><a/>", () => {
eq(XMLParse("<!-- ok --><a/>").name, "a", "a plain comment still parses");
});

__kitS.section("an empty comment still parses", () => {
eq(XMLParse("<!----><a/>").name, "a", "an empty comment still parses");
});

await __kitS.section("<!-- a -- b --><a/>", async () => {
await throws(() => XMLParse("<!-- a -- b --><a/>"), "COMPAT-231: -- inside a comment is refused");
});

await __kitS.section("COMPAT-231: a comment ending in - is refused", async () => {
await throws(() => XMLParse("<!--a---><a/>"), "COMPAT-231: a comment ending in - is refused");
});

__kitS.section("<a><!-- a - b --><b/></a>", () => {
eq(XMLParse("<a><!-- a - b --><b/></a>").children.length, 1, "a single - inside a comment still parses");
});

await __kitS.section("COMPAT-232: an element name over the token cap is refused", async () => {
await throws(() => XMLParse("<" + "a".repeat(17 * 1024 * 1024) + "/>"),
       "COMPAT-232: an element name over the token cap is refused");
});

await __kitS.section("COMPAT-232: an attribute name over the token cap is refused", async () => {
await throws(() => XMLParse("<a " + "b".repeat(17 * 1024 * 1024) + "='1'/>"),
       "COMPAT-232: an attribute name over the token cap is refused");
if (fails === 0) print("test_xml_data_w3_fixes: all " + n + " checks passed");
else print("test_xml_data_w3_fixes: " + fails + " FAILED of " + n);
});
__kitS.end();
