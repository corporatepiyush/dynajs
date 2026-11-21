// covers: dyna:xml.SAXParser
// The XML tree and SAX builders construct their results through pre-shaped
// objects. The results must be indistinguishable from plain objects: same own
// keys in the same order, every property writable/enumerable/configurable.
import { suite } from "../lib/kit.js";
import { XMLParse, SAXParser } from "dyna:xml";

const t = suite("xml_shapes");
const plain = (o) => Reflect.ownKeys(o).map((k) => { const d = Object.getOwnPropertyDescriptor(o, k); return k + ":" + (d.writable && d.enumerable && d.configurable && !d.get && !d.set ? "wec" : "OTHER"); }).join(",");

// 256 bytes is where the tree builder starts using a pre-shaped element.

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
