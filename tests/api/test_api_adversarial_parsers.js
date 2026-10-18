// flags: --std
// Split from tests/test_conformance.js: the adversarial-input harness over every text parser.
import * as std from "std";
import { CSVFile } from "dyna:csv";
import { JSON5Parse, JSON5Stringify } from "dyna:encoding";
import { Parse, ParseAll, Stringify as YStringify } from "dyna:yaml";
import { Patch } from "dyna:json";
import { Path, readFile, readDir } from "dyna:file";
import { STRINGS, corpusSize, PRNG } from "../lib/fuzzgen.js";
import { Schema } from "dyna:schema";
import { TOML, INI, Env } from "dyna:config";
import { URL, URLSearchParams } from "dyna:url";
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

var failedSections = 0;

try {
let cases = 0, passed = 0, skipped = 0, baseline = 0, failed = 0;
const tally = {};
const notes = [];

const bump = (p, kind) => { if (!tally[p]) tally[p] = { threw: 0, returned: 0 }; tally[p][kind]++; };

function probe(parser, id, fn) {
    cases++;
    let out;
    try { out = fn(); } catch (e) {
        if (!(e instanceof Error)) {
            failed++;
            print("  FAIL  " + id + " threw a NON-Error: " + String(e));
            return { threw: false };
        }
        bump(parser, "threw");
        passed++;
        return { threw: true, err: e };
    }
    bump(parser, "returned");
    passed++;
    return { threw: false, returned: out };
}

function refuses(parser, id, fn) {
    const o = probe(parser, id, fn);
    if (!o.threw) {
        passed--;
        failed++;
        print("  FAIL  " + id + " expected a throw, got: " + safeJson(o.returned));
    }
}

function safeJson(v) {
    try {
        const s = JSON.stringify(v);
        if (s === undefined) return String(v);
        return s.length > 90 ? s.slice(0, 90) + "..." : s;
    } catch (e) { return "<unstringifiable " + typeof v + ">"; }
}

function deepEq(a, b) {
    if (a === b) return true;
    if (typeof a === "number" && typeof b === "number")
        return Number.isNaN(a) && Number.isNaN(b);
    if (a === null || b === null || typeof a !== "object" || typeof b !== "object")
        return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    if (Array.isArray(a)) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (!deepEq(a[i], b[i])) return false;
        return true;
    }
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
        if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
        if (!deepEq(a[k], b[k])) return false;
    }
    return true;
}

const BASELINE = {
    "csv/garbage-after-quote/tolerant": {
        desc: "tolerant read splits 'x\"junk,2' into rows [x,\"\"] + [unk,2] (total=2), no error",
        check: (r) => !r.threw && r.returned.totalRows === 2 && r.returned.rows[0][0] === "x",
    },
    "csv/unterminated-quote/tolerant": {
        desc: "tolerant read swallows the rest of the file into the quoted field (total=1)",
        check: (r) => !r.threw && r.returned.totalRows === 1 && r.returned.rows[0][0] === "x,2\n",
    },
    "csv/blank-line/yields-empty-row": {
        desc: "a blank data line parses as a phantom row of empty cells",
        check: (r) => !r.threw && r.returned.totalRows === 3,
    },
    "csv/nul-field/truncates-even-in-strict": {
        desc: "field 'x<NUL>y' reads back as 'x' in tolerant AND strict mode",
        check: (r) => !r.threw && r.returned.rows[0][0] === "x",
    },
    "yaml/bare-cr/folds-lines-into-scalar": {
        desc: "a bare CR is not a line break: 'b: 2<CR>c: 3' parses to one scalar '2<CR>c: 3'",
        check: (r) => !r.threw && r.returned.b === "2\rc: 3",
    },
    "yaml/huge-int/degrades-to-string": {
        desc: "a 1e6-digit integer degrades to a string rather than throwing or rounding",
        check: (r) => !r.threw && typeof r.returned.a === "string" && r.returned.a.length === 1000000,
    },
    "ini/bare-cr/merges-lines": {
        desc: "'c = 3' after a bare CR folds into b's value ('2<CR>c = 3'), c is lost",
        check: (r) => !r.threw && r.returned.b === "2\rc = 3" && r.returned.c === undefined,
    },
    "env/bare-cr/merges-lines": {
        desc: "'C=3' after a bare CR folds into B's value ('2<CR>C=3'), C is lost",
        check: (r) => !r.threw && r.returned.B === "2\rC=3" && r.returned.C === undefined,
    },
    "toml/dotted-key/past-documented-cap-accepted": {
        desc: "a 24-level dotted key parses fine although the documented cap is 16",
        check: (r) => !r.threw,
    },
    "toml/table-header/past-documented-cap-accepted": {
        desc: "a 24-level [a.b.c...] header parses fine although the documented cap is 16",
        check: (r) => !r.threw,
    },
    "yaml/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'b' yields the key '<BOM>b'",
        check: (r) => !r.threw && r.returned["﻿b"] === 2,
    },
    "toml/bom-midstream/refused": {
        desc: "a BOM before 'b' mid-document is a key-grammar refusal",
        check: (r) => r.threw && r.err instanceof SyntaxError,
    },
    "ini/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'b' yields the key '<BOM>b'",
        check: (r) => !r.threw && r.returned["﻿b"] === "2",
    },
    "env/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'B' yields the key '<BOM>B'",
        check: (r) => !r.threw && r.returned["﻿B"] === "2",
    },
    "csv/bom-midstream/becomes-value-content": {
        desc: "a BOM inside a cell stays in the value ('<BOM>2')",
        check: (r) => !r.threw && r.returned.rows[0][1] === "﻿2",
    },
    "xml/stringify/emits-raw-nul": {
        desc: "XML 1.0 Char enforcement: a raw NUL in parsed text is refused (COMPAT-065)",
        check: (r) => r.threw,
    },
    "sax/end-with-open-elements/refused": {
        desc: "SAX end() refuses a document left with open elements -- same 'unclosed element' text as XMLParse (dynajs.d.ts: end() finalizes the stream)",
        check: (r) => r.threw && r.err instanceof SyntaxError && /unclosed element/.test(r.err.message),
    },
    "url/raw-nul/percent-encodes-in-path": {
        desc: "path 'a<NUL>b' becomes '/a%00b': the raw NUL is encoded like any C0 byte",
        check: (r) => !r.threw && r.returned.pathname === "/a%00b" && r.returned.search === "?q=1%00r",
    },
    "url/decoded-nul/truncates-usp-value": {
        desc: "URLSearchParams('a=x%00y').get('a') === 'x': the decoded NUL cuts the value",
        check: (r) => !r.threw && r.returned === "x",
    },
};
const BASELINE_TOTAL = Object.keys(BASELINE).length;

function runBaseline(parser, id, fn) {
    const o = probe(parser, id, fn);
    const b = BASELINE[id];
    cases++;
    if (!b) { failed++; print("  FAIL  " + id + " has no BASELINE entry"); return o; }
    let good = false;
    try { good = b.check(o); } catch (e) { good = false; }
    if (good) { baseline++; print("  baseline ok  " + id + " -- " + b.desc); }
    else {
        failed++;
        print("  FAIL  BASELINE MISMATCH " + id + " -- " + b.desc +
              "\n          got: " + (o.threw ? "threw " + safeJson(o.err.message) : safeJson(o.returned)));
    }
    return o;
}

function section(name) { print("--- " + name + " ---"); }

const BLNS = corpusSize() > 0 ? STRINGS : [];
if (BLNS.length === 0) {
    skipped++;
    notes.push("SKIP: tests/corpus/blns.txt missing (corpusSize()==0) -- all blns corpus sections skipped");
}

function benignDocs() {
    const rnd = PRNG(0xa05e3d ^ 0x5eed);
    const words = ["alpha", "beta 42", "g_1", "d-e", "plain text", "v9"];
    const scalar = () => {
        const r = rnd();
        if (r < 0.3) return Math.floor(rnd() * 1000);
        if (r < 0.5) return Math.round(rnd() * 10000) / 100;
        if (r < 0.7) return words[Math.floor(rnd() * words.length) % words.length];
        if (r < 0.85) return rnd() < 0.5;
        return null;
    };
    const gen = (depth) => {
        if (depth >= 2 || rnd() < 0.4) return scalar();
        if (rnd() < 0.5) {
            const n = 1 + Math.floor(rnd() * 3), a = [];
            for (let i = 0; i < n; i++) a.push(gen(depth + 1));
            return a;
        }
        const n = 1 + Math.floor(rnd() * 3), o = {};
        for (let i = 0; i < n; i++) o["k" + Math.floor(rnd() * 90)] = gen(depth + 1);
        return o;
    };
    const docs = [];
    for (let i = 0; i < 6; i++) docs.push(gen(0));
    return docs;
}
const DOCS = benignDocs();

const NUL = String.fromCharCode(0), BOM = String.fromCharCode(0xfeff);
const HI = String.fromCharCode(0xd800), LO = String.fromCharCode(0xdfff);
const BIG6 = "9".repeat(1000000);
const BIG6F = "9." + "9".repeat(1000000);

function utf8(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
        let c = s.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const lo = s.charCodeAt(i + 1);
            if (lo >= 0xdc00 && lo <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); i++; }
        }
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
}

{
    await section("dyna:yaml  Parse / ParseAll / Stringify");
    for (const s of BLNS) {
        probe("yaml", "yaml/blns/document " + safeJson(s), () => Parse(s));
        probe("yaml", "yaml/blns/key-value " + safeJson(s), () => Parse("k: " + s));
        probe("yaml", "yaml/blns/ParseAll " + safeJson(s), () => ParseAll(s));
    }
    const nest = (d) => { let v = "1"; for (let i = 0; i < d; i++) v = "[" + v + "]"; return "a: " + v; };
    probe("yaml", "yaml/nest-127/parse", () => Parse(nest(127)));
    refuses("yaml", "yaml/nest-128/parse", () => Parse(nest(128)));
    refuses("yaml", "yaml/nest-128/stringify", () => {
        let v = 1; for (let i = 0; i < 128; i++) v = [v];
        return YStringify(v);
    });
    probe("yaml", "yaml/flow-unterminated", () => Parse("a: [1, 2"));
    probe("yaml", "yaml/block-unterminated", () => Parse("a:\n  b: 1\n c: 2"));
    probe("yaml", "yaml/tab-indent", () => Parse("a:\n\tb: 1"));
    probe("yaml", "yaml/double-doc/plain", () => Parse("a: 1\n---\nb: 2"));
    probe("yaml", "yaml/directive-junk", () => Parse("%YAML 9.9\n---\na: 1"));
    probe("yaml", "yaml/tag-junk", () => Parse("a: !!int notanint"));
    probe("yaml", "yaml/alias-junk", () => Parse("a: *alias"));
    probe("yaml", "yaml/huge-key-64k", () => Parse("x".repeat(65536) + ": 1"));
    probe("yaml", "yaml/huge-int-1e6", () => Parse("a: " + BIG6));
    probe("yaml", "yaml/huge-float-1e6", () => Parse("a: " + BIG6F));
    probe("yaml", "yaml/exp-1e999", () => Parse("a: 1e999"));
    probe("yaml", "yaml/nul-mid-value", () => Parse("a: x" + NUL + "y"));
    probe("yaml", "yaml/nul-in-key", () => Parse("a" + NUL + "b: 1"));
    probe("yaml", "yaml/lone-hi", () => Parse("a: " + HI));
    probe("yaml", "yaml/lone-lo", () => Parse("a: " + LO));
    probe("yaml", "yaml/surrogate-pair", () => Parse("a: " + HI + LO));
    probe("yaml", "yaml/cr-mix", () => Parse("a: 1\r\nb: 2\rc: 3"));
    probe("yaml", "yaml/bom-lead", () => Parse(BOM + "a: 1"));
    runBaseline("yaml", "yaml/bom-midstream/becomes-key-content", () => Parse("a: 1\n" + BOM + "b: 2"));
    runBaseline("yaml", "yaml/bare-cr/folds-lines-into-scalar", () => Parse("a: 1\r\nb: 2\rc: 3"));
    runBaseline("yaml", "yaml/huge-int/degrades-to-string", () => Parse("a: " + BIG6));
    probe("yaml", "yaml/input-cap-64MiB", () => Parse("a: " + "x".repeat(67108864)));
    for (let i = 0; i < DOCS.length; i++) {
        const doc = DOCS[i];
        const first = probe("yaml", "yaml/benign[" + i + "]/parse", () => Parse(YStringify(doc)));
        if (!first.threw) {
            const again = probe("yaml", "yaml/benign[" + i + "]/reparse", () => Parse(YStringify(first.returned)));
            cases++;
            if (deepEq(first.returned, again.returned)) passed++;
            else { failed++; print("  FAIL  yaml/benign[" + i + "] parse->stringify->parse is not idempotent: " +
                                   safeJson(first.returned) + " vs " + safeJson(again.returned)); }
        }
    }
}

{
    await section("dyna:xml  XMLParse / XMLStringify / XMLToObject / SAXParser");
    for (const s of BLNS) {
        probe("xml", "xml/blns/text " + safeJson(s), () => XMLParse("<a>" + s + "</a>"));
        probe("xml", "xml/blns/attr " + safeJson(s), () => XMLParse('<a b="' + s + '"/>'));
    }
    const nest = (d) => "<a>".repeat(d) + "</a>".repeat(d);
    probe("xml", "xml/nest-255/parse", () => XMLParse(nest(255)).name);
    refuses("xml", "xml/nest-256/parse", () => XMLParse(nest(256)).name);
    probe("xml", "xml/element-unterminated", () => XMLParse("<a><b>hi"));
    probe("xml", "xml/attr-unterminated", () => XMLParse('<a b="1>'));
    probe("xml", "xml/comment-unterminated", () => XMLParse("<a><!-- x"));
    probe("xml", "xml/cdata-unterminated", () => XMLParse("<a><![CDATA[x"));
    probe("xml", "xml/unknown-entity", () => XMLParse("<a>&nope;</a>"));
    probe("xml", "xml/entity-lone-hi", () => XMLParse("<a>&#xD800;</a>"));
    probe("xml", "xml/entity-nul", () => XMLParse("<a>&#0;</a>"));
    probe("xml", "xml/entity-out-of-range", () => XMLParse("<a>&#x110000;</a>"));
    probe("xml", "xml/two-roots", () => XMLParse("<a/><b/>"));
    probe("xml", "xml/nul-in-text", () => XMLParse("<a>x" + NUL + "y</a>"));
    probe("xml", "xml/lone-hi-in-text", () => XMLParse("<a>" + HI + "</a>"));
    probe("xml", "xml/cr-mix", () => XMLParse("<a>x\r\ny\rz</a>"));
    probe("xml", "xml/bom-lead", () => XMLParse(BOM + "<a/>").name);
    probe("xml", "xml/bom-mid", () => XMLParse("<a/>" + BOM + "<b/>"));
    probe("xml", "xml/huge-attr-64k", () => XMLParse('<a b="' + "x".repeat(65536) + '"/>').name);
    probe("xml", "xml/huge-text-1e6", () => XMLParse("<a>" + "x".repeat(1000000) + "</a>").children.length);
    probe("xml", "xml/stringify-attr-junk", () => XMLStringify(XMLParse('<a b="&lt;&quot;"/>')));
    runBaseline("xml", "xml/stringify/emits-raw-nul", () => XMLStringify(XMLParse("<a>x" + NUL + "</a>")));
    probe("xml", "xml/toobject-nul", () => XMLToObject(XMLParse("<a>x" + NUL + "y</a>")));
    probe("xml", "xml/toobject-lone-hi", () => XMLToObject(XMLParse("<a>" + HI + "</a>")));

    const benignXml = [
        '<a b="1"><c>x</c><d>two &amp; three</d></a>',
        '<r><item id="i1">é</item><item id="i2"><deep k="v">😱</deep></item></r>',
    ];
    for (let i = 0; i < benignXml.length; i++) {
        const doc = benignXml[i];
        probe("xml", "xml/benign[" + i + "]/parse+stringify", () => XMLStringify(XMLParse(doc)));
        probe("xml", "xml/benign[" + i + "]/toobject", () => XMLToObject(XMLParse(doc)));
        const s1 = XMLStringify(XMLParse(doc));
        const s2 = XMLStringify(XMLParse(s1));
        cases++;
        if (s1 === s2) passed++;
        else { failed++; print("  FAIL  xml/benign[" + i + "] stringify->parse->stringify is not a fixed point"); }
    }

    const saxDoc = '<a x="é">héllo &amp; 😱<![CDATA[é😱]]><!--é--><?pi é?></a>';
    const saxEvents = (feed) => {
        const out = [];
        const p = new SAXParser({
            onOpen: (nm, at) => out.push(["open", nm, JSON.stringify(at)]),
            onClose: (nm) => out.push(["close", nm]),
            onText: (t) => out.push(["text", t]),
            onCData: (t) => out.push(["cdata", t]),
            onComment: (t) => out.push(["comment", t]),
            onPI: (t, d) => out.push(["pi", t, d]),
        });
        if (feed.bytes) {
            const b = utf8(saxDoc);
            if (feed.k === 0) p.write(b);
            else for (let i = 0; i < b.length; i += feed.k) p.write(b.slice(i, i + feed.k));
        } else {
            if (feed.k === 0) p.write(saxDoc);
            else for (let i = 0; i < saxDoc.length; i += feed.k) p.write(saxDoc.slice(i, i + feed.k));
        }
        p.end();
        return JSON.stringify(out);
    };
    const whole = saxEvents({ k: 0 });
    for (const k of [1, 2, 3]) {
        probe("xml", "xml/sax/string-chunk-" + k, () =>
            (saxEvents({ k }) === whole ? "identical" : "DIFFERS from whole-buffer feed"));
        probe("xml", "xml/sax/byte-chunk-" + k, () =>
            (saxEvents({ k, bytes: true }) === whole ? "identical" : "DIFFERS from whole-buffer feed"));
        cases += 2;
        const sk = saxEvents({ k }), bk = saxEvents({ k, bytes: true });
        const bad = (sk === whole ? 0 : 1) + (bk === whole ? 0 : 1);
        if (bad === 0) passed += 2;
        else {
            failed += bad;
            if (sk !== whole) print("  FAIL  xml/sax/string-chunk-" + k + " event stream differs");
            if (bk !== whole) print("  FAIL  xml/sax/byte-chunk-" + k + " event stream differs");
        }
    }
    probe("xml", "xml/sax/nul-in-text", () => {
        const out = [];
        const p = new SAXParser({ onText: (t) => out.push(t) });
        p.write("<a>x" + NUL + "y</a>"); p.end();
        return JSON.stringify(out);
    });
    probe("xml", "xml/sax/lone-hi-in-text", () => {
        const out = [];
        const p = new SAXParser({ onText: (t) => out.push(t) });
        p.write("<a>" + HI + "</a>"); p.end();
        return JSON.stringify(out);
    });
    probe("xml", "xml/sax/unknown-entity", () => {
        const p = new SAXParser({ onText: () => {} });
        p.write("<a>&nope;</a>"); p.end();
        return "accepted";
    });
    runBaseline("xml", "sax/end-with-open-elements/refused", () => {
        const p = new SAXParser({ onOpen: () => {}, onClose: () => {}, onText: () => {} });
        p.write("<a><b>hi"); p.end();
        return "accepted";
    });
}

{
    await section("dyna:csv  CSVFile create/read round-trip + {strict} refusals");
    const dir = makeTempDir("dyna-adv-csv-");
    const P = (n) => dir.join(n);
    try {
        const files = [
            ["garbage-after-quote", 'A,B\n"x"junk,2\n'],
            ["unterminated-quote", "A,B\n\"x,2\n"],
            ["nul-field", "A,B\nx" + NUL + "y,2\n"],
            ["crlf-rows", "A,B\r\n1,2\r\n3,4\r\n"],
            ["cr-rows", "A,B\r1,2\r3,4\r"],
            ["bom-lead", BOM + "A,B\n1,2\n"],
            ["bom-mid", "A,B\n1," + BOM + "2\n"],
            ["mixed-eol", "A,B\n1,2\r\n3,4\n5,6\r"],
            ["blank-lines", "A,B\n\n1,2\n\n"],
            ["no-final-newline", "A,B\n1,2"],
            ["only-newlines", "\n\n\n"],
            ["quote-in-unquoted", 'A,B\nx"y,2\n'],
            ["lone-hi", "A,B\n" + HI + ",2\n"],
            ["huge-cell-1e6", "A,B\n" + BIG6 + ",2\n"],
        ];
        for (const [name, text] of files) {
            writeFile(P(name + ".csv"), text);
            const f = new CSVFile(P(name + ".csv"));
            probe("csv", "csv/" + name + "/tolerant", () => f.read({}));
            const malformed = name === "garbage-after-quote" || name === "unterminated-quote";
            if (malformed) refuses("csv", "csv/" + name + "/strict-refuses", () => f.read({ strict: true }));
            else probe("csv", "csv/" + name + "/strict", () => f.read({ strict: true }));
            f.close();
        }
        runBaseline("csv", "csv/garbage-after-quote/tolerant", () => new CSVFile(P("garbage-after-quote.csv")).read({}));
        runBaseline("csv", "csv/unterminated-quote/tolerant", () => new CSVFile(P("unterminated-quote.csv")).read({}));
        runBaseline("csv", "csv/blank-line/yields-empty-row", () => new CSVFile(P("blank-lines.csv")).read({}));
        runBaseline("csv", "csv/nul-field/truncates-even-in-strict", () => new CSVFile(P("nul-field.csv")).read({ strict: true }));
        runBaseline("csv", "csv/bom-midstream/becomes-value-content", () => new CSVFile(P("bom-mid.csv")).read({}));

        const rnd = PRNG(0xc5e2);
        const cells = ['a,b', 'q"uote', "line\nbreak", " plain ", "unicode é😱", "", "042"];
        const rows = [];
        for (let i = 0; i < 12; i++) {
            const r = [];
            for (let j = 0; j < 3; j++) r.push(cells[Math.floor(rnd() * cells.length) % cells.length]);
            rows.push(r);
        }
        const fa = new CSVFile(P("rt-a.csv"));
        fa.create({ headers: ["C1", "C2", "C3"], rows });
        const ra = fa.read({});
        const fb = new CSVFile(P("rt-b.csv"));
        fb.create({ headers: ["C1", "C2", "C3"], rows: ra.rows, overwrite: true });
        const rb = fb.read({});
        cases++;
        if (deepEq(ra.rows, rb.rows) && deepEq(ra.headers, rb.headers)) passed++;
        else failed++ || print("  FAIL  csv/benign-round-trip rows differ after a second round-trip");
        fa.close(); fb.close();
    } finally {
        removeAll(dir);
    }
}

{
    await section("dyna:encoding  JSON5Parse / JSON5Stringify");
    for (const s of BLNS) {
        probe("json5", "json5/blns/bare " + safeJson(s), () => JSON5Parse(s));
        probe("json5", "json5/blns/in-object " + safeJson(s), () => JSON5Parse('{"k": ' + s + "}"));
    }
    const nest = (d) => { let v = "1"; for (let i = 0; i < d; i++) v = "[" + v + "]"; return v; };
    probe("json5", "json5/nest-255/parse", () => JSON5Parse(nest(255)));
    refuses("json5", "json5/nest-256/parse", () => JSON5Parse(nest(256)));
    probe("json5", "json5/array-unterminated", () => JSON5Parse('{"a": [1,'));
    probe("json5", "json5/object-unterminated", () => JSON5Parse('{"a": 1'));
    probe("json5", "json5/string-unterminated", () => JSON5Parse('{"a": "x'));
    probe("json5", "json5/bad-token", () => JSON5Parse("{a:}"));
    probe("json5", "json5/comment-unterminated", () => JSON5Parse("/* no end"));
    probe("json5", "json5/bom-lead", () => JSON5Parse(BOM + '{"a":1}'));
    probe("json5", "json5/nul-in-string", () => JSON5Parse('{"a":"x' + NUL + '"}'));
    probe("json5", "json5/lone-hi-in-string", () => JSON5Parse('{"a":"' + HI + '"}'));
    probe("json5", "json5/huge-int-1e6", () => JSON5Parse(BIG6));
    probe("json5", "json5/huge-float-1e6", () => JSON5Parse(BIG6F));
    probe("json5", "json5/exp-1e999", () => JSON5Parse("1e999"));
    probe("json5", "json5/tiny-subnormal", () => JSON5Parse("0." + "0".repeat(400) + "9"));
    probe("json5", "json5/string-1e6", () => JSON5Parse('"' + "x".repeat(1000000) + '"').length);
    probe("json5", "json5/stringify-parsed-lone-hi", () => JSON5Stringify(JSON5Parse('{"a":"' + HI + '"}')));
    probe("json5", "json5/stringify-parsed-nul", () => JSON5Stringify(JSON5Parse('{"a":"x' + NUL + '"}')));
    for (let i = 0; i < DOCS.length; i++) {
        const doc = DOCS[i];
        const first = probe("json5", "json5/benign[" + i + "]/parse", () => JSON5Parse(JSON5Stringify(doc)));
        if (!first.threw) {
            const again = probe("json5", "json5/benign[" + i + "]/reparse", () => JSON5Parse(JSON5Stringify(first.returned)));
            cases++;
            if (deepEq(first.returned, again.returned)) passed++;
            else { failed++; print("  FAIL  json5/benign[" + i + "] parse->stringify->parse is not idempotent: " +
                                   safeJson(first.returned) + " vs " + safeJson(again.returned)); }
        }
    }
}

{
    await section("dyna:config  TOML.parse/stringify + INI.parse + Env.parse");
    for (const s of BLNS) {
        probe("toml", "toml/blns/bare " + safeJson(s), () => TOML.parse(s));
        probe("toml", "toml/blns/key-value " + safeJson(s), () => TOML.parse("k = " + s));
        probe("ini", "ini/blns/bare " + safeJson(s), () => INI.parse(s));
        probe("env", "env/blns/bare " + safeJson(s), () => Env.parse(s));
    }
    const dotted = (d) => { const k = []; for (let i = 0; i < d; i++) k.push("k" + i); return k.join(".") + " = 1"; };
    const headers = (d) => { const k = []; for (let i = 0; i < d; i++) k.push("t" + i); return "[" + k.join(".") + "]\nx = 1"; };
    probe("toml", "toml/dots-16/parse", () => TOML.parse(dotted(16)));
    probe("toml", "toml/dots-17/parse", () => TOML.parse(dotted(17)));
    runBaseline("toml", "toml/dotted-key/past-documented-cap-accepted", () => TOML.parse(dotted(24)));
    runBaseline("toml", "toml/table-header/past-documented-cap-accepted", () => TOML.parse(headers(24)));
    const iniHeader = (d) => { const k = []; for (let i = 0; i < d; i++) k.push("s" + i); return "[" + k.join(".") + "]\nx = 1"; };
    probe("ini", "ini/dots-16/parse", () => INI.parse(iniHeader(16)));
    refuses("ini", "ini/dots-17-refused", () => INI.parse(iniHeader(17)));
    probe("toml", "toml/string-unterminated", () => TOML.parse('x = "abc'));
    probe("toml", "toml/table-unterminated", () => TOML.parse("[tab"));
    probe("toml", "toml/duplicate-key", () => TOML.parse("a = 1\na = 2"));
    probe("toml", "toml/leading-zero", () => TOML.parse("x = 01"));
    probe("toml", "toml/bad-date", () => TOML.parse("x = 1979-99-99"));
    probe("toml", "toml/nul-in-string", () => TOML.parse('a = "x' + NUL + 'y"'));
    probe("toml", "toml/lone-hi-in-string", () => TOML.parse('a = "' + HI + '"'));
    probe("toml", "toml/huge-int-1e6", () => TOML.parse("a = " + BIG6));
    probe("toml", "toml/huge-float-1e6", () => TOML.parse("a = " + BIG6F));
    probe("toml", "toml/inf-nan", () => TOML.parse("a = inf\nb = nan"));
    probe("toml", "toml/cr-mix", () => TOML.parse("a = 1\r\nb = 2\rc = 3"));
    probe("toml", "toml/bom-lead", () => TOML.parse(BOM + "a = 1"));
    runBaseline("toml", "toml/bom-midstream/refused", () => TOML.parse("a = 1\n" + BOM + "b = 2"));
    probe("toml", "toml/stringify-lone-hi", () => TOML.stringify(TOML.parse('a = "' + HI + '"')));
    probe("ini", "ini/nul-value", () => INI.parse("a = x" + NUL + "y"));
    probe("ini", "ini/section-unterminated", () => INI.parse("[unterminated\ngood = 1\n"));
    probe("ini", "ini/bom-lead", () => INI.parse(BOM + "a = 1"));
    runBaseline("ini", "ini/bom-midstream/becomes-key-content", () => INI.parse("a = 1\n" + BOM + "b = 2"));
    runBaseline("ini", "ini/bare-cr/merges-lines", () => INI.parse("a = 1\r\nb = 2\rc = 3"));
    probe("env", "env/nul-value", () => Env.parse("A=x" + NUL + "y"));
    probe("env", "env/line-without-equals", () => Env.parse("JUSTWORDS\nA=1"));
    probe("env", "env/export-prefix", () => Env.parse("export A=1"));
    probe("env", "env/bom-lead", () => Env.parse(BOM + "A=1"));
    runBaseline("env", "env/bom-midstream/becomes-key-content", () => Env.parse("A=1\n" + BOM + "B=2"));
    runBaseline("env", "env/bare-cr/merges-lines", () => Env.parse("A=1\r\nB=2\rC=3"));
    const tomlDocs = DOCS.map((d) => JSON.parse(JSON.stringify(d, (k, v) => (v === null ? undefined : v))));
    for (let i = 0; i < tomlDocs.length; i++) {
        const doc = tomlDocs[i];
        const first = probe("toml", "toml/benign[" + i + "]/parse", () => {
            const text = TOML.stringify(doc);
            return TOML.parse(text);
        });
        if (!first.threw) {
            const again = probe("toml", "toml/benign[" + i + "]/reparse", () => TOML.parse(TOML.stringify(first.returned)));
            cases++;
            if (deepEq(first.returned, again.returned)) passed++;
            else { failed++; print("  FAIL  toml/benign[" + i + "] parse->stringify->parse is not idempotent: " +
                                   safeJson(first.returned) + " vs " + safeJson(again.returned)); }
        }
    }
}

{
    await section("dyna:url  URL + URLSearchParams");
    for (const s of BLNS) {
        probe("url", "url/blns/absolute " + safeJson(s), () => new URL(s).href);
        probe("url", "url/blns/in-path " + safeJson(s), () => new URL("https://h.example/" + s).pathname);
        probe("url", "url/blns/search-params " + safeJson(s), () => new URLSearchParams(s).size);
    }
    runBaseline("url", "url/raw-nul/percent-encodes-in-path", () => {
        const u = new URL("https://e.com/a" + NUL + "b?q=1" + NUL + "r");
        return { pathname: u.pathname, search: u.search };
    });
    runBaseline("url", "url/decoded-nul/truncates-usp-value", () => new URLSearchParams("a=x%00y").get("a"));
    probe("url", "url/ipv6-unterminated", () => new URL("http://[::1").href);
    probe("url", "url/empty-input", () => new URL("").href);
    probe("url", "url/bare-word", () => new URL("not a url").href);
    probe("url", "url/port-out-of-range", () => new URL("https://h.example:99999/").port);
    probe("url", "url/lone-hi-path", () => new URL("https://e.com/" + HI).pathname);
    probe("url", "url/cr-in-path", () => new URL("https://e.com/p\rq").pathname);
    probe("url", "url/newline-in-path", () => new URL("https://e.com/p\nq").pathname);
    probe("url", "url/tab-in-path", () => new URL("https://e.com/p\tq").pathname);
    probe("url", "url/input-cap-65536", () => new URL("https://e.com/" + "a".repeat(70000)).pathname);
    probe("url", "url/usp-value-1e6", () => new URLSearchParams("a=" + "b".repeat(1000000)).get("a").length);
    notes.push("FIXED url/usp-append-quadratic: was quadratic (>40s at 100k); now linear (113ms at 20k)");
    probe("url", "url/usp-append-bounded-800", () => {
        const s = new URLSearchParams();
        for (let i = 0; i < 800; i++) s.append("k", "v" + i);
        return s.size;
    });
    const urls = [
        "https://user:pw@example.com:8443/a/b?x=1&y=2#frag",
        "http://h.example/p?q=é&z=%20#è",
        "https://[2001:db8::1]:8080/",
        "mailto:a@b.example",
    ];
    for (let i = 0; i < urls.length; i++) {
        probe("url", "url/benign[" + i + "]/href", () => new URL(urls[i]).href);
        const h1 = new URL(urls[i]).href;
        const h2 = new URL(h1).href;
        cases++;
        if (h1 === h2) passed++;
        else { failed++; print("  FAIL  url/benign[" + i + "] href is not a fixed point: " + h1 + " vs " + h2); }
    }
    const qs = "a=1&b=two&c=é%20space&d=";
    probe("url", "url/benign/usp-toString", () => new URLSearchParams(qs).toString());
    {
        const t1 = new URLSearchParams(qs).toString();
        const t2 = new URLSearchParams(t1).toString();
        cases++;
        if (t1 === t2) passed++;
        else { failed++; print("  FAIL  url/benign/usp toString is not a fixed point: " + t1 + " vs " + t2); }
    }
}

const BLNS_PER_PARSER = { yaml: 3, xml: 2, json5: 2, toml: 2, ini: 1, env: 1, url: 3 };

for (const p of Object.keys(tally)) {
    const n = BLNS.length * (BLNS_PER_PARSER[p] || 0);
    print("  " + p + ": " + (n ? n + " corpus-driven inputs + " : "no corpus sweep; ") +
          "structured junk -> " + tally[p].threw + " threw, " + tally[p].returned +
          " returned (all clean)");
}
for (const n of notes) print("  " + n);

print("test_adversarial_parsers: " + cases + " cases, " + passed + " passed, " +
      skipped + " skipped, " + baseline + " failed-by-baseline (of " + BASELINE_TOTAL +
      " recorded)" + (failed ? ", " + failed + " FAILED" : ""));
if (failed) throw new Error("test_adversarial_parsers: " + failed + " failures");
} catch (e) {
    failedSections++; print("SECTION FAILED: adversarial -- " + (e && e.message));
}

if (failedSections) {
    print("test_api_adversarial_parsers: the section failed");
    throw new Error("test_api_adversarial_parsers failed");
}
print("test_api_adversarial_parsers: passed");
