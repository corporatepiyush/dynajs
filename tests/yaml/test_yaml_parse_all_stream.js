// flags: --std
// covers: dyna:yaml.ParseAll
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_all_stream");
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

__kitS.section("name: d0 - 1 - 2 \"k\": | text", () => {
    const text = "---\nname: d0\n---\n\n---\n- 1\n- 2\n---\n\"k\": |\n  text\n" +
        "---\n5\n...\n---\n{f: true}\n---\nnull\n";
    eqj(collect(text), ParseAll(text), "agrees with ParseAll on a mixed stream");
});

__kitS.section("--- id:", () => {
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
    eqj(collect(want.map((d) => "---\n" + Stringify(d)).join("")), want,
        "stringified per-document stream re-parses");
});
__kitS.end();

eqj(collect("a: ...\n"), ParseAll("a: ...\n"), "agrees with ParseAll on a ... scalar");

if (fails === 0) print("test_yaml_stream: all " + n + " tests passed");
else { print("test_yaml_stream: " + fails + " of " + n + " FAILED"); std.exit(1); }
