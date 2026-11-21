// covers: dyna:matcher.MultiMatcher
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_multi_matcher");
import { Matcher, MultiMatcher, Levenshtein, JaroWinkler, DamerauLevenshtein } from "dyna:matcher";
import { hostScale } from "../lib/hostscale.js";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function eqJson(got, want, msg) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b)
        throw new Error("assertion failed: " + msg + "\n  got:  " + a + "\n  want: " + b);
}
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

__kitS.section("the automaton has states", () => {
    const mm = new MultiMatcher(["he", "she", "his", "hers"]);
    eq(mm.size, 4, "size");
    assert(mm.states > 4, "the automaton has states (" + mm.states + ")");
    eqJson(mm.allIn("ushers"),
           [{ index: 1, at: 1 }, { index: 0, at: 2 }, { index: 3, at: 2 }],
           "the suffix-link case: she@1, he@2, hers@2");
    eq(mm.countIn("ushers"), 3, "countIn");
    eqJson(mm.firstIn("ushers"), { index: 1, at: 1 }, "firstIn stops at the first");
    eq(mm.test("ushers"), true, "test hit");
    eq(mm.test("xyz"), false, "test miss");
    eq(mm.firstIn("xyz"), null, "firstIn returns null on a miss");
    eq(mm.countIn("xyz"), 0, "countIn on a miss");
    eqJson(mm.allIn("xyz"), [], "allIn on a miss");

    const router = new MultiMatcher(["GET /api/", "POST /api/", "DELETE /"]);
    eqJson(router.firstIn("POST /api/users"), { index: 1, at: 0 }, "a router");
    eq(router.test("PATCH /api/users"), false, "an unrouted method");

    const pre = new MultiMatcher(["ab", "abc", "bc"]);
    eqJson(pre.allIn("abc"),
           [{ index: 0, at: 0 }, { index: 1, at: 0 }, { index: 2, at: 1 }],
           "prefix and suffix patterns all report");

    const dup = new MultiMatcher(["aa", "aa"]);
    eqJson(dup.allIn("aa"), [{ index: 0, at: 0 }], "a duplicate reports once");

    eqJson(new MultiMatcher(["aa"]).allIn("aaaa"),
           [{ index: 0, at: 0 }, { index: 0, at: 1 }, { index: 0, at: 2 }],
           "overlaps are reported");

    throws(() => new MultiMatcher([]), "an empty pattern list");
    throws(() => new MultiMatcher(["ok", ""]), "an empty pattern");
    throws(() => new MultiMatcher("not an array"), "a non-array");
    throws(() => MultiMatcher(["x"]), "calling without new");
});

__kitS.section("café 日本x", () => {
    const mm = new MultiMatcher(["日", "本", "x"]);
    eqJson(mm.allIn("café 日本x"),
           [{ index: 0, at: 5 }, { index: 1, at: 6 }, { index: 2, at: 7 }],
           "every hit in CODE UNITS");
});

__kitS.section("a pattern set under the byte cap builds", () => {
    const chunk = "x".repeat(17000);
    const under = [];
    for (let i = 0; i < 500; i++) under.push(chunk);
    eq(new MultiMatcher(under).size, 500, "a pattern set under the byte cap builds");

    const over = [];
    for (let i = 0; i < 1200; i++) over.push(chunk);
    throws(() => new MultiMatcher(over), "additions past the byte cap throw");
});
__kitS.end();

{
    function eqS(got, want, msg) {
        assert(got === want, msg + "\n  got:  " + got + "\n  want: " + want);
    }
    const mm = new MultiMatcher(["world", "hello"]);
    eqS(mm.replaceAllIn("hello world hello", "X"), "X X X", "all patterns replaced");
    const seen = [];
    const out = mm.replaceAllIn("hello world hello", (m, i) => {
        seen.push([m, i]);
        return "<" + i + ">";
    });
    eqS(out, "<1> <0> <1>", "callback output");
    assert(JSON.stringify(seen) === JSON.stringify([["hello", 1], ["world", 0], ["hello", 1]]),
           "callback args (match, patternIndex): " + JSON.stringify(seen));
    const mm2 = new MultiMatcher(["he", "she"]);
    eqS(mm2.replaceAllIn("ushers", "-"), "u-rs", "longest-first at one end: she wins over he");
    const mm3 = new MultiMatcher(["aa", "aba"]);
    assert(mm3.replaceAllIn("abaa", "_") === "_a",
           "non-overlapping leftmost: aba claims [0,3), overlapping aa skipped");
    eqS(mm.replaceAllIn("hello", () => 42), "42", "numeric callback result");
    eqS(mm2.replaceAllIn("she he", ""), " ", "empty replacement deletes");
    let called = 0;
    try {
        mm.replaceAllIn("hello world hello", () => { called++; throw new TypeError("boom"); });
        assert(false, "callback exception must propagate");
    } catch (e) {
        assert(String(e).includes("boom"), "the callback's error surfaces");
    }
    assert(called === 1, "scan stopped at the first callback error (called=" + called + ")");
    const mmW = new MultiMatcher(["\u00e9"]);
    eqS(mmW.replaceAllIn("a\u00e9b\u00e9c", "#"), "a#b#c", "wide text gaps preserved");
    throws(() => mm.replaceAllIn("x"), "replaceAllIn needs two arguments");
    const mm4 = new MultiMatcher(["p0", "p1", "p2"]);
    const idxs = new Set();
    mm4.replaceAllIn("p0 p1 p2", (m, i) => { idxs.add(i); return m; });
    assert(JSON.stringify([...idxs].sort()) === "[0,1,2]", "all three indices seen");
}

console.log("test_matcher.js: " + n + " assertions passed");
