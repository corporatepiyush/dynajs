// covers: dyna:xml.XMLParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_parse");
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

__kitS.section("the root name", () => {
    const t = XMLParse("<a/>");
    eq(t.name, "a", "the root name");
    eq(j(t.attrs), "{}", "no attributes");
    eq(t.children.length, 0, "no children");
});

__kitS.section("<a id=\"1\" x=\"y\">hi<b/>there</a>", () => {
    const t = XMLParse('<a id="1" x="y">hi<b/>there</a>');
    eq(t.name, "a", "root");
    eq(t.attrs.id, "1", "an attribute");
    eq(t.attrs.x, "y", "a second attribute");
    eq(t.children.length, 3, "mixed content keeps its order");
    eq(t.children[0], "hi", "text first");
    eq(t.children[1].name, "b", "then the element");
    eq(t.children[2], "there", "then text again");
});

eq(XMLParse("<a><b><c>deep</c></b></a>").children[0].children[0].children[0], "deep",
   "nesting");

eq(XMLParse("<a>  </a>").children.length, 0, "whitespace-only text is trimmed by default");

eq(XMLParse("<a>  </a>", { trim: false }).children[0], "  ", "trim: false keeps it");

eq(XMLParse("<a> x </a>").children[0], "x", "text with content is trimmed by default");

eq(XMLParse("<?xml version=\"1.0\"?><a/>").name, "a", "an XML declaration is skipped");

eq(XMLParse("<!-- c --><a/>").name, "a", "a leading comment is skipped");

eq(XMLParse("<a><!-- c -->x</a>").children.length, 1, "comments are not tree nodes");

eq(XMLParse("<a:b xmlns:a='u'/>").name, "a:b", "a prefix is kept verbatim in the name");

eq(XMLParse("<a:b xmlns:a='u'/>").attrs["xmlns:a"], "u", "and so is xmlns");

eq(XMLParse("<a>&lt;&gt;&amp;&apos;&quot;</a>").children[0], "<>&'\"",
   "the five predefined entities");

eq(XMLParse("<a>&#65;&#x42;&#x1F600;</a>").children[0], "AB\u{1F600}",
   "decimal, hex and astral character references");

eq(XMLParse('<a b="&lt;&#65;"/>').attrs.b, "<A", "entities inside an attribute value");

eq(XMLParse("<a><![CDATA[<b>&amp;]]></a>").children[0], "<b>&amp;",
   "CDATA is literal: no markup, no entities");

__kitS.section("<!DOCTYPE lolz", () => {
    let doc = "<!DOCTYPE lolz [<!ENTITY lol \"lol\">";
    for (let i = 1; i <= 9; i++)
        doc += "<!ENTITY lol" + i + " \"&lol" + (i - 1) + ";&lol" + (i - 1) + ";\">";
    doc += "]><lolz>&lol9;</lolz>";
    let msg = "";
    try { XMLParse(doc); } catch (e) { msg = String(e.message); }
    assert(msg.indexOf("unknown entity") >= 0,
           "an entity bomb fails as an UNKNOWN entity, not a size cap (" + msg + ")");
});

__kitS.section("<!DOCTYPE r [<!ENTITY xxe SYSTEM \"file:///etc/passwd\">]>", () => {
    const doc = '<!DOCTYPE r [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>'
              + "<r>&xxe;</r>";
    throws(() => XMLParse(doc), "an external entity reference is refused");
});

eq(XMLParse('<!DOCTYPE html><a>x</a>').children[0], "x", "a DOCTYPE is skipped, not parsed");

eq(XMLParse('<!DOCTYPE r [<!ELEMENT r (#PCDATA)>]><r>x</r>').children[0], "x",
   "and so is an internal subset, brackets and all");

eq(XMLParse("<a>&custom;</a>", { entities: "keep" }).children[0], "&custom;",
   "entities: keep passes the reference through literally");

for (const bad of ["", "  ", "<a>", "</a>", "<a></b>", "<a><b></a></b>", "text",
                   "<a>x", "<a", "<>", "<1a/>", "<a b/>", "<a b=/>", "<a b='x/>",
                   "<a>&nope;</a>", "<a>&#xD800;</a>", "<a>&#0;</a>",
                   "<a>&#x110000;</a>", "<a/><b/>", "<a>]]></a>", "<a b='<'/>"])
    throws(() => XMLParse(bad), "refuses " + j(bad));

throws(() => XMLParse(42), "the input must be a string");

throws(() => XMLParse("<a/>", { entities: "expand" }), "an unknown entities mode");

__kitS.section("nesting past the cap is refused", () => {
    const deep = "<a>".repeat(300) + "</a>".repeat(300);
    throws(() => XMLParse(deep), "nesting past the cap is refused");
    const ok = "<a>".repeat(200) + "</a>".repeat(200);
    eq(XMLParse(ok).name, "a", "and 200 deep still parses");
});

throws(() => XMLParse('<a x="1" x="2"/>'), "a duplicate attribute name is refused");

throws(() => XMLParse('<a x="1" y="2" x="3"/>'), "a duplicate is caught past other names");

__kitS.section("<a X=\"1\" x=\"2\"/>", () => {
    const t = XMLParse('<a X="1" x="2"/>');
    eq(t.attrs.X, "1", "case-distinct attribute names stay distinct");
    eq(t.attrs.x, "2", "both values of a case pair survive");
});

__kitS.section("> in text is refused whole-buffer", () => {
    const doc = "<a>x]]>y</a>";
    throws(() => XMLParse(doc), "]]> in text is refused whole-buffer");
    let saw = 0, leaked = 0, refused = 0;
    for (let k = 1; k < doc.length; k++) {
        const out = [];
        const p = new SAXParser({ onText: (t) => out.push(t) });
        try {
            p.write(doc.slice(0, k));
            p.write(doc.slice(k));
            p.end();
        } catch (e) {
            refused++;
        }
        saw++;
        if (out.join("").indexOf("]]>") >= 0)
            leaked++;
    }
    assert(leaked === 0, "no split point emits ]]> as text (" + leaked + " of "
                         + saw + " leaked)");
    assert(refused === saw, "and every split that sees the whole sequence refuses ("
                            + refused + " of " + saw + ")");
});

const round = (x) => XMLStringify(XMLParse(x));

eq(round("<a/>"), "<a/>", "an empty element round-trips to the self-closing form");

eq(round("<a></a>"), "<a/>", "and so does the long form");

eq(round('<a id="1"/>'), '<a id="1"/>', "attributes survive");

eq(round("<a>x</a>"), "<a>x</a>", "text survives");

eq(round("<a><b>x</b></a>"), "<a><b>x</b></a>", "nesting survives");

__kitS.section("<r x=\"a&amp;b\"><i n=\"1\">one</i><i n=\"2\">two &lt;/i&gt;</i></r>", () => {
    const src = '<r x="a&amp;b"><i n="1">one</i><i n="2">two &lt;/i&gt;</i></r>';
    const once = XMLStringify(XMLParse(src));
    const twice = XMLStringify(XMLParse(once));
    eq(once, twice, "serialize is idempotent through a re-parse");
    eq(XMLParse(once).children[1].children[0], "two </i>",
       "and the escaped text comes back as the same characters");
});

__kitS.section("<a __proto__=\"x\"/>", () => {
    const t = XMLParse('<a __proto__="x"/>');
    eq(t.attrs.__proto__, "x", "an attribute named __proto__ is an own property");
    eq(Object.keys(t.attrs).length, 1, "and it is enumerable");
    eq(j(XMLToObject(XMLParse("<a><__proto__>v</__proto__></a>"))),
       '{"a":{"__proto__":"v"}}', "and so is an element named __proto__");
});

__kitS.section("<a b=\"x&#10;y&#9;z&#13;w\"/>", () => {
    const src = '<a b="x&#10;y&#9;z&#13;w"/>';
    const want = XMLParse(src).attrs.b;
    eq(want, "x\ny\tz\rw", "attr ws refs decode to the real chars");
    const out = XMLStringify(XMLParse(src));
    eq(XMLParse(out).attrs.b, want, "attr ws survives stringify (char refs emitted)");
    assert(out.indexOf("\n") < 0 && out.indexOf("\t") < 0,
       "no literal tab/newline in the emitted attribute");
    eq(XMLStringify({ name: "a", attrs: {}, children: ["x\ny\tz"] }), "<a>x\ny\tz</a>",
       "text context keeps literal whitespace");
});

__kitS.section("<!DOCTYPE r", () => {
    eq(XMLParse("<!DOCTYPE r [ <!-- [ --> <!ELEMENT r ANY> ]><r/>").name, "r",
       "a bracket inside a subset comment does not break the skip");
    eq(XMLParse('<!DOCTYPE r SYSTEM "http://x/a>b"><r/>').name, "r",
       "a > inside a quoted SYSTEM literal does not end the DOCTYPE early");
    eq(XMLParse("<!DOCTYPE r [<!ENTITY g \">\">]><r/>").name, "r",
       "a > inside a quoted entity value still skips");
});

__kitS.section("a leading BOM is skipped when parsing", () => {
    eq(XMLParse("\uFEFF<a/>").name, "a", "a leading BOM is skipped when parsing");
    const seen = [];
    const p = new SAXParser({ onOpen: (n2) => seen.push(n2), onText: (t) => seen.push("text:" + t) });
    p.write(new Uint8Array([0xEF, 0xBB, 0xBF, 0x3C, 0x61, 0x2F, 0x3E]));
    p.end();
    eq(j(seen), '["a"]', "a BOM fed as bytes produces no phantom text event");
    const seen2 = [];
    const p2 = new SAXParser({ onOpen: (n2) => seen2.push(n2), onText: (t) => seen2.push("text:" + t) });
    for (const b of [0xEF, 0xBB, 0xBF, 0x3C, 0x61, 0x2F, 0x3E]) p2.write(new Uint8Array([b]));
    p2.end();
    eq(j(seen2), '["a"]', "a BOM assembled one byte at a time is still skipped");
    eq(XMLParse("<a>\uFEFF</a>").children[0], "\uFEFF", "a mid-document BOM stays content");
});

function throwsMsg(fn, re, msg) {
    let m = "";
    try { fn(); } catch (e) { m = String(e.message); }
    assert(re.test(m), msg + " (got: " + (m || "no throw") + ")");
}

__kitS.section("XMLParse bag names the key AND the valid set", () => {
    throwsMsg(() => XMLParse("<a/>", { multipe: true }),
              /^unknown option "multipe" \(valid: multiple, trim, entities\)$/,
              "XMLParse bag names the key AND the valid set");
    throwsMsg(() => XMLParse("<a/>", { trim: true, entites: "keep" }),
              /^unknown option "entites" \(valid: /, "XMLParse bag rejects a second unknown key");
    eq(XMLParse("<a><b/></a>", { multiple: true, trim: false, entities: "keep" }).length,
       1, "a fully valid XMLParse bag is unchanged");
    throwsMsg(() => XMLStringify({ name: "a" }, { indnt: 2 }),
              /^unknown option "indnt" \(valid: indent\)$/,
              "XMLStringify bag is strict");
    eq(XMLStringify(XMLParse("<a><b/></a>"), { indent: 2 }),
       "<a>\n  <b/>\n</a>", "a valid XMLStringify bag is unchanged");
    assert(typeof new SAXParser({ onOpen: () => {}, meta: 1 }).write === "function",
           "SAXParser handlers object tolerates extra keys (documented duality skip)");
    throws(() => XMLParse("<a/>", 42), "a primitive bag is still refused by XMLParse");
    eq(XMLStringify({ name: "a" }, 42), "<a/>", "a primitive bag reads as absent for XMLStringify");
});
__kitS.end();

if (fails) {
    print("test_xml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_xml failed");
}
print("test_xml: " + n + " assertions, 0 failures");
