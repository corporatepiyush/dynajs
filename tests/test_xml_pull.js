// flags: --std
import { SAXParser, XMLParse } from "dyna:xml";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqj(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b), msg + " (got " +
        JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

function pullAll(p, doc) {
    if (doc !== undefined) p.write(doc);
    const out = [];
    let ev;
    while ((ev = p.next()) !== null) out.push(ev);
    return out;
}

{
    const evs = pullAll(new SAXParser(),
        '<?xml version="1.0"?><root a="1" b="two"><b>hi</b><!--c-->' +
        '<![CDATA[raw <tag>]]><?pi data?></root>');
    eq(evs.length, 9, "every event surfaces");
    eqj(evs[0], { event: "pi", offset: 0, name: "xml",
        text: "version=\"1.0\"" }, "the declaration is a pi event");
    eq(evs[1].event, "open");
    eq(evs[1].name, "root");
    eqj(evs[1].attrs, { a: "1", b: "two" }, "attrs ride the open event");
    eq(evs[2].event, "open"); eq(evs[2].name, "b"); eq(evs[2].offset, 41);
    eq(evs[3].event, "text"); eq(evs[3].text, "hi");
    eq(evs[4].event, "close"); eq(evs[4].name, "b");
    eq(evs[5].event, "comment"); eq(evs[5].text, "c");
    eq(evs[6].event, "cdata"); eq(evs[6].text, "raw <tag>");
    eq(evs[7].event, "pi"); eq(evs[7].name, "pi"); eq(evs[7].text, "data");
    eq(evs[8].event, "close"); eq(evs[8].name, "root");
}

{
    const doc = '<a x="1">hi<b/></a>';
    const evs = pullAll(new SAXParser(), doc);
    for (const ev of evs) {
        if (ev.event === "open")
            eq(doc.slice(ev.offset, ev.offset + 1), "<", ev.name + " opens at its offset");
        if (ev.event === "text")
            eq(doc.substr(ev.offset, ev.text.length), ev.text, "text offset is byte-exact");
        if (ev.event === "close")
            eq(doc.slice(ev.offset, ev.offset + 2), "</", ev.name + " close at its offset");
    }
}

{
    const doc = '<!-- top --><feed id="9"><meta name="t">v &amp; w</meta>' +
        '<entry>one</entry><entry>two &lt;gt&lt;</entry><!-- mid -->' +
        '<![CDATA[1 < 2]]><?x y?></feed><!-- tail -->';
    const kinds = (evs) => evs.map((e) => e.event + ":" + (e.name !== undefined ? e.name : e.text));
    const pulled = kinds(pullAll(new SAXParser(), doc));

    const pushed = [];
    const p = new SAXParser({
        onOpen: (n2, a) => pushed.push("open:" + n2),
        onClose: (n2) => pushed.push("close:" + n2),
        onText: (t) => pushed.push("text:" + t),
        onCData: (t) => pushed.push("cdata:" + t),
        onComment: (t) => pushed.push("comment:" + t),
        onPI: (n2, d) => pushed.push("pi:" + n2)
    });
    p.write(doc);
    p.end();
    eqj(pulled, pushed, "pull and push agree on the whole document");

    let bad = 0, checked = 0;
    for (let i = 1; i < doc.length; i++) {
        const q = new SAXParser();
        q.write(doc.slice(0, i));
        q.write(doc.slice(i));
        const got = kinds((() => { const r = []; let e; while ((e = q.next()) !== null) r.push(e); return r; })());
        q.end();
        checked++;
        if (j(got) !== j(pulled)) { bad++; if (bad < 3) print("  split at " + i + " differs"); }
    }
    function j(x) { return JSON.stringify(x); }
    eq(bad, 0, "every split point agrees with the whole-buffer pull (" + checked + ")");
    assert(checked > 150, "the sweep covered every offset");
}

{
    let doc = "";
    for (let i = 0; i < 250; i++) doc += "<l" + i + " x=\"" + i + "\">";
    doc += "core";
    for (let i = 249; i >= 0; i--) doc += "</l" + i + ">";
    const evs = pullAll(new SAXParser(), doc);
    eq(evs.filter((e) => e.event === "open").length, 250, "250 opens");
    eq(evs.filter((e) => e.event === "close").length, 250, "250 closes");
    eq(evs.filter((e) => e.event === "text")[0].text, "core", "the core text");
}

{
    const evs = pullAll(new SAXParser(),
        "<a>&lt;&gt;&amp;&apos;&quot;&#65;&#x1F600;b</a>");
    eq(evs[1].text, "<>&'\"A\u{1F600}b", "entities decode in the pull form");
}
{
    const p = new SAXParser(undefined, { entities: "keep" });
    p.write("<a>&foo;&lt;</a>");
    const evs = pullAll(p);
    eq(evs[1].text, "&foo;<", "entities: keep passes unknown through");
}

{
    const doc = "<r><item n=\"1\">aaa</item><item n=\"2\">bbb</item></r>";
    const q = new SAXParser();
    const rows = [];
    for (const ch of doc) {
        q.write(ch);
        let e;
        while ((e = q.next()) !== null)
            rows.push([e.event, e.name !== undefined ? e.name : e.text]);
    }
    q.end();
    eqj(rows, [
        ["open", "r"], ["open", "item"], ["text", "aaa"], ["close", "item"],
        ["open", "item"], ["text", "bbb"], ["close", "item"], ["close", "r"]
    ], "byte-at-a-time pull reassembles every token");
}

{
    const p = new SAXParser();
    p.write("<a>&nope;</a>");
    p.end();
    let saw = [];
    try { for (;;) { const e = p.next(); if (e === null) break; saw.push(e.event); }
        assert(false, "the unknown entity should throw from next()"); }
    catch (e) { assert(e.message.indexOf("unknown entity") >= 0, "entity error at next()"); }
    assert(saw.join(",") === "open", "the events before the error arrived");
    throws(() => p.next(), "the pull error is sticky");
}
{
    const p = new SAXParser();
    p.write("<a>");
    p.end();
    try { for (;;) { if (p.next() === null) break; } assert(false, "unclosed"); }
    catch (e) { eq(e.message, "SAXParser: unclosed element", "unclosed at next()"); }
}
{
    const p = new SAXParser();
    p.write("text before root");
    p.end();
    throws(() => p.next(), "text outside the root throws at next()");
}
{
    const p = new SAXParser();
    p.write("<a>x</a>trailing");
    p.end();
    throws(() => { for (;;) { if (p.next() === null) break; } },
        "trailing content after the root throws");
}
{
    const p = new SAXParser();
    p.write("");
    p.end();
    throws(() => p.next(), "a document with no root throws");
}
{
    const p = new SAXParser();
    p.write("<a>&nope;</a>");
    p.end();
    throws(() => { for (;;) { if (p.next() === null) break; } },
        "the error waits for next()");
}

{
    const p = new SAXParser();
    p.write("<a/>");
    p.end();
    assert(p.next() !== null, "one event");
    eq(p.next(), null, "done");
    eq(p.next(), null, "still done");
}

{
    const cases = [
        ["close", "</a>"],
        ["text", "<a>]]>x</a>"],
        ["text", "<a>&x y;</a>"],
        ["cdata", "<a><![CDATA[x]]></a"],
        ["comment", "<a><!--x-></a>"],
        ["pi", "<a><?p d></a>"]
    ];
    for (const [kind, doc] of cases) {
        const p = new SAXParser();
        p.write(doc);
        p.end();
        throws(() => { for (;;) { if (p.next() === null) break; } },
            "malformed " + kind + " input is refused");
    }
}

{
    throws(() => { new SAXParser({}).write("<a>&nope;</a>"); },
        "push mode ({}): entity error at write()");
    throws(() => { const p = new SAXParser({}); p.write("<a>"); p.end(); },
        "push mode ({}): unclosed at end()");
}
{
    const seen = [];
    const p = new SAXParser({ onOpen: (n2) => seen.push(n2) });
    p.write("<a><b/></a>");
    p.end();
    eq(seen.join("|"), "a|b", "handlers fired during write()");
    eq(p.next(), null, "a push parser has nothing pending for next()");
}
{
    let threw = false;
    const p = new SAXParser({ onOpen() { try { p.next(); } catch (e) { threw = true; } } });
    p.write("<a/>");
    p.end();
    assert(threw, "next() from a handler is refused");
}
throws(() => new SAXParser(42), "a non-object handlers argument is refused");
throws(() => new SAXParser({ onOpen: 1 }), "a non-function handler is refused");

{
    const big = "<a>" + "x".repeat(8 * 1024 * 1024) + "</a>";
    const p = new SAXParser();
    p.write(big);
    let evs = 0, maxLen = 0, e;
    while ((e = p.next()) !== null) { evs++; if (e.text && e.text.length > maxLen) maxLen = e.text.length; }
    p.end();
    eq(evs, 3, "8MB token: three events");
    eq(maxLen, 8 * 1024 * 1024, "the text event carries the full run");
}
{
    const p = new SAXParser();
    p.write("\uFEFF");
    eq(p.next(), null, "BOM pending: nothing yet");
    p.write("<a/>");
    const evs = [];
    let e;
    while ((e = p.next()) !== null) evs.push(e.event + "@" + e.offset);
    p.end();
    eqj(evs, ["open@3"], "BOM skipped; a self-closed tag is ONE open event");
}
{
    const p = new SAXParser();
    p.write("<a>&x;</a>");
    p.end();
    throws(() => { for (;;) { if (p.next() === null) break; } }, "entity error");
    throws(() => p.write("<b/>"), "write after end refused after a failure");
}

(async () => {
    const out = [];
    for await (const ev of SAXParser.iterate(['<a x="1">', 'hi</a>']))
        out.push([ev.event, ev.name !== undefined ? ev.name : ev.text]);
    eqj(out, [["open", "a"], ["text", "hi"], ["close", "a"]], "iterate over an array");

    const out2 = [];
    for await (const ev of SAXParser.iterate((function* () {
        yield "<feed>"; yield "<item>on"; yield "e</item>"; yield "</feed>";
    })()))
        out2.push(ev.event);
    eq(out2.join(","), "open,open,text,close,close", "iterate over a generator");

    async function* src() { yield "<r>"; await 0; yield "<c/></r>"; }
    const out3 = [];
    for await (const ev of SAXParser.iterate(src())) out3.push(ev.event);
    eq(out3.join(","), "open,open,close", "iterate over an async generator");

    const offs = [];
    for await (const ev of SAXParser.iterate(["<a>", "x</a>"])) offs.push(ev.offset);
    eqj(offs, [0, 3, 4], "offsets are stream-absolute across chunks");

    const kept = [];
    for await (const ev of SAXParser.iterate(["<a>&foo;</a>"], { entities: "keep" }))
        if (ev.event === "text") kept.push(ev.text);
    eqj(kept, ["&foo;"], "iterate honors entities: keep");

    try { for await (const ev of SAXParser.iterate(["<a>"])) void ev; assert(false, "iterate malformed"); }
    catch (e) { eq(e.message, "SAXParser: unclosed element", "iterate rejects at the end"); }

    let n2 = 0;
    for await (const ev of SAXParser.iterate(["<a><b/><b/></a>"])) { n2++; if (n2 === 2) break; }
    eq(n2, 2, "break out of iterate");

    throws(() => SAXParser.iterate(null), "iterate requires a source");
    throws(() => SAXParser.iterate(), "iterate requires a source");

    const doc = "<a><b>t</b><c d=\"1\"/></a>";
    const names = [];
    for await (const ev of SAXParser.iterate([doc]))
        if (ev.event === "open") names.push(ev.name);
    const tree = XMLParse(doc);
    eqj(names, ["a", "b", "c"], "open events in tree order");
    eq(tree.children[1].attrs.d, "1", "tree agrees");

    if (fails === 0) print("test_xml_pull: all " + n + " tests passed");
    else { print("test_xml_pull: " + fails + " of " + n + " FAILED"); std.exit(1); }
})().catch((e) => { print("test_xml_pull: threw", e, e && e.stack); std.exit(1); });
