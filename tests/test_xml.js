/* test_xml.js -- the scanner, tree, SAX and serializer in dyna:xml (design 05).
 *
 * THE ORACLE THAT MATTERS IS THE SPLIT SWEEP: the same document is fed to the
 * streaming parser at EVERY byte offset, one write of k bytes then the rest,
 * and the event stream must be identical to the whole-buffer parse. A resume
 * bug is only observable while exactly the wrong bytes are outstanding, so
 * splitting at one convenient place proves nothing.
 *
 * The security cases are not decoration: an entity bomb and an external-entity
 * reference must be REFUSED, and the tests below check that the refusal comes
 * from "no entity can exist" rather than from a size cap.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_xml.js
 */
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";

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

/* ------------------------------------------------------------------ tree */

{
    const t = XMLParse("<a/>");
    eq(t.name, "a", "the root name");
    eq(j(t.attrs), "{}", "no attributes");
    eq(t.children.length, 0, "no children");
}
{
    const t = XMLParse('<a id="1" x="y">hi<b/>there</a>');
    eq(t.name, "a", "root");
    eq(t.attrs.id, "1", "an attribute");
    eq(t.attrs.x, "y", "a second attribute");
    eq(t.children.length, 3, "mixed content keeps its order");
    eq(t.children[0], "hi", "text first");
    eq(t.children[1].name, "b", "then the element");
    eq(t.children[2], "there", "then text again");
}
eq(XMLParse("<a><b><c>deep</c></b></a>").children[0].children[0].children[0], "deep",
   "nesting");
eq(XMLParse("<a>  </a>").children.length, 0, "whitespace-only text is trimmed by default");
eq(XMLParse("<a>  </a>", { trim: false }).children[0], "  ", "trim: false keeps it");
eq(XMLParse("<a> x </a>").children[0], " x ", "text with content keeps its spaces");
eq(XMLParse("<?xml version=\"1.0\"?><a/>").name, "a", "an XML declaration is skipped");
eq(XMLParse("<!-- c --><a/>").name, "a", "a leading comment is skipped");
eq(XMLParse("<a><!-- c -->x</a>").children.length, 1, "comments are not tree nodes");
eq(XMLParse("<a:b xmlns:a='u'/>").name, "a:b", "a prefix is kept verbatim in the name");
eq(XMLParse("<a:b xmlns:a='u'/>").attrs["xmlns:a"], "u", "and so is xmlns");

/* Entities: the five predefined ones and numeric references. Nothing else. */
eq(XMLParse("<a>&lt;&gt;&amp;&apos;&quot;</a>").children[0], "<>&'\"",
   "the five predefined entities");
eq(XMLParse("<a>&#65;&#x42;&#x1F600;</a>").children[0], "AB\u{1F600}",
   "decimal, hex and astral character references");
eq(XMLParse('<a b="&lt;&#65;"/>').attrs.b, "<A", "entities inside an attribute value");
eq(XMLParse("<a><![CDATA[<b>&amp;]]></a>").children[0], "<b>&amp;",
   "CDATA is literal: no markup, no entities");

/* ----------------------------------------------- XXE and the entity bomb */

{
    /* THE BILLION LAUGHS. It must fail because no entity can be DECLARED --
     * `&lol1;` is simply unknown -- not because an expansion counter tripped. */
    let doc = "<!DOCTYPE lolz [<!ENTITY lol \"lol\">";
    for (let i = 1; i <= 9; i++)
        doc += "<!ENTITY lol" + i + " \"&lol" + (i - 1) + ";&lol" + (i - 1) + ";\">";
    doc += "]><lolz>&lol9;</lolz>";
    let msg = "";
    try { XMLParse(doc); } catch (e) { msg = String(e.message); }
    assert(msg.indexOf("unknown entity") >= 0,
           "an entity bomb fails as an UNKNOWN entity, not a size cap (" + msg + ")");
}
{
    /* XXE: the external entity is never fetched because it is never declared. */
    const doc = '<!DOCTYPE r [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>'
              + "<r>&xxe;</r>";
    throws(() => XMLParse(doc), "an external entity reference is refused");
}
eq(XMLParse('<!DOCTYPE html><a>x</a>').children[0], "x", "a DOCTYPE is skipped, not parsed");
eq(XMLParse('<!DOCTYPE r [<!ELEMENT r (#PCDATA)>]><r>x</r>').children[0], "x",
   "and so is an internal subset, brackets and all");
/* `keep` passes an unknown entity through as text -- it never expands it. */
eq(XMLParse("<a>&custom;</a>", { entities: "keep" }).children[0], "&custom;",
   "entities: keep passes the reference through literally");

/* --------------------------------------------------------------- refusals */

for (const bad of ["", "  ", "<a>", "</a>", "<a></b>", "<a><b></a></b>", "text",
                   "<a>x", "<a", "<>", "<1a/>", "<a b/>", "<a b=/>", "<a b='x/>",
                   "<a>&nope;</a>", "<a>&#xD800;</a>", "<a>&#0;</a>",
                   "<a>&#x110000;</a>", "<a/><b/>", "<a>]]></a>", "<a b='<'/>"])
    throws(() => XMLParse(bad), "refuses " + j(bad));
throws(() => XMLParse(42), "the input must be a string");
throws(() => XMLParse("<a/>", { entities: "expand" }), "an unknown entities mode");
{
    /* The depth cap is a RangeError-shaped refusal, not a stack overflow. */
    const deep = "<a>".repeat(300) + "</a>".repeat(300);
    throws(() => XMLParse(deep), "nesting past the cap is refused");
    const ok = "<a>".repeat(200) + "</a>".repeat(200);
    eq(XMLParse(ok).name, "a", "and 200 deep still parses");
}

/* --------------------------------------------------------------- SAX */

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
{
    const whole = events(DOC);
    assert(whole.indexOf('["open","root"') >= 0, "SAX reports the root open");
    assert(whole.indexOf('["pi","pi","some data"]') >= 0, "and a processing instruction");
    assert(whole.indexOf('["cdata","raw <tag>"]') >= 0, "and CDATA, literally");
    assert(whole.indexOf('["comment"," head "]') >= 0, "and a comment");
}
{
    /* THE SPLIT SWEEP. Every chunk size from 1 byte up, all identical. */
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
}
{
    /* A split INSIDE every kind of token is the case a resume bug lives in. */
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
}
{
    const p = new SAXParser({});
    p.write("<a>x</a>");
    p.end();
    assert(true, "a parser with no handlers still consumes the document");
    throws(() => p.write("<b/>"), "write after end() is refused");
    throws(() => p.end(), "and so is a second end()");
}
throws(() => new SAXParser({ onOpen: 42 }), "a non-function handler is refused");
throws(() => new SAXParser(), "handlers are required");
throws(() => { const p = new SAXParser({}); p.write(); }, "write needs a chunk");
{
    /* Bytes as well as strings, because a chunk usually arrives as bytes. */
    const out = [];
    const p = new SAXParser({ onText: (t) => out.push(t) });
    const bytes = new Uint8Array([0x3c, 0x61, 0x3e, 0x68, 0x69, 0x3c, 0x2f, 0x61, 0x3e]);
    p.write(bytes);
    p.end();
    eq(out.join(""), "hi", "a Uint8Array chunk");
}
{
    /* A handler that re-enters write() would corrupt the carry buffer. */
    let threw = false;
    const p = new SAXParser({ onOpen() { try { p.write("<x/>"); } catch (e) { threw = true; } } });
    p.write("<a/>");
    p.end();
    assert(threw, "write() from inside a handler is refused, not corrupted");
}
{
    /* A throwing handler stops the parse and the exception reaches the caller. */
    const p = new SAXParser({ onOpen() { throw new Error("stop"); } });
    throws(() => p.write("<a/>"), "a handler exception propagates");
}
throws(() => new SAXParser({}).write("<a>&nope;</a>"), "SAX refuses an unknown entity too");

/* XML well-formedness: unique attribute names per element. Names compare as
 * case-SENSITIVE bytes -- nothing here case-folds, so `X` and `x` are distinct
 * and both survive. */
throws(() => XMLParse('<a x="1" x="2"/>'), "a duplicate attribute name is refused");
throws(() => XMLParse('<a x="1" y="2" x="3"/>'), "a duplicate is caught past other names");
{
    const t = XMLParse('<a X="1" x="2"/>');
    eq(t.attrs.X, "1", "case-distinct attribute names stay distinct");
    eq(t.attrs.x, "2", "both values of a case pair survive");
}
{
    const out = [];
    throws(() => {
        const p = new SAXParser({ onOpen: (n, at) => out.push(at) });
        p.write('<a x="1" x="2"/>');
        p.end();
    }, "SAX refuses a duplicate attribute too");
    eq(out.length, 0, "onOpen never fires for the malformed tag");
}

/* --------------------------------------- comment/CDATA/PI token-size cap */

/* XML_MAX_TOKEN is 16 MiB (16u<<20). A comment/CDATA/PI is ONE token, so the
 * same cap that bounds a text run must bound it too: a single streamed multi-GB
 * comment grows the carry without limit otherwise. Use ~20 MiB (NOT 10 GB) so
 * the scan throws promptly and the test stays fast. */
{
    const CAP = 16 * 1024 * 1024;

    /* A comment body just OVER the cap fails fast, catchable, no OOM. */
    const big = "x".repeat(CAP + 1);
    let msg = "", fast = true;
    {
        const t0 = Date.now();
        const p = new SAXParser({ onComment() {} });
        try { p.write("<r><!--" + big + "--></r>"); p.end(); }
        catch (e) { msg = String(e.message); }
        fast = (Date.now() - t0) < 20000;   /* bounded, not a hang */
    }
    assert(msg.indexOf("token limit") >= 0, "an over-cap comment fails with the token limit (" + msg + ")");
    assert(fast, "the over-cap comment throws promptly, without hanging");
    throws(() => { const p = new SAXParser({}); p.write("<r><![CDATA[" + big + "]]></r>"); p.end(); },
           "an over-cap CDATA section fails too");
    throws(() => { const p = new SAXParser({}); p.write("<r><?pi " + big + "?></r>"); p.end(); },
           "an over-cap processing instruction fails too");

    /* A comment of EXACTLY cap size is fine; cap+1 is not. */
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

    /* Ordinary small comments, CDATA and PIs still work. */
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

    /* One huge chunk across N writes (the real streaming case) must fail too. */
    throws(() => {
        const p = new SAXParser({});
        p.write("<r><!--");
        p.write(big);
        p.write("--></r>");
        p.end();
    }, "a comment streamed in chunks over the cap fails as it accumulates");

    /* The OPEN TAG and the DOCTYPE scan paths carry the same cap: an
     * unterminated one must refuse AT THE TOKEN LIMIT, not stream forever --
     * and not merely die later with the generic end-of-document error. */
    for (const head of ["<r><a", "<r><a ", "<r><!DOCTYPE "]) {
        let msg = "";
        try { const p = new SAXParser({}); p.write(head + big); p.end(); }
        catch (e) { msg = String(e.message); }
        assert(msg.indexOf("token limit") >= 0,
               "an unterminated " + JSON.stringify(head) + " over the cap fails "
               + "with the token limit (" + msg + ")");
    }
}

/* ----------------------------- the `]]>` guard across a chunk boundary */

/* The whole-buffer parse refuses `]]>` in text; the STREAMING parser used to
 * emit the assembled bytes as text whenever a write ended inside the sequence.
 * EVERY two-write split must either refuse or keep `]]>` out of the text. */
{
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
}

/* ---------------------------------------------------------- stringify */

const round = (x) => XMLStringify(XMLParse(x));
eq(round("<a/>"), "<a/>", "an empty element round-trips to the self-closing form");
eq(round("<a></a>"), "<a/>", "and so does the long form");
eq(round('<a id="1"/>'), '<a id="1"/>', "attributes survive");
eq(round("<a>x</a>"), "<a>x</a>", "text survives");
eq(round("<a><b>x</b></a>"), "<a><b>x</b></a>", "nesting survives");
eq(XMLStringify(XMLParse("<a>&lt;&amp;</a>")), "<a>&lt;&amp;</a>",
   "text is re-escaped, so the output re-parses");
eq(XMLStringify(XMLParse('<a b="&quot;&lt;"/>')), '<a b="&quot;&lt;"/>',
   "and so is an attribute value");
eq(XMLStringify({ name: "a", attrs: { x: "1" }, children: ["t"] }), '<a x="1">t</a>',
   "a hand-built node serializes");
eq(XMLStringify({ name: "a" }), "<a/>", "attrs and children are optional");
eq(XMLStringify(XMLParse("<a><b/><c/></a>"), { indent: 2 }),
   "<a>\n  <b/>\n  <c/>\n</a>", "indent puts each element on its own line");
throws(() => XMLStringify({ name: 42 }), "a node needs a string name");
throws(() => XMLStringify({ name: "" }), "and a non-empty one");
throws(() => XMLStringify({ name: "1bad" }), "that is a valid element name");
throws(() => XMLStringify(), "stringify needs a node");
throws(() => XMLStringify({ name: "a" }, { indent: 99 }), "indent is bounded");
{
    /* The round trip is blind to a consistent mistake, so check the STRING
     * against what an independent reader sees: parse the output again. */
    const src = '<r x="a&amp;b"><i n="1">one</i><i n="2">two &lt;/i&gt;</i></r>';
    const once = XMLStringify(XMLParse(src));
    const twice = XMLStringify(XMLParse(once));
    eq(once, twice, "serialize is idempotent through a re-parse");
    eq(XMLParse(once).children[1].children[0], "two </i>",
       "and the escaped text comes back as the same characters");
}

/* ----------------------------------------------------------- toObject */

eq(j(XMLToObject(XMLParse("<a>x</a>"))), '{"a":"x"}', "text-only collapses to a string");
eq(j(XMLToObject(XMLParse("<a><b>1</b><c>2</c></a>"))), '{"a":{"b":"1","c":"2"}}',
   "children become keys");
eq(j(XMLToObject(XMLParse("<a><b>1</b><b>2</b></a>"))), '{"a":{"b":["1","2"]}}',
   "a repeated name becomes an array");
eq(j(XMLToObject(XMLParse("<a><b>1</b><b>2</b><b>3</b></a>"))),
   '{"a":{"b":["1","2","3"]}}', "and keeps growing");
eq(j(XMLToObject(XMLParse('<a id="7">x</a>'))), '{"a":{"@id":"7","#text":"x"}}',
   "attributes are @-prefixed and text lands in #text");
eq(j(XMLToObject(XMLParse("<a/>"))), '{"a":{}}', "an empty element is an empty object");
throws(() => XMLToObject("x"), "toObject needs an element");

/* --------------------------------------------- prototype safety */

{
    /* An attribute called __proto__ must become an own property. Setting it
     * would retarget the object's prototype and the key would vanish. */
    const t = XMLParse('<a __proto__="x"/>');
    eq(t.attrs.__proto__, "x", "an attribute named __proto__ is an own property");
    eq(Object.keys(t.attrs).length, 1, "and it is enumerable");
    eq(j(XMLToObject(XMLParse("<a><__proto__>v</__proto__></a>"))),
       '{"a":{"__proto__":"v"}}', "and so is an element named __proto__");
}

if (fails) {
    print("test_xml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_xml failed");
}
print("test_xml: " + n + " assertions, 0 failures");
