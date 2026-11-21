// covers: dyna:xml.XmlWriter
// xml_escape scans 64 bytes, then hands the rest to a SIMD search. Every text and attribute value of length 0..300 --
// plain, and with one escapable byte at each position -- must equal a replace chain written from the
// definition. Lengths 65..127 with nothing to escape used to lose byte 64 (written as `"`): the probe ended
// without a hit and the code escaped the byte it stopped on.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_writer_escape_lengths");
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

__kitS.section("<d></d>", () => {
for (let n = 0; n <= 300; n++) {
    const s = body(n);
    const t = writeText(s),
        a = writeAttr(s);
    ok(t === `<d>${escText(s)}</d>` || (n === 0 && t === "<d></d>") || (n === 0 && t === "<d/>"), `text n=${n}: ${t.slice(0, 90)}`);
    ok(a.startsWith(`<d v="${escAttr(s)}"`), `attr n=${n}`);
    // one escapable byte at every position (all positions up to 140, then every 7th)
    for (let k = 0; k < n; k += k < 140 ? 1 : 7)
        for (const c of ["<", "&", '"', "\n"]) {
            const v = s.slice(0, k) + c + s.slice(k + 1);
            if (c !== "\n") ok(writeText(v) === `<d>${escText(v)}</d>`, `text n=${n} k=${k} c=${JSON.stringify(c)}`);
            ok(writeAttr(v).startsWith(`<d v="${escAttr(v)}"`), `attr n=${n} k=${k} c=${JSON.stringify(c)}`);
        }
}
print(`xml escape lengths: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
});
__kitS.end();
