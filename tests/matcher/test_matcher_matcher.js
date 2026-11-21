// covers: dyna:matcher.Matcher
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_matcher_matcher");
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

__kitS.section("firstIn miss", () => {
    const m = new Matcher("ss");
    eq(m.firstIn("mississippi"), 2, "firstIn");
    eq(m.firstIn("nope"), -1, "firstIn miss");
    eq(m.countIn("mississippi"), 2, "countIn");
    eq(m.test("mississippi"), true, "test hit");
    eq(m.test("nope"), false, "test miss");
    eq(m.length, 2, "length");
    eq(m.algo, "kmp", "default algo");
    eqJson(new Matcher("aa").allIn("aaaa"), [0, 1, 2], "allIn counts OVERLAPS");
    eqJson(new Matcher("x").allIn("abc"), [], "allIn with no match");

    const e = new Matcher("");
    eq(e.firstIn("abc"), 0, "empty pattern is at 0");
    eq(e.test("abc"), true, "empty pattern tests true");
    eq(e.countIn("abc"), 0, "empty pattern counts 0, not length+1");
    eqJson(e.allIn("abc"), [], "empty pattern enumerates nothing");
    for (const a of ["bmh", "boyer-moore"]) {
        const ea = new Matcher("", { algo: a });
        eq(ea.firstIn(""), 0, "empty pattern firstIn on empty text [" + a + "]");
        eq(ea.countIn("abc"), 0, "empty pattern countIn [" + a + "]");
        eqJson(ea.allIn("abc"), [], "empty pattern allIn [" + a + "]");
    }

    eq(new Matcher("x", { algo: "bmh" }).algo, "bmh", "algo option round-trips");
    // FIX2: the d.ts union lists "boyer-moore" as its own accepted value, so .algo echoes the
    // spelling that was passed (it previously normalized to "bmh", a value outside the union).
    eq(new Matcher("x", { algo: "boyer-moore" }).algo, "boyer-moore", "the long spelling echoes");
    throws(() => new Matcher("x", { algo: "nope" }), "an unknown algo");
    throws(() => Matcher("x"), "calling without new");

    eq(m.replaceAllIn("mississippi", "S"), "miSiSippi", "replaceAllIn");
    eq(new Matcher("a").replaceAllIn("aaa", "XY"), "XYXYXY", "a longer replacement");
    eq(new Matcher("aa").replaceAllIn("aaaa", "-"), "--", "non-overlapping");
    eq(new Matcher("x").replaceAllIn("abc", "-"), "abc", "no match, unchanged");
    eq(new Matcher("").replaceAllIn("abc", "-"), "abc", "empty pattern replaces nothing");
    eq(new Matcher("b").replaceAllIn("abc", ""), "ac", "an empty replacement deletes");
});

__kitS.section("café 日本", () => {
    const text = "café 日本";
    eq(text.length, 7, "the text is 7 code units");
    eq(new TextEncoder().encode(text).length, 12, "and 12 UTF-8 bytes");
    eq(new Matcher("日").firstIn(text), 5, "firstIn reports CODE UNITS (bytes: 6)");
    eq(new Matcher("本").firstIn(text), 6, "and so does the next character");
    eq(text.indexOf("日"), 5, "String.prototype.indexOf agrees");

    const emoji = "a😀b";
    eq(emoji.length, 4, "one emoji is two code units");
    eq(new Matcher("b").firstIn(emoji), 3, "after a surrogate pair (bytes: 5)");
    eq(emoji.indexOf("b"), 3, "String.prototype.indexOf agrees");

    eqJson(new Matcher("😀").allIn("😀x😀"), [0, 3], "allIn across surrogate pairs");
    eq(new Matcher("é").countIn("ééé"), 3, "countIn on multi-byte characters");

    eq(new Matcher("😀").length, 2, "length counts CODE UNITS (bytes: 4)");
    eq(new Matcher("日").length, 1, "length of a BMP character (bytes: 3)");
    eq(new Matcher("é").length, 1, "length of a Latin-1 character (bytes: 2)");
    eq(new Matcher("a😀b").length, 4, "mixed ASCII + astral");

    {
        const ALGO_SPELLINGS = ["kmp", "bmh", "boyer-moore"];
        const pairs = [["abababab", "ab"], ["mississippi", "ss"],
                       ["aaaaaaaaab", "aaab"], ["日本語日本語", "本語"]];
        for (const [text, pat] of pairs) {
            const res = ALGO_SPELLINGS.map(a => {
                const m = new Matcher(pat, { algo: a });
                return [m.firstIn(text), m.countIn(text), m.allIn(text).join(",")];
            });
            eqJson(res[0], res[1], "kmp == bmh on " + JSON.stringify([text, pat]));
            eqJson(res[0], res[2], "kmp == boyer-moore on " + JSON.stringify([text, pat]));
        }
    }
});

__kitS.section("the quick brown fox jumps over the lazy dog, quickly", () => {
    const pats = ["the", "quick", "fox", "he", "ck", "e"];
    const text = "the quick brown fox jumps over the lazy dog, quickly";
    const single = [];
    pats.forEach((p, i) => {
        for (const at of new Matcher(p).allIn(text)) single.push({ index: i, at });
    });
    single.sort((a, b) => a.at - b.at || a.index - b.index);
    const multi = new MultiMatcher(pats).allIn(text)
                      .sort((a, b) => a.at - b.at || a.index - b.index);
    eqJson(multi, single, "the automaton finds exactly what N searches find");
    eq(new MultiMatcher(pats).countIn(text),
       pats.reduce((acc, p) => acc + new Matcher(p).countIn(text), 0),
       "and counts the same");
});

__kitS.section("normal overlap counting unchanged", () => {
    eq(new Matcher("aa").countIn("aaaa"), 3, "normal overlap counting unchanged");
    eqJson(new Matcher("aa").allIn("aaaa"), [0, 1, 2], "normal allIn unchanged");

    {
        const dense = "a".repeat(2e6);
        let ms = cpuNow();
        eq(new Matcher("aa").countIn(dense), 1999999,
           "2e6 overlapping matches are counted, not refused");
        ms = cpuNow() - ms;
        perfCheck(assert, ms < 1000 * hostScale(), "the dense count is a fast linear pass (" + ms + " ms)");
        let log = "2026-09-16 WARN the thing failed; the retry worked\n";
        log = log.repeat(Math.ceil(1048576 / log.length));
        assert(new Matcher("the").countIn(log) > 40000,
               "counting a common word in a 1 MB log works");
    }

    const hostile = "ab".repeat(500);
    let ms = cpuNow(), threw = false, msg = null;
    try { new Matcher(hostile).countIn("ab".repeat(2e6)); }
    catch (e) { threw = true; msg = String(e); }
    ms = cpuNow() - ms;
    assert(threw, "a ~4e9-step periodic count is refused (got: " + msg + ")");
    assert(/budget/.test(msg), "the refusal names the scan budget (got: " + msg + ")");
    perfCheck(assert, ms < 2000 * hostScale(), "the budget trips quickly (took " + ms + " ms)");

    ms = cpuNow(); threw = false;
    try { new Matcher(hostile).allIn("ab".repeat(2e6)); }
    catch (e) { threw = true; }
    ms = cpuNow() - ms;
    assert(threw, "the same allIn is refused");
    perfCheck(assert, ms < 2000 * hostScale(), "allIn trips quickly too (took " + ms + " ms)");
});

__kitS.section("differential firstIn", () => {
    let seed = 0xC0FFEE >>> 0;
    function rnd() {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        return seed / 4294967296;
    }
    const ALPHA = "ab";
    function rstr(maxlen) {
        let s = "";
        const len = 1 + Math.floor(rnd() * maxlen);
        for (let i = 0; i < len; i++)
            s += ALPHA[Math.floor(rnd() * ALPHA.length)];
        return s;
    }
    for (let it = 0; it < 2000; it++) {
        const hay = rstr(24), needle = rstr(4);
        const m = new Matcher(needle);
        const want = hay.indexOf(needle);
        eq(m.firstIn(hay), want,
           "differential firstIn(" + JSON.stringify(hay) + ", " +
           JSON.stringify(needle) + ")");
        const positions = [];
        for (let at = hay.indexOf(needle); at !== -1; at = hay.indexOf(needle, at + 1))
            positions.push(at);
        eqJson(m.allIn(hay), positions, "differential allIn");
        eq(m.countIn(hay), positions.length, "differential countIn");
    }
});

__kitS.section("firstIn basic", () => {
    const m = new Matcher("bar");
    eq(m.firstIn("foobar"), 3, "firstIn basic");
    eq(m.firstIn("foobar", 0), 3, "fromIndex 0");
    eq(m.firstIn("foobar", 3), 3, "fromIndex at the match");
    eq(m.firstIn("foobar", 4), -1, "fromIndex past the match");
    eq(m.firstIn("foobar", -5), 3, "negative fromIndex clamps to 0");
    eq(m.firstIn("foobar", 100), -1, "fromIndex beyond the text");
    eq(m.firstIn("barbarbar", 1), 3, "skips the earlier match");
    eq(m.firstIn("barbarbar", 2), 3, "skips from mid-pattern");
    eq(m.firstIn("foobar", NaN), 3, "NaN -> 0");
    eq(m.firstIn("foobar", undefined), 3, "undefined fromIndex = absent");
    const e = new Matcher("");
    eq(e.firstIn("abc"), 0, "empty pattern at 0");
    eq(e.firstIn("abc", 2), 2, "empty pattern: min(fromIndex, length)");
    eq(e.firstIn("abc", 5), 3, "empty pattern clamps to length");
    const w = new Matcher("\u00e9");
    eq(w.firstIn("a\u00e9b\u00e9"), 1, "wide first hit");
    eq(w.firstIn("a\u00e9b\u00e9", 2), 3, "wide fromIndex skips a code unit");
    eq(w.firstIn("a\u00e9b\u00e9", 99), -1, "wide fromIndex past the end");
    const wa = new Matcher("X");
    eq(wa.firstIn("\u00e9X\u00e9X", 1), 1, "ascii pattern over wide text");
    eq(wa.firstIn("\u00e9X\u00e9X", 2), 3, "ascii pattern over wide text, from 2");
});
__kitS.end();

console.log("test_matcher.js: " + n + " assertions passed");
