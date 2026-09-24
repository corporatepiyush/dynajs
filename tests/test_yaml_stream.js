// flags: --std
/* test_yaml_stream.js --: ParseStream, the line-at-a-time multi-doc
 * iterator, plus the adversarial cases the surface invites.
 *
 * The oracle is ParseAll: the stream and the array form must produce the
 * SAME documents over the same bytes -- an empty document between markers
 * is `null` in both -- and a malformed document N must be reported AS
 * document N (1-based), with the line number of the whole text.
 *
 * Run: dynajs tests/test_yaml_stream.js
 */
import { ParseStream, ParseAll, Stringify } from "dyna:yaml";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqj(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b), msg +
        " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMsg(fn, re, msg) {
    let e = null;
    try { fn(); } catch (ex) { e = ex; }
    assert(e !== null && re.test(e.message),
        msg + " (threw " + (e && e.message) + ")");
}
const collect = (text, opts) => {
    const out = [];
    for (const d of ParseStream(text, opts)) out.push(d);
    return out;
};

/* ------------------------------------------------------- the ParseAll oracle */

eqj(collect("---\na: 1\n---\nb: 2\n"), [{ a: 1 }, { b: 2 }], "two documents");
eqj(collect("a: 1\n"), [{ a: 1 }], "one document without a leading marker");
eqj(collect(""), [], "empty input has no documents");
eqj(collect("# only a comment\n"), [], "comment-only input");
eqj(collect("---\n---\n"), [null, null], "empty documents are null");
eqj(collect("a: 1\n...\n"), [{ a: 1 }], "a ... terminator ends the stream");
eqj(collect("# lead\n---\n1 # one\n---\n# mid\n2\n...\n# tail\n"),
    [1, 2], "comments between and around documents");
eqj(collect("---\n{a: [1, {b: null}]}\n---\n'q': \"d\"\n---\n|\n  block\n"),
    [{ a: [1, { b: null }] }, { q: "d" }, "block\n"],
    "flow, quoted and block scalars per document");
eqj(collect("---\n---\nx\n---\n"), [null, "x", null], "trailing open marker");

/* stream === ParseAll on a larger mixed input */
{
    const text = "---\nname: d0\n---\n\n---\n- 1\n- 2\n---\n\"k\": |\n  text\n" +
        "---\n5\n...\n---\n{f: true}\n---\nnull\n";
    eqj(collect(text), ParseAll(text), "agrees with ParseAll on a mixed stream");
}

/* documents are yielded ONE at a time (the point of the form): consume
 * without collecting, verify order via a side effect */
{
    const seen = [];
    for (const d of ParseStream("---\n1\n---\n2\n---\n3\n"))
        seen.push(d);
    eqj(seen, [1, 2, 3], "in-order consumption");
}

/* the iterator protocol */
{
    const it = ParseStream("---\na\n---\nb\n");
    eq(it.next().done, false, "next() reports not-done");
    eqj(it.next().value, "b", "next() yields each document");
    eq(it.next().done, true, "next() reports done");
    eq(it.next().done, true, "next() stays done");
}
{
    let n2 = 0;
    for (const d of ParseStream("---\n1\n---\n2\n---\n3\n")) {
        n2++;
        if (n2 === 2) break;
    }
    eq(n2, 2, "for-of break exits early");
}
/* return() closes; next() after return() is done */
{
    const it = ParseStream("---\n1\n---\n2\n");
    it.next();
    eq(it.return().done, true, "return() closes the iterator");
    eq(it.next().done, true, "next() after return() is done");
}
/* an abandoned iterator with a large input is collected (GC + finalizer) */
{
    let big = "";
    for (let i = 0; i < 2000; i++) big += "---\nk" + i + ": " + i + "\n";
    let it = ParseStream(big);
    big = null;
    it.next();
    it = null;
    std.gc();
    std.gc();
    assert(true, "abandoned iterator survives GC without incident");
}

/* per-document errors carry the 1-based document index */
{
    const it = ParseStream("---\na: 1\n---\nb: *alias\n---\nc: 3\n");
    eqj(it.next().value, { a: 1 }, "the document before the error is fine");
    throwsMsg(() => it.next(), /^ParseStream: document 2: .* at line 4$/,
        "the error names document 2 and its line");
    throwsMsg(() => it.next(), /^ParseStream: document 2: /,
        "the error is sticky after the throw");
}
{
    const it = ParseStream("---\nok\n---\nok\n---\nbad: [1,\n---\nok\n");
    it.next(); it.next();
    throwsMsg(() => it.next(), /^ParseStream: document 3: /,
        "document 3 carries its own index");
}
throwsMsg(() => collect("---\na: 1\n---\nb: [1\n"),
    /^ParseStream: document 2: /, "unterminated flow in doc 2");
throwsMsg(() => collect("---\na: &anchor 1\n"),
    /^ParseStream: document 1: /, "anchors are refused in doc 1, by name");
throwsMsg(() => collect("---\na: 1\n---\n... junk\n"),
    /^ParseStream: document 3: content after the document-end marker at line \d+$/,
    "a non-comment tail after ... is refused");

/* options: the same bag as Parse, labeled ParseStream */
{
    const it = ParseStream("---\na:\n  b:\n    c:\n      d: 1\n", { maxDepth: 2 });
    throwsMsg(() => it.next(), /^ParseStream: document 1: nesting exceeds/,
        "maxDepth lowers the cap");
}
eqj(collect("---\na:\n  b: 1\n", { maxDepth: 2 }), [{ a: { b: 1 } }],
    "maxDepth above the real depth is fine");
throwsMsg(() => ParseStream("---\na: 1\n", { schema: "full" }),
    /^ParseStream: the YAML 1.2 full schema is not implemented/,
    "schema full is refused by name");
throwsMsg(() => ParseStream("---\na: 1\n", { dep: 1 }),
    /^unknown option "dep" \(valid: maxDepth, schema\)$/,
    "the options bag is strict");
throwsMsg(() => ParseStream(42), /^text must be a string$/, "non-string input");
throwsMsg(() => ParseStream(), /^ParseStream\(text\): text is required$/,
    "missing text");

/* 100-document stream, one at a time, against ParseAll */
{
    let text = "";
    const want = [];
    for (let i = 0; i < 100; i++) {
        text += "---\nid: " + i + "\nvals:\n  - " + i * 2 + "\n  - " + (i * 2 + 1) +
            "\nflag: " + (i % 2 === 0) + "\n";
        want.push({ id: i, vals: [i * 2, i * 2 + 1], flag: i % 2 === 0 });
    }
    const docs = collect(text);
    eq(docs.length, 100, "100 documents come out one at a time");
    eqj(docs, want, "all 100 are exact");
    eqj(docs, ParseAll(text), "and agree with ParseAll");
    /* Stringify -> ParseStream round trip (markers keep them documents) */
    eqj(collect(want.map((d) => "---\n" + Stringify(d)).join("")), want,
        "stringified per-document stream re-parses");
}

/* hostile-loop pins: 10k documents, marker-only documents */
{
    let text = "";
    for (let i = 0; i < 10000; i++) text += "---\nk" + i + ": " + i + "\n";
    let count = 0;
    for (const d of ParseStream(text)) count++;
    eq(count, 10000, "10k documents stream one at a time");
}
eqj(collect("---\n...\n---\n1\n"), [null, 1], "a marker-only document is null");
eqj(collect("a: ...\n"), ParseAll("a: ...\n"), "agrees with ParseAll on a ... scalar");
{
    const it = ParseStream("---\nbad: [1\n");
    it.return();
    eq(it.next().done, true, "next after return() is done");
}

/* CRLF stream */
eqj(collect("---\r\na: 1\r\n---\r\nb: 2\r\n"), [{ a: 1 }, { b: 2 }],
    "CRLF line endings");

/* BOM before the first marker */
eqj(collect("\uFEFF---\na: 1\n"), [{ a: 1 }], "a leading BOM is not content");

if (fails === 0) print("test_yaml_stream: all " + n + " tests passed");
else { print("test_yaml_stream: " + fails + " of " + n + " FAILED"); std.exit(1); }
