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

{
    const deep = "<a>".repeat(300) + "x" + "</a>".repeat(300);
    const e = errOf(() => XMLParse(deep));
    assert(e !== null && /nesting/.test(String(e.message)),
        "XMLParse nesting over 256 is refused (got " + e + ")");
    const ok = XMLParse("<a>".repeat(200) + "x" + "</a>".repeat(200));
    assert(ok !== undefined, "XMLParse nesting under the cap parses");
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

{
    const txt = pullText("<r>&bogus;</r>", () => new SAXParser(null, { entities: "keep" }));
    assert(txt === "&bogus;", "pull-mode opts bag entities=keep keeps unknown entities (got " + JSON.stringify(txt) + ")");
}

{
    const e = errOf(() => pullText("<r>&bogus;</r>", () => new SAXParser({}, { entities: "keep" })));
    assert(e !== null && /entity/.test(String(e.message)),
        "handlers bag wins: second-bag entities=keep is ignored, strict applies (got " + e + ")");
}

{
    const e = errOf(() => pullText("<r>&bogus;</r>", () => new SAXParser(null, { entities: "strict" })));
    assert(e !== null && /entity/.test(String(e.message)),
        "explicit strict refuses the unknown entity (got " + e + ")");
}

print("test_a4_xml_sax_bounds: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_xml_sax_bounds: " + fails + " failures");
