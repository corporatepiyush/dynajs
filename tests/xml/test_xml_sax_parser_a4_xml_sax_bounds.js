// covers: dyna:xml.SAXParser
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_sax_parser_a4_xml_sax_bounds");
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

function pullText(src, ctor) {
    const s = ctor();
    s.write(src);
    s.end();
    let txt = "";
    for (;;) {
        const ev = s.next();
        if (!ev)
            break;
        if (ev.event === "text")
            txt += ev.text;
    }
    return txt;
}

__kitS.section("pull-mode opts bag entities=keep keeps unknown entities", () => {
    const txt = pullText("<r>&bogus;</r>", () => new SAXParser(null, { entities: "keep" }));
    assert(txt === "&bogus;", "pull-mode opts bag entities=keep keeps unknown entities (got " + JSON.stringify(txt) + ")");
});

__kitS.section("handlers bag wins: second-bag entities=keep is ignored, strict applies", () => {
    const e = errOf(() => pullText("<r>&bogus;</r>", () => new SAXParser({}, { entities: "keep" })));
    assert(e !== null && /entity/.test(String(e.message)),
        "handlers bag wins: second-bag entities=keep is ignored, strict applies (got " + e + ")");
});

__kitS.section("explicit strict refuses the unknown entity", () => {
    const e = errOf(() => pullText("<r>&bogus;</r>", () => new SAXParser(null, { entities: "strict" })));
    assert(e !== null && /entity/.test(String(e.message)),
        "explicit strict refuses the unknown entity (got " + e + ")");
});
__kitS.end();

print("test_a4_xml_sax_bounds: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_xml_sax_bounds: " + fails + " failures");
