// covers: dyna:xml.SAXParser
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_sax_parser");
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

function events(xml, chunk) {
    const out = [];
    const p = new SAXParser({
        onOpen: (name, attrs) => out.push(["open", name, j(attrs)]),
        onClose: (name) => out.push(["close", name]),
        onText: (t) => out.push(["text", t]),
        onCData: (t) => out.push(["cdata", t]),
        onComment: (t) => out.push(["comment", t]),
        onPI: (target, data) => out.push(["pi", target, data])
    });
    if (chunk === undefined) {
        p.write(xml);
    } else {
        for (let i = 0; i < xml.length; i += chunk) p.write(xml.slice(i, i + chunk));
    }
    p.end();
    return j(out);
}

const DOC = '<?xml version="1.0"?><!-- head --><root a="1" b="&lt;">'
          + "<item id='x'>text &amp; more</item><item/><![CDATA[raw <tag>]]>"
          + "<?pi some data?></root>";

__kitS.section("SAX reports the root open", () => {
    const whole = events(DOC);
    assert(whole.indexOf('["open","root"') >= 0, "SAX reports the root open");
    assert(whole.indexOf('["pi","pi","some data"]') >= 0, "and a processing instruction");
    assert(whole.indexOf('["cdata","raw <tag>"]') >= 0, "and CDATA, literally");
    assert(whole.indexOf('["comment"," head "]') >= 0, "and a comment");
});

__kitS.section("chunk", () => {
    const whole = events(DOC);
    let bad = 0, checked = 0;
    for (let k = 1; k <= DOC.length; k++) {
        checked++;
        if (events(DOC, k) !== whole) {
            bad++;
            if (bad < 3) print("  chunk " + k + " differs");
        }
    }
    assert(bad === 0, "every chunk size gives the identical event stream ("
                      + (checked - bad) + "/" + checked + ")");
    assert(checked > 100, "the sweep covered every offset (" + checked + ")");
});

__kitS.section("<a x=\"1\">", () => {
    const bits = ['<a x="1">', "hi &amp; bye", "<![CDATA[c]]>", "<!-- k -->",
                  "<?t d?>", "</a>"];
    const doc = bits.join("");
    const whole = events(doc);
    let bad = 0;
    for (let i = 1; i < doc.length; i++) {
        const out = [];
        const p = new SAXParser({
            onOpen: (nm, at) => out.push(["open", nm, j(at)]),
            onClose: (nm) => out.push(["close", nm]),
            onText: (t) => out.push(["text", t]),
            onCData: (t) => out.push(["cdata", t]),
            onComment: (t) => out.push(["comment", t]),
            onPI: (t, d) => out.push(["pi", t, d])
        });
        p.write(doc.slice(0, i));
        p.write(doc.slice(i));
        p.end();
        if (j(out) !== whole) { bad++; if (bad < 3) print("  split at " + i + " differs"); }
    }
    assert(bad === 0, "every two-write split point agrees with the whole buffer");
});

__kitS.section("a parser with no handlers still consumes the document", () => {
    const p = new SAXParser({});
    p.write("<a>x</a>");
    p.end();
    assert(true, "a parser with no handlers still consumes the document");
    throws(() => p.write("<b/>"), "write after end() is refused");
    throws(() => p.end(), "and so is a second end()");
});

throws(() => new SAXParser({ onOpen: 42 }), "a non-function handler is refused");

__kitS.section("a non-object handlers argument is refused", () => {
    throws(() => new SAXParser(42), "a non-object handlers argument is refused");
    const pull = new SAXParser();
    pull.write("<a>x</a>");
    const seen = [];
    let ev;
    while ((ev = pull.next()) !== null) seen.push(ev.event);
    pull.end();
    eq(seen.join(","), "open,text,close", "no-argument SAXParser is a pull parser");
});

throws(() => { const p = new SAXParser({}); p.write(); }, "write needs a chunk");

__kitS.section("a Uint8Array chunk", () => {
    const out = [];
    const p = new SAXParser({ onText: (t) => out.push(t) });
    const bytes = new Uint8Array([0x3c, 0x61, 0x3e, 0x68, 0x69, 0x3c, 0x2f, 0x61, 0x3e]);
    p.write(bytes);
    p.end();
    eq(out.join(""), "hi", "a Uint8Array chunk");
});

__kitS.section("write() from inside a handler is refused, not corrupted", () => {
    let threw = false;
    const p = new SAXParser({ onOpen() { try { p.write("<x/>"); } catch (e) { threw = true; } } });
    p.write("<a/>");
    p.end();
    assert(threw, "write() from inside a handler is refused, not corrupted");
});

__kitS.section("a handler exception propagates", () => {
    const p = new SAXParser({ onOpen() { throw new Error("stop"); } });
    throws(() => p.write("<a/>"), "a handler exception propagates");
});

throws(() => new SAXParser({}).write("<a>&nope;</a>"), "SAX refuses an unknown entity too");

__kitS.section("<a x=\"1\" x=\"2\"/>", () => {
    const out = [];
    throws(() => {
        const p = new SAXParser({ onOpen: (n, at) => out.push(at) });
        p.write('<a x="1" x="2"/>');
        p.end();
    }, "SAX refuses a duplicate attribute too");
    eq(out.length, 0, "onOpen never fires for the malformed tag");
});

__kitS.section("token limit", () => {
    const CAP = 16 * 1024 * 1024;

    const big = "x".repeat(CAP + 1);
    let msg = "", fast = true;
    {
        const t0 = cpuNow();
        const p = new SAXParser({ onComment() {} });
        try { p.write("<r><!--" + big + "--></r>"); p.end(); }
        catch (e) { msg = String(e.message); }
        fast = (cpuNow() - t0) < 20000;
    }
    assert(msg.indexOf("token limit") >= 0, "an over-cap comment fails with the token limit (" + msg + ")");
    assert(fast, "the over-cap comment throws promptly, without hanging");
    throws(() => { const p = new SAXParser({}); p.write("<r><![CDATA[" + big + "]]></r>"); p.end(); },
           "an over-cap CDATA section fails too");
    throws(() => { const p = new SAXParser({}); p.write("<r><?pi " + big + "?></r>"); p.end(); },
           "an over-cap processing instruction fails too");

    const atCap = "x".repeat(CAP);
    {
        let got = "";
        const p = new SAXParser({ onComment: (t) => { got = t; } });
        p.write("<r><!--" + atCap + "--></r>");
        p.end();
        assert(got.length === CAP, "a comment of exactly the cap size parses");
    }
    throws(() => { const p = new SAXParser({}); p.write("<r><!--" + big + "--></r>"); p.end(); },
           "a comment one byte OVER the cap fails");

    {
        let got = [];
        const p = new SAXParser({
            onComment: (t) => got.push(["c", t]),
            onCData: (t) => got.push(["d", t]),
            onPI: (t, d) => got.push(["p", t, d])
        });
        p.write('<r><!--ten bytes--><![CDATA[<b>&amp;]]><?pi data?></r>');
        p.end();
        eq(j(got), '[["c","ten bytes"],["d","<b>&amp;"],["p","pi","data"]]',
           "small comment, CDATA and PI still emit");
    }

    throws(() => {
        const p = new SAXParser({});
        p.write("<r><!--");
        p.write(big);
        p.write("--></r>");
        p.end();
    }, "a comment streamed in chunks over the cap fails as it accumulates");

    for (const head of ["<r><a", "<r><a ", "<r><!DOCTYPE "]) {
        let msg = "";
        try { const p = new SAXParser({}); p.write(head + big); p.end(); }
        catch (e) { msg = String(e.message); }
        assert(msg.indexOf("token limit") >= 0,
               "an unterminated " + JSON.stringify(head) + " over the cap fails "
               + "with the token limit (" + msg + ")");
    }
});

__kitS.section("close tag exceeds the token limit", () => {
    const CAP = 16 * 1024 * 1024;
    let msg = "";
    try { const p = new SAXParser({}); p.write("<r>x</r></" + "a".repeat(CAP + 1)); p.end(); }
    catch (e) { msg = String(e.message); }
    assert(msg.indexOf("close tag exceeds the token limit") >= 0,
           "an over-cap close tag fails at the cap, not with a generic error (" + msg + ")");
});

for (const [doc, what] of [["<a/><!--x", "unterminated comment"],
                           ["<a><![CDATA[x", "unterminated CDATA"],
                           ["<!DOCTYPE x", "unterminated DOCTYPE"]]) {
    let msg = "";
    try { const p = new SAXParser({}); p.write(doc); p.end(); }
    catch (e) { msg = String(e.message); }
    assert(msg.indexOf(what) >= 0, doc + " -> " + what + " (" + msg + ")");
    let msg2 = "";
    try { XMLParse(doc); } catch (e) { msg2 = String(e.message); }
    assert(msg2.indexOf(what) >= 0, "tree parse of " + doc + " names the token too (" + msg2 + ")");
}

__kitS.section("<a>real</a>", () => {
    const out = [];
    const p = new SAXParser({ onText: (t) => out.push(t) });
    p.write("\n  <a>real</a>\n  ");
    p.end();
    eq(j(out), '["real"]', "blank top-level text is silent in SAX (padding dropped)");
    throws(() => { const q = new SAXParser({}); q.write("x<a/>"); q.end(); },
           "non-blank text before the root is still refused");
    throws(() => { const q = new SAXParser({}); q.write("<a><![CDATA[ ]]></a><![CDATA[ ]]>"); q.end(); },
           "top-level CDATA is still refused even when whitespace-only");
});
__kitS.end();

if (fails) {
    print("test_xml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_xml failed");
}
print("test_xml: " + n + " assertions, 0 failures");
