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
 * against the byte-offset implementation is recorded in STDLIB_OOP_PLAN.md; it
 * cannot live here, because the module it compared against is gone.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_matcher.js */

import { Matcher, MultiMatcher } from "dyna:matcher";

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

    /* the empty pattern: at 0 and nowhere else worth enumerating */
    const e = new Matcher("");
    eq(e.firstIn("abc"), 0, "empty pattern is at 0");
    eq(e.test("abc"), true, "empty pattern tests true");
    eq(e.countIn("abc"), 0, "empty pattern counts 0, not length+1");
    eqJson(e.allIn("abc"), [], "empty pattern enumerates nothing");

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

console.log("test_matcher.js: " + n + " assertions passed");
