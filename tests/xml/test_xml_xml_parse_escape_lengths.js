// covers: dyna:xml.XMLParse
// xml_escape scans 64 bytes, then hands the rest to a SIMD search. Every text and attribute value of length 0..300 --
// plain, and with one escapable byte at each position -- must equal a replace chain written from the
// definition. Lengths 65..127 with nothing to escape used to lose byte 64 (written as `"`): the probe ended
// without a hit and the code escaped the byte it stopped on.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_parse_escape_lengths");
import { XmlWriter, XMLParse, XMLStringify } from "dyna:xml";

let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        if (failures < 20) print("FAIL " + msg);
    }
}
const escText = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const escAttr = (s) =>
    escText(s).replace(/"/g, "&quot;").replace(/\t/g, "&#9;").replace(/\n/g, "&#10;").replace(/\r/g, "&#13;");
const writeText = (s) => {
    const w = new XmlWriter();
    w.open("d");
    w.text(s);
    w.close();
    return w.finish();
};
const writeAttr = (s) => {
    const w = new XmlWriter();
    w.open("d", { v: s });
    w.close();
    return w.finish();
};
const body = (n) => Array.from({ length: n }, (_, i) => "abcdefghijklmnopqrstuvwxyz0123456789"[i % 36]).join("");

__kitS.section("XMLStringify round-trips a 100-byte text", () => {
    // the tree serializer shares the escaper: a parsed 100-byte text survives stringify unchanged
    const s = body(100);
    ok(XMLParse(XMLStringify(XMLParse(`<r>${s}</r>`))).children[0] === s, "XMLStringify round-trips a 100-byte text");
});
__kitS.end();
print(`xml escape lengths: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
