// The XML tree and SAX builders construct their results through pre-shaped
// objects. The results must be indistinguishable from plain objects: same own
// keys in the same order, every property writable/enumerable/configurable.
import { suite } from "../lib/kit.js";
import { XMLParse, SAXParser } from "dyna:xml";

const t = suite("xml_shapes");
const plain = (o) => Reflect.ownKeys(o).map((k) => { const d = Object.getOwnPropertyDescriptor(o, k); return k + ":" + (d.writable && d.enumerable && d.configurable && !d.get && !d.set ? "wec" : "OTHER"); }).join(",");

// 256 bytes is where the tree builder starts using a pre-shaped element.
for (const pad of [0, 150, 255 - 34, 256 - 34, 257 - 34, 5000]) {
    const doc = "<r><i a='1'>t</i><j/>" + " ".repeat(pad) + "<k>u</k></r>";
    t.test("element objects are plain at " + doc.length + " bytes", (a) => {
        const r = XMLParse(doc);
        a.eq(plain(r), "name:wec,attrs:wec,children:wec");
        a.eq(Object.getPrototypeOf(r), Object.prototype);
        a.ok(Array.isArray(r.children));
        a.eq(JSON.stringify(r), '{"name":"r","attrs":{},"children":[{"name":"i","attrs":{"a":"1"},"children":["t"]},{"name":"j","attrs":{},"children":[]},{"name":"k","attrs":{},"children":["u"]}]}');
        r.extra = 1; delete r.name; r.children.push("x");
        a.eq(plain(r), "attrs:wec,children:wec,extra:wec", "the result can be reshaped like any object");
    });
}

t.test("close tags are matched against the right open element at every depth", (a) => {
    const open = (n) => Array.from({ length: n }, (_, i) => "<e" + i + ">").join("");
    const close = (n) => Array.from({ length: n }, (_, i) => "</e" + (n - 1 - i) + ">").join("");
    for (const depth of [1, 2, 17, 200, 254]) {
        let node = XMLParse(open(depth) + "x" + close(depth));
        for (let i = 0; i < depth - 1; i++) node = node.children[0];
        a.eq(node.name, "e" + (depth - 1), "depth " + depth);
        a.deep(node.children, ["x"]);
    }
    a.throws(() => XMLParse("<a><b></a></b>"), /<\/a> closes <b>/);
    a.throws(() => XMLParse("<a><b><c></c></a></b>"), /<\/a> closes <b>/, "the name stack was popped after </c>");
    a.throws(() => XMLParse("<aa><a></aa></a>"), /<\/aa> closes <a>/, "a prefix of the open name is not a match");
    a.throws(() => XMLParse("<a><ab></a></ab>"), /<\/a> closes <ab>/, "nor is the open name's prefix");
    a.throws(() => XMLParse(open(300)), /nesting exceeds/);
    a.eq(XMLParse("<a><b/><b></b><c><b/></c></a>").children.length, 3);
});

t.test("a long element name survives buffer growth in the open-name stack", (a) => {
    const long = "n".repeat(5000);
    const r = XMLParse("<a><" + long + "><b>t</b></" + long + "></a>");
    a.eq(r.children[0].name.length, 5000);
    a.throws(() => XMLParse("<a><" + long + "></" + long + "x></a>"), SyntaxError, "a longer close name is a mismatch");
    a.throws(() => XMLParse("<a><" + long + "></" + long.slice(1) + "></a>"), SyntaxError, "a shorter close name is a mismatch");
});

t.test("pull events are plain objects with the documented keys per kind", (a) => {
    const p = new SAXParser();
    p.write("<?pi data?><a x='1'>t<![CDATA[c]]><!--m--><b/></a>");
    p.end();
    const got = [];
    for (let e; (e = p.next()); ) { got.push(e.event + "{" + plain(e) + "}"); a.eq(Object.getPrototypeOf(e), Object.prototype); }
    a.deep(got, [
        "pi{event:wec,offset:wec,name:wec,text:wec}",
        "open{event:wec,offset:wec,name:wec,attrs:wec}",
        "text{event:wec,offset:wec,text:wec}",
        "cdata{event:wec,offset:wec,text:wec}",
        "comment{event:wec,offset:wec,text:wec}",
        "open{event:wec,offset:wec,name:wec,attrs:wec}",
        "close{event:wec,offset:wec,name:wec}",
        "close{event:wec,offset:wec,name:wec}",
    ]);
});

t.test("a push parser delivers through handlers and next() hands out nothing", (a) => {
    const seen = [];
    const p = new SAXParser({ onOpen: (n, at) => seen.push(["o", n, at]), onText: (s) => seen.push(["t", s]), onClose: (n) => seen.push(["c", n]) });
    p.write("<a k='v'>x<b/></a>");
    a.eq(p.next(), null);
    p.end();
    a.deep(seen, [["o", "a", { k: "v" }], ["t", "x"], ["o", "b", {}], ["c", "b"], ["c", "a"]]);
});

await t.run();
