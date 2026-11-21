// covers: c:src/builtins/builtin-string.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_builtin_string");
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

__kitS.section("trimPrefix that does not match", () => {
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
});

__kitS.section("on a number", () => {
    eq(String.prototype.trimPrefix.call(123, "1"), "23", "on a number");
    eq(String.prototype.indexOfAny.call({ toString: () => "abc" }, "c"), 2,
       "on an object with toString");
});
__kitS.end();

console.log("test_matcher.js: " + n + " assertions passed");
