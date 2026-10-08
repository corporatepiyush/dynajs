import { Matcher, MultiMatcher, Levenshtein, JaroWinkler, DamerauLevenshtein } from "dyna:matcher";

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

{
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
}

{
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
}

{
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
}

{
    const mm = new MultiMatcher(["日", "本", "x"]);
    eqJson(mm.allIn("café 日本x"),
           [{ index: 0, at: 5 }, { index: 1, at: 6 }, { index: 2, at: 7 }],
           "every hit in CODE UNITS");
}

{
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
}

{
    eq("foobar".trimPrefix("foo"), "bar", "trimPrefix");
    eq("foobar".trimPrefix("bar"), "foobar", "trimPrefix that does not match");
    eq("foobar".trimPrefix(""), "foobar", "trimPrefix with an empty affix");
    eq("foobar".trimSuffix("bar"), "foo", "trimSuffix");
    eq("foobar".trimSuffix("foo"), "foobar", "trimSuffix that does not match");
    eq("aaa".trimPrefix("aaaa"), "aaa", "an affix longer than the string");

    eq("xxhelloyy".trimChars("xy"), "hello", "trimChars");
    eq("hello".trimChars(""), "hello", "trimChars with an empty set");
    eq("xxx".trimChars("x"), "", "trimChars consuming everything");
    eq("café".trimChars("é"), "caf", "trimChars on a multi-byte character");

    eq("hello".containsAny("xyz"), false, "containsAny miss");
    eq("hello".containsAny("le"), true, "containsAny hit");
    eq("hello".containsAny(""), false, "containsAny with an empty set");
    eq("hello".indexOfAny("le"), 1, "indexOfAny finds the earliest position");
    eq("hello".indexOfAny("xyz"), -1, "indexOfAny miss");
    eq("café 日本".indexOfAny("日"), 5, "indexOfAny in CODE UNITS (bytes: 6)");

    eqJson("aaaa".indexOfAll("aa"), [0, 1, 2], "indexOfAll counts OVERLAPS");
    eqJson("abc".indexOfAll(""), [], "indexOfAll with an empty needle");
    eqJson("abcabc".indexOfAll("abc"), [0, 3], "indexOfAll");
    eqJson("a😀b😀c".indexOfAll("😀"), [1, 4], "indexOfAll in CODE UNITS (bytes: 1,6)");

    eq("HeLLo".equalsIgnoreCase("hello"), true, "equalsIgnoreCase");
    eq("a".equalsIgnoreCase("b"), false, "equalsIgnoreCase miss");
    eq("abc".equalsIgnoreCase("ab"), false, "different lengths");
    eq("CAFÉ".equalsIgnoreCase("café"), false,
       "non-ASCII is NOT folded -- use toLowerCase() for the locale answer");

    eq("a".compareBytes("b"), -1, "compareBytes less");
    eq("b".compareBytes("a"), 1, "compareBytes greater");
    eq("a".compareBytes("a"), 0, "compareBytes equal");
    eq("ab".compareBytes("a"), 1, "a prefix sorts first");
    eq("".compareBytes(""), 0, "two empty strings");
    eq("�".compareBytes("\u{10000}"), -1,
       "U+FFFD sorts before U+10000 in code point order");
    eq("�" < "\u{10000}", false,
       "and JS `<` disagrees -- which is the whole point of this method");

    eqJson("a:b:c".splitN(":", 2), ["a", "b:c"], "splitN KEEPS the remainder");
    eqJson("a:b:c".split(":", 2), ["a", "b"], "...unlike split, which drops it");
    eqJson("a:b:c".splitN(":"), ["a", "b", "c"], "splitN with no limit");
    eqJson("a:b:c".splitN(":", -1), ["a", "b", "c"], "a negative limit");
    eqJson("a:b:c".splitN(":", 0), [], "a zero limit");
    eqJson("a:b:c".splitN(":", 1), ["a:b:c"], "a limit of one");
    eqJson("a:b:c".splitN(":", 99), ["a", "b", "c"], "a limit past the end");
    eqJson("abc".splitN("", 2), ["abc"], "an empty separator splits nothing");
    eqJson("".splitN(":", 2), [""], "an empty string");
    eqJson("日:本:語".splitN(":", 2), ["日", "本:語"], "multi-byte parts");
}

{
    eq(String.prototype.trimPrefix.call(123, "1"), "23", "on a number");
    eq(String.prototype.indexOfAny.call({ toString: () => "abc" }, "c"), 2,
       "on an object with toString");
}

{
    eq(new Matcher("aa").countIn("aaaa"), 3, "normal overlap counting unchanged");
    eqJson(new Matcher("aa").allIn("aaaa"), [0, 1, 2], "normal allIn unchanged");

    {
        const dense = "a".repeat(2e6);
        let ms = Date.now();
        eq(new Matcher("aa").countIn(dense), 1999999,
           "2e6 overlapping matches are counted, not refused");
        ms = Date.now() - ms;
        assert(ms < 1000, "the dense count is a fast linear pass (" + ms + " ms)");
        let log = "2026-09-16 WARN the thing failed; the retry worked\n";
        log = log.repeat(Math.ceil(1048576 / log.length));
        assert(new Matcher("the").countIn(log) > 40000,
               "counting a common word in a 1 MB log works");
    }

    const hostile = "ab".repeat(500);
    let ms = Date.now(), threw = false, msg = null;
    try { new Matcher(hostile).countIn("ab".repeat(2e6)); }
    catch (e) { threw = true; msg = String(e); }
    ms = Date.now() - ms;
    assert(threw, "a ~4e9-step periodic count is refused (got: " + msg + ")");
    assert(/budget/.test(msg), "the refusal names the scan budget (got: " + msg + ")");
    assert(ms < 2000, "the budget trips quickly (took " + ms + " ms)");

    ms = Date.now(); threw = false;
    try { new Matcher(hostile).allIn("ab".repeat(2e6)); }
    catch (e) { threw = true; }
    ms = Date.now() - ms;
    assert(threw, "the same allIn is refused");
    assert(ms < 2000, "allIn trips quickly too (took " + ms + " ms)");
}

{
    const chunk = "x".repeat(17000);
    const under = [];
    for (let i = 0; i < 500; i++) under.push(chunk);
    eq(new MultiMatcher(under).size, 500, "a pattern set under the byte cap builds");

    const over = [];
    for (let i = 0; i < 1200; i++) over.push(chunk);
    throws(() => new MultiMatcher(over), "additions past the byte cap throw");
}

{
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
}

{
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
}

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

{
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
}

console.log("test_matcher.js: " + n + " assertions passed");
