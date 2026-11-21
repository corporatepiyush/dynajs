// covers: dyna:matcher.DamerauLevenshtein
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_damerau_levenshtein");
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

__kitS.section("(got", () => {
    function near(got, want, msg) {
        assert(Math.abs(got - want) < 1e-12,
               msg + " (got " + got + ", want " + want + ")");
    }
    function ok(c, msg) { assert(c, msg); }
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    near(JaroWinkler("MARTHA", "MARHTA"), 0.9611111111111111, "JW MARTHA/MARHTA");
    near(JaroWinkler("DWAYNE", "DUANE"), 0.84, "JW DWAYNE/DUANE");
    near(JaroWinkler("DIXON", "DICKSONX"), 0.8133333333333332, "JW DIXON/DICKSONX");
    near(JaroWinkler("CRATE", "TRACE"), 0.7333333333333333, "JW CRATE/TRACE (no prefix boost)");
    near(JaroWinkler("abcd", "dcba"), 0.5, "JW abcd/dcba (raw jaro, no prefix: no boost)");
    eq(JaroWinkler("identical", "identical"), 1, "JW identical");
    eq(JaroWinkler("", ""), 1, "JW empty/empty");
    eq(JaroWinkler("a", ""), 0, "JW one empty");
    ok(JaroWinkler("abc", "xyz") >= 0 && JaroWinkler("abc", "xyz") <= 1, "JW in range");
    const astral = "\u{1F600}";
    ok(JaroWinkler(astral + "ab", astral + "ab") === 1, "JW astral identical");
    near(JaroWinkler("\u00e9\u00e9xx", "\u00e9\u00e9yy"), 2 / 3, "JW code points (jaro 2/3, below the boost threshold)");
    eq(DamerauLevenshtein("abcd", "acbd"), 1, "OSA counts one transposition");
    eq(DamerauLevenshtein("ca", "abc"), 3, "OSA: ca/abc is 3, NOT unrestricted-2");
    eq(DamerauLevenshtein("kitten", "sitting"), 3, "OSA kitten/sitting");
    eq(DamerauLevenshtein("", "abc"), 3, "OSA empty vs abc");
    eq(DamerauLevenshtein("", ""), 0, "OSA empty vs empty");
    eq(DamerauLevenshtein("abc", "abc"), 0, "OSA identical");
    eq(DamerauLevenshtein("\u00e9\u00e9xx", "\u00e9\u00e9"), 2, "OSA counts code points");
    eq(DamerauLevenshtein("abc", "abcx", { max: 2 }), 1, "OSA exact within max");
    eq(DamerauLevenshtein("abc", "abcdx", { max: 1 }), 2, "OSA overshoot answers max+1");
    eq(DamerauLevenshtein("abc", "zxy", { max: 1 }), 2, "OSA overshoot (length+sub)");
    eq(DamerauLevenshtein("abcd", "acbd", { max: 1 }), 1, "OSA banded finds the transposition");
    eq(DamerauLevenshtein("kitten", "sitting", { max: 2 }), 3, "OSA banded overshoot");
    throws(() => DamerauLevenshtein("a", "b", { max: -1 }), "OSA negative max throws");
    throws(() => DamerauLevenshtein("a"), "OSA needs two strings");
    let msg = "";
    try { DamerauLevenshtein("a", "b", { max: -1 }); } catch (e) { msg = String(e); }
    ok(msg.includes("DamerauLevenshtein"), "error names DamerauLevenshtein: " + msg);
    for (const [a, b] of [["sunday", "saturday"], ["abcdef", "azced"], ["flaw", "lawn"]]) {
        eq(DamerauLevenshtein(a, b), Levenshtein(a, b), "OSA == Lev on " + a + "/" + b);
    }
    ok(DamerauLevenshtein("form", "from") < Levenshtein("form", "from"),
       "OSA beats Lev on a pure transposition");
});
__kitS.end();

console.log("test_matcher.js: " + n + " assertions passed");
