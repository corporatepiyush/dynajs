// covers: c:src/modules/xml/xml-writer.c
// XmlWriter.open writes "<name attrs>" straight into the output buffer (the
// attributes are still escaped into a scratch buffer first, so attribute
// getters run before the writer changes), keeps open element names in one
// arena addressed by offsets instead of one malloc per element, and grows the
// indent string geometrically instead of a realloc per open. close/finish pop
// the arena. Modperf round 2026-11-15, 10K open/text/close: 6.82 ms -> 4.78 ms.
// Oracle: a JS model of the documented writer (indent before every open tag
// but the first and before every close tag, attribute values escaped & < > ",
// text escaped & < >, finish closes everything and is idempotent).
import { suite } from "../lib/kit.js";
import * as xml from "dyna:xml";

const t = suite("test_xml_xml_writer_names_arena");

class Ref {
    constructor(ind) {
        this.ind = ind;
        this.out = "";
        this.st = [];
    }
    nl() {
        if (this.ind) this.out += "\n" + " ".repeat(this.st.length * this.ind);
    }
    open(n, a) {
        if (this.out.length && this.ind) this.nl();
        let s = "<" + n;
        for (const [k, v] of Object.entries(a || {})) s += " " + k + '="' + v.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;") + '"';
        this.out += s + ">";
        this.st.push(n);
    }
    text(s) {
        this.out += s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }
    close() {
        const n = this.st.pop();
        this.nl();
        this.out += "</" + n + ">";
    }
    finish() {
        while (this.st.length) this.close();
        return this.out;
    }
}

let seed = 5;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);
const NAMES = ["a", "item", "x-y", "n.1", "é", "long" + "n".repeat(300)];

t.test("random open/text/close programs match the JS model", ({ eq }) => {
    for (let prog = 0; prog < 200; prog++) {
        const ind = [0, 1, 2, 16][prog & 3];
        const w = new xml.XmlWriter({ indent: ind }), r = new Ref(ind);
        let depth = 0;
        for (let step = 0; step < 120; step++) {
            const op = rnd() % 4;
            if (op < 2 && depth < 40) {
                const n = NAMES[rnd() % NAMES.length];
                const a = rnd() % 2 ? { k: "v" + step, q: '<&">' } : undefined;
                w.open(n, a);
                r.open(n, a);
                depth++;
            } else if (op === 2) {
                const s = "t" + step + (step & 1 ? " & <b>" : "");
                w.text(s);
                r.text(s);
            } else if (depth) {
                w.close();
                r.close();
                depth--;
            }
        }
        eq(w.toString(), r.out, "prog " + prog + " toString");
        eq(w.finish(), r.finish(), "prog " + prog + " finish");
        eq(w.finish(), r.out, "prog " + prog + " finish again");
    }
});

t.test("a rejected open leaves the document unchanged", ({ eq, throws }) => {
    const w = new xml.XmlWriter({ indent: 2 }), r = new Ref(2);
    w.open("root");
    r.open("root");
    throws(() => w.open("b", { "1bad": "x" }), undefined, "bad attribute name");
    throws(() => w.open("b", { ok: "\u0001" }), undefined, "illegal char");
    throws(() => w.open("9x"), undefined, "bad element name");
    throws(() => w.open("b", { get g() { throw new Error("getter"); } }), undefined, "getter");
    w.open("c", { k: "1" });
    r.open("c", { k: "1" });
    eq(w.finish(), r.finish(), "document after rejected opens");
    throws(() => new xml.XmlWriter().close(), undefined, "close with nothing open");
});

t.test("depth 5000 with indent round-trips through finish", ({ eq }) => {
    const w = new xml.XmlWriter({ indent: 1 }), r = new Ref(1);
    for (let i = 0; i < 5000; i++) {
        w.open("d" + (i % 7));
        r.open("d" + (i % 7));
    }
    eq(w.finish(), r.finish(), "deep");
});

await t.run();
