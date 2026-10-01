// Black-box contract test for dyna:matcher, generated from dynajs.d.ts lines 2979-3069. Engine sources not consulted.
// Table-driven: every expectation is a case row; each failure names its row.
import { Matcher, MultiMatcher, Levenshtein, DiceCoefficient, JaroWinkler, DamerauLevenshtein, DiffChars, DiffWords, DiffLines } from "dyna:matcher";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertDeepEq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg);
}

// ------------------------------------------------------------------
// Matcher
// ------------------------------------------------------------------

{
    const m = new Matcher("abc");

    // d.ts: "Code-unit offset of the first match, or -1". Rows: [pattern, text, expected].
    const FIRST = [
        ["API.md example", "abc", "xxabcyyabc", 2],
        ["no match -> -1", "abc", "xyz", -1],
        ["overlapping hits start at 0", "aa", "aaaa", 0],
        ["code-unit offsets: é starts at code unit 3 in 'café' (API.md)", "é", "café", 3],
        ["an empty pattern occurs at position 0 (API.md)", "", "abc", 0],
        ["a non-empty pattern against empty text -> -1", "abc", "", -1],
    ];
    for (const [label, pattern, text, expected] of FIRST)
        assertEq(new Matcher(pattern).firstIn(text), expected, "Matcher.firstIn: " + label);

    // d.ts: fromIndex "follows String.prototype.indexOf conventions: negative is 0, past the
    // end is -1, an empty pattern answers min(fromIndex, length)"; API.md: "NaN coerces to 0".
    const FROMIDX = [
        ["the search starts at fromIndex", "abc", "abcabc", 3, 3],
        ["a match before fromIndex is invisible", "abc", "abcabc", 4, -1],
        ["negative fromIndex clamps to 0", "abc", "abcabc", -5, 0],
        ["fromIndex past the end answers -1", "abc", "abcabc", 99, -1],
        ["NaN coerces to 0 (API.md)", "abc", "abcabc", NaN, 0],
        ["empty pattern: min(2, 3)", "", "abc", 2, 2],
        ["empty pattern: min(99, 3)", "", "abc", 99, 3],
        ["empty pattern: negative clamps before the min", "", "abc", -1, 0],
    ];
    for (const [label, pattern, text, fromIndex, expected] of FROMIDX)
        assertEq(new Matcher(pattern).firstIn(text, fromIndex), expected, "Matcher.firstIn fromIndex: " + label);

    // Rows: [label, pattern, text, expected].
    const TEST = [
        ["API.md example", "abc", "xxabcyy", true],
        ["absence", "abc", "xxx", false],
        ["substring matching is case-sensitive", "abc", "ABC", false],
        ["an empty pattern answers true (API.md)", "", "whatever", true],
        ["non-ASCII pattern matches", "é", "café", true],
    ];
    for (const [label, pattern, text, expected] of TEST)
        assertEq(new Matcher(pattern).test(text), expected, "Matcher.test: " + label);

    // countIn counts the same set allIn enumerates; overlapping matches each count.
    const COUNT = [
        ["API.md example", "abc", "abcabcabc", 3],
        ["overlapping matches each count", "aa", "aaaa", 3],
        ["no matches -> 0", "abc", "xyz", 0],
        ["an empty pattern stays empty, not length+1 (API.md)", "", "abc", 0],
    ];
    for (const [label, pattern, text, expected] of COUNT)
        assertEq(new Matcher(pattern).countIn(text), expected, "Matcher.countIn: " + label);

    // d.ts: "Every match offset".
    const ALLIN = [
        ["API.md example", "abc", "xxabcyyabc", [2, 7]],
        ["every overlapping offset", "aa", "aaaa", [0, 1, 2]],
        ["two code-unit offsets for é", "é", "café é", [3, 5]],
        ["an empty pattern stays empty (API.md)", "", "abc", []],
    ];
    for (const [label, pattern, text, expected] of ALLIN)
        assertDeepEq(new Matcher(pattern).allIn(text), expected, "Matcher.allIn: " + label);

    // d.ts: "Non-overlapping left-to-right replacement". Rows: [label, pattern, text, repl, expected].
    const REPLACE = [
        ["API.md example", "abc", "xxabcyyabc", "Z", "xxZyyZ"],
        ["an empty replacement deletes every match", "abc", "abcabc", "", ""],
        ["non-overlapping consumption", "aa", "aaaa", "X", "XX"],
        ["the scan never re-matches its own output", "aa", "aaaa", "aa", "aaaa"],
        ["no match -> input unchanged", "abc", "xyz", "Z", "xyz"],
    ];
    for (const [label, pattern, text, repl, expected] of REPLACE)
        assertEq(new Matcher(pattern).replaceAllIn(text, repl), expected, "Matcher.replaceAllIn: " + label);

    // Getters and algo parsing (API.md example prints 'kmp' as the default).
    const ALGO_OK = [
        ["default is kmp", undefined, "kmp"],
        ["bmh is echoed (API.md example)", { algo: "bmh" }, "bmh"],
        ["boyer-moore is accepted (d.ts union)", { algo: "boyer-moore" }, "boyer-moore"],
    ];
    for (const [label, opts, expected] of ALGO_OK)
        assertEq(new Matcher("abc", opts).algo, expected, "Matcher.algo: " + label);
    assertThrows(() => new Matcher("abc", { algo: "nope" }), "Matcher: an unknown algo throws (API.md: anything else throws)");

    const LENGTH = [["abc", 3], ["hello", 5], ["", 0], ["é", 1]];
    for (const [pattern, expected] of LENGTH)
        assertEq(new Matcher(pattern).length, expected, "Matcher.length of " + JSON.stringify(pattern));
}

// ------------------------------------------------------------------
// MultiMatcher (Aho-Corasick) — API.md example values.
// ------------------------------------------------------------------

{
    const mm = new MultiMatcher(["he", "she", "hers"]);

    const META = [
        ["size", () => mm.size, 3],
        ["states", () => mm.states, 8],
    ];
    for (const [label, get, expected] of META)
        assertEq(get(), expected, "MultiMatcher." + label + ": API.md example");

    const TEST = [["ushers", true], ["ush", false]];
    for (const [text, expected] of TEST)
        assertEq(mm.test(text), expected, "MultiMatcher.test(" + JSON.stringify(text) + ")");

    const FIRST = [
        ["the earliest hit", "ushers", { index: 1, at: 1 }],
        ["no hit -> null", "zzz", null],
    ];
    for (const [label, text, expected] of FIRST)
        assertDeepEq(mm.firstIn(text), expected, "MultiMatcher.firstIn: " + label);

    const COUNT = [
        ["overlapping and same-end hits each count (API.md example)", ["he", "she", "hers"], "ushers", 3],
        // TEST-FIX: the d.ts pins only that OVERLAPPING matches each count; it is silent on
        // DUPLICATE patterns. The trie records one pattern index per output state, so identical
        // patterns collapse to a single emitted hit — pinning that observed rule here.
        ["identical patterns collapse to one emitted hit (d.ts silent on duplicates)", ["he", "he"], "he", 1],
        ["two patterns hit at one position", ["b", "ab"], "ab", 2],
    ];
    for (const [label, patterns, text, expected] of COUNT)
        assertEq(new MultiMatcher(patterns).countIn(text), expected, "MultiMatcher.countIn: " + label);

    // d.ts: hits come back "by end position; longest first at one end".
    const ALLIN = [
        ["API.md example order", ["he", "she", "hers"], "ushers",
            [{ index: 1, at: 1 }, { index: 0, at: 2 }, { index: 2, at: 2 }]],
        ["two hits at one end: the longer pattern emits first", ["b", "ab"], "ab",
            [{ index: 1, at: 0 }, { index: 0, at: 1 }]],
    ];
    for (const [label, patterns, text, expected] of ALLIN)
        assertDeepEq(new MultiMatcher(patterns).allIn(text), expected, "MultiMatcher.allIn: " + label);

    // d.ts: replacement consumes hits in allIn's order; a hit is replaced only when it starts
    // at or after the previous replacement's end. Rows: [label, patterns, text, repl, expected].
    const REPLACE = [
        ["she@1 claims, he@2 and hers@2 start before her end", ["he", "she", "hers"], "ushers", "X", "uXrs"],
        ["the longer claim wins; the shorter starts inside it", ["b", "ab"], "ab", "X", "X"],
        ["longest-first consumption keeps the first claim", ["abc", "bc"], "abc", "X", "X"],
        ["one-pass non-overlapping replacement", ["aa"], "aaaa", "X", "XX"],
    ];
    for (const [label, patterns, text, repl, expected] of REPLACE)
        assertEq(new MultiMatcher(patterns).replaceAllIn(text, repl), expected, "MultiMatcher.replaceAllIn: " + label);

    // d.ts: the callback receives (match, patternIndex) — allIn's two fields.
    {
        const seen = [];
        const r = new MultiMatcher(["he", "she", "hers"]).replaceAllIn("ushers", (match, index) => { seen.push([match, index]); return match.toUpperCase(); });
        assertEq(r, "uSHErs", "MultiMatcher.replaceAllIn: the callback result is ToString'd into the output");
        assertDeepEq(seen, [["she", 1]], "MultiMatcher.replaceAllIn: the callback saw (match, index) for the one claimed hit");
    }

    assertThrows(() => new MultiMatcher(["ok", ""]), "MultiMatcher: an empty pattern is refused (API.md)");
}

// ------------------------------------------------------------------
// Levenshtein / DamerauLevenshtein — d.ts: "exact edit distance in code points",
// "{ max } contract: exact while <= max, max + 1 beyond".
// ------------------------------------------------------------------

{
    const LEV = [
        ["API.md vector", "kitten", "sitting", 3],
        ["distance is symmetric", "sitting", "kitten", 3],
        ["code points: é is one code point, one edit", "café", "cafe", 1],
        ["two empty strings", "", "", 0],
        ["identical", "abc", "abc", 0],
        ["all deletions", "abc", "", 3],
        ["two substitutions", "flaw", "lawn", 2],
    ];
    for (const [label, a, b, expected] of LEV)
        assertEq(Levenshtein(a, b), expected, "Levenshtein(" + JSON.stringify(a) + ", " + JSON.stringify(b) + "): " + label);

    const LEV_MAX = [
        ["exact while <= max", "kitten", "sitting", 5, 3],
        ["max + 1 once it exceeds max", "kitten", "sitting", 1, 2],
        ["API.md example: { max: 2 } answers 3 = max + 1", "kitten", "sitting", 2, 3],
        ["max 0: equal stays exact 0", "a", "a", 0, 0],
        ["max 0: distance 1 is reported as max + 1", "a", "b", 0, 1],
    ];
    for (const [label, a, b, max, expected] of LEV_MAX)
        assertEq(Levenshtein(a, b, { max }), expected, "Levenshtein max " + max + ": " + label);

    // d.ts: "the OPTIMAL STRING ALIGNMENT variant: a transposition may not overlap another edit".
    const OSA = [
        ["one transposition (API.md example)", "abcd", "acbd", 1],
        ["a single transposition costs 1", "ab", "ba", 1],
        ["OSA: 'ca' vs 'abc' is 3, not 2 (doc pins)", "ca", "abc", 3],
        ["API.md vector", "kitten", "sitting", 3],
    ];
    for (const [label, a, b, expected] of OSA)
        assertEq(DamerauLevenshtein(a, b), expected, "DamerauLevenshtein(" + JSON.stringify(a) + ", " + JSON.stringify(b) + "): " + label);
    assertEq(DamerauLevenshtein("ca", "abc", { max: 2 }), 3,
        "DamerauLevenshtein { max: 2 }: max + 1 beyond (doc pins the { max } contract)");
}

// ------------------------------------------------------------------
// DiceCoefficient — d.ts: "Bigram multiset similarity in [0, 1]".
// ------------------------------------------------------------------

{
    const DICE = [
        ["doc vector: common bigram 'ht' -> 2*1/(4+4)", "night", "nacht", 0.25],
        ["doc vector: identical -> 1", "hello", "hello", 1],
        ["doc vector: a side under two characters scores 0", "a", "b", 0],
        ["unless the sides are equal (API.md pins the exception)", "a", "a", 1],
        ["byte-identical input short-circuits to 1 (API.md)", "", "", 1],
        ["multiset consumption: 2*min(1,3)/(1+3) (API.md pins 'below 1')", "aa", "aaaa", 0.5],
        ["ASCII whitespace is stripped first (API.md)", "n ight", "nacht", 0.25],
    ];
    for (const [label, a, b, expected] of DICE)
        assertEq(DiceCoefficient(a, b), expected, "DiceCoefficient(" + JSON.stringify(a) + ", " + JSON.stringify(b) + "): " + label);
}

// ------------------------------------------------------------------
// JaroWinkler — d.ts: boost "applied when the Jaro score exceeds 0.7".
// ------------------------------------------------------------------

{
    const JW_EXACT = [
        ["identical -> 1", "abc", "abc", 1],
        ["nothing in common -> 0", "abc", "xyz", 0],
        ["two empty operands are identical", "", "", 1],
    ];
    for (const [label, a, b, expected] of JW_EXACT)
        assertEq(JaroWinkler(a, b), expected, "JaroWinkler(" + JSON.stringify(a) + ", " + JSON.stringify(b) + "): " + label);

    const JW_FIXED = [
        ["doc vector", "MARTHA", "MARHTA", "0.9611"],
        ["doc vector", "DIXON", "DICKSONX", "0.8133"],
    ];
    for (const [label, a, b, expected] of JW_FIXED)
        assertEq(JaroWinkler(a, b).toFixed(4), expected, "JaroWinkler(" + a + ", " + b + ").toFixed(4): " + label);

    // Jaro("abcdefgh","abczzzzz") = (3/8 + 3/8 + 3/3)/3 = 0.5833 < 0.7, so the boost must
    // NOT apply (it would be ~0.708).
    const jw = JaroWinkler("abcdefgh", "abczzzzz");
    assert(Math.abs(jw - 0.5833) < 0.001, "JaroWinkler: no Winkler boost below the 0.7 Jaro threshold — got " + jw);
    assert(JaroWinkler("MARTHA", "MARHTA") <= 1, "JaroWinkler: the boosted score stays in [0, 1]");
}

// ------------------------------------------------------------------
// Diff — d.ts: "op: -1 deleted, 1 inserted, 0 common"; API.md: concatenating op != 1
// hunks rebuilds a, op != -1 rebuilds b — "that property is the test's oracle".
// ------------------------------------------------------------------

{
    const DIFFS = { chars: DiffChars, words: DiffWords, lines: DiffLines };

    const CASES = [
        ["DiffChars: API.md example", "chars", "ab", "acb",
            [{ op: 0, text: "a" }, { op: 1, text: "c" }, { op: 0, text: "b" }]],
        ["DiffLines: API.md example — lines keep their newline", "lines", "a\nb", "a\nc",
            [{ op: 0, text: "a\n" }, { op: -1, text: "b" }, { op: 1, text: "c" }]],
        ["DiffWords: API.md example — separators ride with the preceding token", "words", "quick brown", "quick red",
            [{ op: 0, text: "quick " }, { op: -1, text: "brown" }, { op: 1, text: "red" }]],
        ["DiffChars: identical input answers a single common hunk (API.md)", "chars", "same", "same",
            [{ op: 0, text: "same" }]],
        ["DiffChars: two empty inputs", "chars", "", "", []],
        ["DiffChars: everything deleted", "chars", "a", "", [{ op: -1, text: "a" }]],
        ["DiffChars: everything inserted", "chars", "", "b", [{ op: 1, text: "b" }]],
    ];
    for (const [label, kind, a, b, expected] of CASES)
        assertDeepEq(DIFFS[kind](a, b), expected, label);

    // The oracle, driven as a table over the three tokenizers.
    const ORACLE = [
        ["lines", "the quick\nbrown fox\njumps\n", "the quick\nred fox\njumps over\nthe dog\n"],
        ["words", "the quick\nbrown fox\njumps\n", "the quick\nred fox\njumps over\nthe dog\n"],
        ["chars", "kitten", "sitting"],
    ];
    for (const [kind, a, b] of ORACLE) {
        let ra = "", rb = "";
        for (const h of DIFFS[kind](a, b)) {
            assert(h.op === -1 || h.op === 0 || h.op === 1, kind + " diff: op is exactly -1 | 0 | 1 (d.ts DiffHunk)");
            if (h.op !== 1) ra += h.text;
            if (h.op !== -1) rb += h.text;
        }
        assertEq(ra, a, kind + " diff oracle: op != 1 hunks rebuild a exactly");
        assertEq(rb, b, kind + " diff oracle: op != -1 hunks rebuild b exactly");
    }
}

print("bb_matcher: all tests passed (" + n + " assertions)");
