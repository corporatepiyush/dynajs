/* test_matcher.js -- dyna:matcher (W2.2), and the nine String.prototype
 * methods that came with it (W2.1).
 *
 * Two things are being pinned, and the second is the one that matters:
 *
 *   1. The behaviour of `Matcher`, `MultiMatcher` and the nine methods.
 *   2. That every offset is a UTF-16 CODE UNIT. These functions used to report
 *      UTF-8 BYTE offsets, and for ASCII the two are identical -- so an
 *      ASCII-only test suite cannot tell the conventions apart and would have
 *      passed unchanged through the switch. Every offset assertion here uses
 *      text with a character above U+007F on the LEFT of the answer.
 *
 * The one-shot differential that established the equivalence over 4784 cases
 * against the byte-offset implementation; it
 * cannot live here, because the module it compared against is gone.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_matcher.js */

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

/* ============================================================ 1. Matcher */
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

    /* the empty pattern: at 0 and nowhere else worth enumerating.
     * EMPTY-NEEDLE CONVENTIONS, pinned deliberately (behavior unchanged):
     * Matcher.countIn("") === 0 and allIn("") === [], while dyna:bytes'
     * count(s, []) === n+1 (the byte-count convention, pinned in
     * tests/test_bytes.js "empty needle -> length+1"). The modules disagree
     * ON PURPOSE: dyna:bytes counts POSITIONS (an empty needle is at every
     * seam), Matcher reports only matches worth enumerating -- see the
     * empty-pattern branch of dyn_matcher_scan in src/dyna-matcher.c. */
    const e = new Matcher("");
    eq(e.firstIn("abc"), 0, "empty pattern is at 0");
    eq(e.test("abc"), true, "empty pattern tests true");
    eq(e.countIn("abc"), 0, "empty pattern counts 0, not length+1");
    eqJson(e.allIn("abc"), [], "empty pattern enumerates nothing");
    /* the conventions must hold under EVERY algo spelling */
    for (const a of ["bmh", "boyer-moore"]) {
        const ea = new Matcher("", { algo: a });
        eq(ea.firstIn(""), 0, "empty pattern firstIn on empty text [" + a + "]");
        eq(ea.countIn("abc"), 0, "empty pattern countIn [" + a + "]");
        eqJson(ea.allIn("abc"), [], "empty pattern allIn [" + a + "]");
    }

    eq(new Matcher("x", { algo: "bmh" }).algo, "bmh", "algo option round-trips");
    eq(new Matcher("x", { algo: "boyer-moore" }).algo, "bmh", "the long spelling");
    throws(() => new Matcher("x", { algo: "nope" }), "an unknown algo");
    throws(() => Matcher("x"), "calling without new");

    /* replaceAllIn is NON-overlapping: allIn's overlapping matches cannot all
     * be replaced once bytes start being removed */
    eq(m.replaceAllIn("mississippi", "S"), "miSiSippi", "replaceAllIn");
    eq(new Matcher("a").replaceAllIn("aaa", "XY"), "XYXYXY", "a longer replacement");
    eq(new Matcher("aa").replaceAllIn("aaaa", "-"), "--", "non-overlapping");
    eq(new Matcher("x").replaceAllIn("abc", "-"), "abc", "no match, unchanged");
    eq(new Matcher("").replaceAllIn("abc", "-"), "abc", "empty pattern replaces nothing");
    eq(new Matcher("b").replaceAllIn("abc", ""), "ac", "an empty replacement deletes");
}

/* ------------------------- the offsets are CODE UNITS, and this proves it */
{
    /* "café 日本" -- é is 2 UTF-8 bytes, 日 and 本 are 3 each. A byte-offset
     * implementation answers 6 for 日; a code-unit one answers 5. */
    const text = "café 日本";
    eq(text.length, 7, "the text is 7 code units");
    eq(new TextEncoder().encode(text).length, 12, "and 12 UTF-8 bytes");
    eq(new Matcher("日").firstIn(text), 5, "firstIn reports CODE UNITS (bytes: 6)");
    eq(new Matcher("本").firstIn(text), 6, "and so does the next character");
    eq(text.indexOf("日"), 5, "String.prototype.indexOf agrees");

    /* a non-BMP character is TWO code units, so everything after it shifts by 2 */
    const emoji = "a😀b";
    eq(emoji.length, 4, "one emoji is two code units");
    eq(new Matcher("b").firstIn(emoji), 3, "after a surrogate pair (bytes: 5)");
    eq(emoji.indexOf("b"), 3, "String.prototype.indexOf agrees");

    eqJson(new Matcher("😀").allIn("😀x😀"), [0, 3], "allIn across surrogate pairs");
    eq(new Matcher("é").countIn("ééé"), 3, "countIn on multi-byte characters");

    /* `length` is the pattern's CODE-UNIT length -- the same convention as
     * every offset this module reports, so one match spans `length` code
     * units. It used to report the UTF-8 BYTE length (4 for "😀"), which only
     * an ASCII-only test could miss. */
    eq(new Matcher("😀").length, 2, "length counts CODE UNITS (bytes: 4)");
    eq(new Matcher("日").length, 1, "length of a BMP character (bytes: 3)");
    eq(new Matcher("é").length, 1, "length of a Latin-1 character (bytes: 2)");
    eq(new Matcher("a😀b").length, 4, "mixed ASCII + astral");

    /* the algo options are metadata: all three spellings scan through the same
     * simd kernel and must agree row for row */
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

/* ======================================================= 2. MultiMatcher
 * The textbook Aho-Corasick case: {he, she, his, hers} over "ushers" must find
 * she@1, he@2 and hers@2. The he@2 hit is the one a naive trie misses -- it is
 * reported only because "she" fails back into "he", which is what the output
 * links exist for. */
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

    /* one pass finds every pattern, which is the entire point */
    const router = new MultiMatcher(["GET /api/", "POST /api/", "DELETE /"]);
    eqJson(router.firstIn("POST /api/users"), { index: 1, at: 0 }, "a router");
    eq(router.test("PATCH /api/users"), false, "an unrouted method");

    /* a pattern that is a prefix of another, and one that is a suffix */
    const pre = new MultiMatcher(["ab", "abc", "bc"]);
    eqJson(pre.allIn("abc"),
           [{ index: 0, at: 0 }, { index: 1, at: 0 }, { index: 2, at: 1 }],
           "prefix and suffix patterns all report");

    /* a duplicate pattern reports once, under the FIRST index */
    const dup = new MultiMatcher(["aa", "aa"]);
    eqJson(dup.allIn("aa"), [{ index: 0, at: 0 }], "a duplicate reports once");

    /* overlapping occurrences of one pattern */
    eqJson(new MultiMatcher(["aa"]).allIn("aaaa"),
           [{ index: 0, at: 0 }, { index: 0, at: 1 }, { index: 0, at: 2 }],
           "overlaps are reported");

    throws(() => new MultiMatcher([]), "an empty pattern list");
    throws(() => new MultiMatcher(["ok", ""]), "an empty pattern");
    throws(() => new MultiMatcher("not an array"), "a non-array");
    throws(() => MultiMatcher(["x"]), "calling without new");
}

/* MultiMatcher offsets are code units too */
{
    const mm = new MultiMatcher(["日", "本", "x"]);
    eqJson(mm.allIn("café 日本x"),
           [{ index: 0, at: 5 }, { index: 1, at: 6 }, { index: 2, at: 7 }],
           "every hit in CODE UNITS");
}

/* MultiMatcher equals N separate Matchers -- the property that makes the
 * automaton a drop-in for the loop it replaces */
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

/* ================================ 3. the nine String.prototype methods */
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
    /* ASCII folding only, and deliberately: full folding is locale-dependent
     * and length-changing, so a method claiming it would be wrong invisibly */
    eq("CAFÉ".equalsIgnoreCase("café"), false,
       "non-ASCII is NOT folded -- use toLowerCase() for the locale answer");

    eq("a".compareBytes("b"), -1, "compareBytes less");
    eq("b".compareBytes("a"), 1, "compareBytes greater");
    eq("a".compareBytes("a"), 0, "compareBytes equal");
    eq("ab".compareBytes("a"), 1, "a prefix sorts first");
    eq("".compareBytes(""), 0, "two empty strings");
    /* THE REASON compareBytes EXISTS. JS `<` compares UTF-16 code units, which
     * puts every non-BMP character (a surrogate pair, 0xD800-0xDFFF) BEFORE
     * U+E000-U+FFFF -- the opposite of code point order. UTF-8 byte order and
     * code point order agree, so this is what sorts like a UTF-8 database. */
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

/* they work on any this-coercible value, like every other String method */
{
    eq(String.prototype.trimPrefix.call(123, "1"), "23", "on a number");
    eq(String.prototype.indexOfAny.call({ toString: () => "abc" }, "c"), 2,
       "on an object with toString");
}

/* ============== 4. the overlapping-scan budget (countIn / allIn) =========
 * The pos+1 rescan costs n + k*plen byte-steps with the simd first+last
 * kernel (each restart scans the gap to the next match, then verifies ~plen
 * bytes), and that honest model is what the budget charges. Ordinary counts
 * -- a word 47k times in a 1 MB document, "aa" over 2e6 a's -- are one
 * linear pass and MUST SUCCEED; only k*plen-heavy shapes (a long
 * self-overlapping pattern over a periodic text) refuse with a RangeError.
 * The earlier charge, (t.len - pos) per match, modelled every restart as a
 * scan-to-end-of-text and refused those ordinary inputs (a 1 MB word count
 * threw). */
{
    eq(new Matcher("aa").countIn("aaaa"), 3, "normal overlap counting unchanged");
    eqJson(new Matcher("aa").allIn("aaaa"), [0, 1, 2], "normal allIn unchanged");

    /* an ordinary dense count is a linear pass and completes */
    {
        const dense = "a".repeat(2e6);
        let ms = Date.now();
        eq(new Matcher("aa").countIn(dense), 1999999,
           "2e6 overlapping matches are counted, not refused");
        ms = Date.now() - ms;
        assert(ms < 1000, "the dense count is a fast linear pass (" + ms + " ms)");
        /* a real-world shape the old budget refused outright */
        let log = "2026-09-16 WARN the thing failed; the retry worked\n";
        log = log.repeat(Math.ceil(1048576 / log.length));
        assert(new Matcher("the").countIn(log) > 40000,
               "counting a common word in a 1 MB log works");
    }

    /* k*plen-heavy: "ab".repeat(500) over "ab".repeat(2e6) matches at every
     * even offset (~4e6 matches x 1000-byte verify ~ 4e9 byte-steps) */
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

/* ============== 5. MultiMatcher caps total pattern bytes =============
 * Every automaton state is a 1 KiB goto table; an unbounded pattern set
 * degenerated into reallocation churn before failing. 16 MiB of pattern
 * bytes is the cap, charged per add at construction. */
{
    const chunk = "x".repeat(17000);
    const under = [];
    for (let i = 0; i < 500; i++) under.push(chunk);        /* ~8.5 MB */
    eq(new MultiMatcher(under).size, 500, "a pattern set under the byte cap builds");

    const over = [];
    for (let i = 0; i < 1200; i++) over.push(chunk);        /* ~20 MB > 16 MiB */
    throws(() => new MultiMatcher(over), "additions past the byte cap throw");
}

/* ============== 6. seeded differential vs String.prototype.indexOf ====
 * ~2000 random haystack/needle pairs over a deliberately tiny alphabet
 * (maximally many overlaps), firstIn vs indexOf (both answer -1 on a miss)
 * and allIn/countIn vs an indexOf sweep. Fixed seed: any failure replays. */
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
        const want = hay.indexOf(needle);          /* -1 on a miss, like firstIn */
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

/* ----------: JaroWinkler and DamerauLevenshtein (OSA) ---------- */
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
    // published reference values (Winkler's classic trio)
    near(JaroWinkler("MARTHA", "MARHTA"), 0.9611111111111111, "JW MARTHA/MARHTA");
    near(JaroWinkler("DWAYNE", "DUANE"), 0.84, "JW DWAYNE/DUANE");
    near(JaroWinkler("DIXON", "DICKSONX"), 0.8133333333333332, "JW DIXON/DICKSONX");
    // the 0.7 boost threshold: CRATE/TRACE scores 0.7333 with NO common
    // prefix, so no Winkler adjustment may move it
    near(JaroWinkler("CRATE", "TRACE"), 0.7333333333333333, "JW CRATE/TRACE (no prefix boost)");
    // below the threshold the score stays raw Jaro
    near(JaroWinkler("abcd", "dcba"), 0.5, "JW abcd/dcba (raw jaro, no prefix: no boost)");
    // identical and degenerate inputs
    eq(JaroWinkler("identical", "identical"), 1, "JW identical");
    eq(JaroWinkler("", ""), 1, "JW empty/empty");
    eq(JaroWinkler("a", ""), 0, "JW one empty");
    ok(JaroWinkler("abc", "xyz") >= 0 && JaroWinkler("abc", "xyz") <= 1, "JW in range");
    // code-point operands (like Levenshtein): astral chars are ONE char
    const astral = "\u{1F600}";
    ok(JaroWinkler(astral + "ab", astral + "ab") === 1, "JW astral identical");
    // code-point operands: an accented char is ONE char, not two bytes
    near(JaroWinkler("\u00e9\u00e9xx", "\u00e9\u00e9yy"), 2 / 3, "JW code points (jaro 2/3, below the boost threshold)");
    // transpositions and OSA-specific behavior
    eq(DamerauLevenshtein("abcd", "acbd"), 1, "OSA counts one transposition");
    eq(DamerauLevenshtein("ca", "abc"), 3, "OSA: ca/abc is 3, NOT unrestricted-2");
    eq(DamerauLevenshtein("kitten", "sitting"), 3, "OSA kitten/sitting");
    eq(DamerauLevenshtein("", "abc"), 3, "OSA empty vs abc");
    eq(DamerauLevenshtein("", ""), 0, "OSA empty vs empty");
    eq(DamerauLevenshtein("abc", "abc"), 0, "OSA identical");
    eq(DamerauLevenshtein("\u00e9\u00e9xx", "\u00e9\u00e9"), 2, "OSA counts code points");
    // the { max } contract matches Levenshtein's exactly
    eq(DamerauLevenshtein("abc", "abcx", { max: 2 }), 1, "OSA exact within max");
    eq(DamerauLevenshtein("abc", "abcdx", { max: 1 }), 2, "OSA overshoot answers max+1");
    eq(DamerauLevenshtein("abc", "zxy", { max: 1 }), 2, "OSA overshoot (length+sub)");
    eq(DamerauLevenshtein("abcd", "acbd", { max: 1 }), 1, "OSA banded finds the transposition");
    eq(DamerauLevenshtein("kitten", "sitting", { max: 2 }), 3, "OSA banded overshoot");
    throws(() => DamerauLevenshtein("a", "b", { max: -1 }), "OSA negative max throws");
    throws(() => DamerauLevenshtein("a"), "OSA needs two strings");
    // the error names the function you called
    let msg = "";
    try { DamerauLevenshtein("a", "b", { max: -1 }); } catch (e) { msg = String(e); }
    ok(msg.includes("DamerauLevenshtein"), "error names DamerauLevenshtein: " + msg);
    // Levenshtein parity on OSA-equivalent inputs (no adjacent swaps)
    for (const [a, b] of [["sunday", "saturday"], ["abcdef", "azced"], ["flaw", "lawn"]]) {
        eq(DamerauLevenshtein(a, b), Levenshtein(a, b), "OSA == Lev on " + a + "/" + b);
    }
    // ...and strictly below it where one adjacent swap suffices
    ok(DamerauLevenshtein("form", "from") < Levenshtein("form", "from"),
       "OSA beats Lev on a pure transposition");
}

/* ----------: MultiMatcher.replaceAllIn ---------- */
{
    function eqS(got, want, msg) {
        assert(got === want, msg + "\n  got:  " + got + "\n  want: " + want);
    }
    // string replacement over the whole pattern set in one pass
    const mm = new MultiMatcher(["world", "hello"]);
    eqS(mm.replaceAllIn("hello world hello", "X"), "X X X", "all patterns replaced");
    // the callback receives the matched text and the PATTERN INDEX -- the same
    // two fields allIn reports
    const seen = [];
    const out = mm.replaceAllIn("hello world hello", (m, i) => {
        seen.push([m, i]);
        return "<" + i + ">";
    });
    eqS(out, "<1> <0> <1>", "callback output");
    assert(JSON.stringify(seen) === JSON.stringify([["hello", 1], ["world", 0], ["hello", 1]]),
           "callback args (match, patternIndex): " + JSON.stringify(seen));
    // hits are consumed in allIn's order; overlapping hits keep the first
    // (longest) claim
    const mm2 = new MultiMatcher(["he", "she"]);
    eqS(mm2.replaceAllIn("ushers", "-"), "u-rs", "longest-first at one end: she wins over he");
    const mm3 = new MultiMatcher(["aa", "aba"]);
    assert(mm3.replaceAllIn("abaa", "_") === "_a",
           "non-overlapping leftmost: aba claims [0,3), overlapping aa skipped");
    // non-string callback results are ToString'd (String.replace conventions)
    eqS(mm.replaceAllIn("hello", () => 42), "42", "numeric callback result");
    // an empty replacement deletes (both words here: mm2 holds "he" AND "she")
    eqS(mm2.replaceAllIn("she he", ""), " ", "empty replacement deletes");
    // callback exceptions propagate and abort the scan
    let called = 0;
    try {
        mm.replaceAllIn("hello world hello", () => { called++; throw new TypeError("boom"); });
        assert(false, "callback exception must propagate");
    } catch (e) {
        assert(String(e).includes("boom"), "the callback's error surfaces");
    }
    assert(called === 1, "scan stopped at the first callback error (called=" + called + ")");
    // code-unit offsets: a wide subject keeps text between replacements intact
    const mmW = new MultiMatcher(["\u00e9"]);
    eqS(mmW.replaceAllIn("a\u00e9b\u00e9c", "#"), "a#b#c", "wide text gaps preserved");
    // argument errors
    throws(() => mm.replaceAllIn("x"), "replaceAllIn needs two arguments");
    // every pattern index reported is within [0, size)
    const mm4 = new MultiMatcher(["p0", "p1", "p2"]);
    const idxs = new Set();
    mm4.replaceAllIn("p0 p1 p2", (m, i) => { idxs.add(i); return m; });
    assert(JSON.stringify([...idxs].sort()) === "[0,1,2]", "all three indices seen");
}

/* ----------: Matcher.firstIn(text, fromIndex?) ---------- */
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
    // the empty-pattern conventions follow indexOf("", pos)
    const e = new Matcher("");
    eq(e.firstIn("abc"), 0, "empty pattern at 0");
    eq(e.firstIn("abc", 2), 2, "empty pattern: min(fromIndex, length)");
    eq(e.firstIn("abc", 5), 3, "empty pattern clamps to length");
    // CODE-UNIT offsets with a wide subject (the module's offset convention)
    const w = new Matcher("\u00e9");          // non-ASCII pattern: UTF-8 path
    eq(w.firstIn("a\u00e9b\u00e9"), 1, "wide first hit");
    eq(w.firstIn("a\u00e9b\u00e9", 2), 3, "wide fromIndex skips a code unit");
    eq(w.firstIn("a\u00e9b\u00e9", 99), -1, "wide fromIndex past the end");
    const wa = new Matcher("X");               // ASCII pattern, wide subject
    eq(wa.firstIn("\u00e9X\u00e9X", 1), 1, "ascii pattern over wide text");
    eq(wa.firstIn("\u00e9X\u00e9X", 2), 3, "ascii pattern over wide text, from 2");
}

console.log("test_matcher.js: " + n + " assertions passed");
