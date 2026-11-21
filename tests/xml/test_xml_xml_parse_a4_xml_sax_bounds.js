// covers: dyna:xml.XMLParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_parse_a4_xml_sax_bounds");
import { XMLParse, SAXParser } from "dyna:xml";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}
function errOf(fn) {
    try { fn(); return null; } catch (e) { return e; }
}

__kitS.section("XMLParse nesting over 256 is refused", () => {
    const deep = "<a>".repeat(300) + "x" + "</a>".repeat(300);
    const e = errOf(() => XMLParse(deep));
    assert(e !== null && /nesting/.test(String(e.message)),
        "XMLParse nesting over 256 is refused (got " + e + ")");
    const ok = XMLParse("<a>".repeat(200) + "x" + "</a>".repeat(200));
    assert(ok !== undefined, "XMLParse nesting under the cap parses");
});
__kitS.end();

print("test_a4_xml_sax_bounds: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_xml_sax_bounds: " + fails + " failures");
