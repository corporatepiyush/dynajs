// flags: --std
/* test_glob_match.js --: Glob.match(path, pattern).
 *
 * Two things are pinned here:
 *   1. the LEXICAL matcher's grammar (the dyna:file Glob block), every claim
 *      runtime-verified: '*' and '?' cross '/' because the match is over the
 *      WHOLE path string; a star run matches INCLUDING the empty string (so
 *      "x**y" matches "xy", and the pattern "a", two stars, slash "c" DOES
 *      match "a/c" -- the run matches empty), but the literals around the
 *      run stay literal (so "a", slash, two stars, slash, "c" does NOT
 *      match "a/c", and "two stars, slash, b" does not match "b" -- the run
 *      may vanish, the slash may not); character classes with bang/caret
 *      negation; NO leading-dot rule ("*idden" matches ".hidden"); backslash
 *      is NOT an escape (it is a literal byte); an unterminated "[" is
 *      literal text;
 *   2. Glob.match is the SAME matcher as Glob#matches (one implementation,
 *      two spellings): a differential over patterns x paths must agree.
 *      The one documented wrinkle: a Path contributes its NORMALISED bytes
 *      ("" becomes "."), while a plain string is used verbatim -- so the
 *      differential runs on unambiguous paths and the normalisation edge is
 *      pinned separately.
 */
import { Glob, Path } from "dyna:file";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + a + ", want " + b + ")"); }
function throws(fn, ErrType, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t === null) return;
    ok(t instanceof ErrType,
       what + " (wrong type: " + (t && t.constructor && t.constructor.name) + ")");
    ok(!re || re.test(String(t.message)),
       what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}

/* ---- 1. the documented lexical grammar ---- */
{
    eq(Glob.match("abc", "*"), true, "'*' matches a plain name");
    eq(Glob.match("a/b/c", "*"), true, "'*' crosses '/' (whole-path match)");
    eq(Glob.match(".hidden", "*"), true, "no leading-dot rule in the lexical matcher");
    eq(Glob.match(".hidden", "*idden"), true, "'*idden' matches '.hidden'");
    eq(Glob.match("a/b.txt", "*.txt"), true, "'*' spans segments to a suffix");
    eq(Glob.match("a/b/x/c", "a/*/c"), true, "'*' crosses a nested segment");
    eq(Glob.match("xy", "x**y"), true, "'**' matches an EMPTY run");
    eq(Glob.match("xZZy", "x**y"), true, "'**' matches a non-empty run");
    eq(Glob.match("a/c", "a**/c"), true,
       "'a', two stars, slash 'c' DOES match 'a/c': the run matches empty");
    eq(Glob.match("ac", "a**/c"), false,
       "but 'ac' cannot feed the run's literal slash");
    eq(Glob.match("a/c", "a/**/c"), false,
       "two literal slashes are two slashes: 'a/**/c' is NOT 'a/c'");
    eq(Glob.match("a/b/c", "a/**/c"), true, "and it matches with the slashes present");
    eq(Glob.match("abc", "a?c"), true, "'?' is exactly one character");
    eq(Glob.match("a/c", "a?c"), true, "'?' crosses '/' too");
    eq(Glob.match("ac", "a?c"), false, "'?' is not zero characters");
    eq(Glob.match("abc", "[abc]bc"), true, "character class");
    eq(Glob.match("abc", "[a-c]bc"), true, "character range");
    eq(Glob.match("abc", "[!a]bc"), false, "'[!...]' negation");
    eq(Glob.match("zbc", "[!a]bc"), true, "'[!...]' negation (hit)");
    eq(Glob.match("zbc", "[^a]bc"), true, "'[^...]' negation (hit)");
    eq(Glob.match("abc", "[^a]bc"), false, "'[^...]' negation");
    eq(Glob.match("a\\*b", "a\\*b"), true,
       "backslash is a LITERAL byte the path must carry, not an escape");
    eq(Glob.match("a\\Xb", "a\\*b"), true,
       "the '*' after a literal backslash is still a wildcard");
    eq(Glob.match("[a-", "[a-"), true, "an unterminated '[' is literal text");
    eq(Glob.match("", ""), true, "the empty pattern matches the empty path only");
    eq(Glob.match("a", ""), false, "the empty pattern matches nothing else");
    eq(Glob.match("a/", "a/*"), true, "'*' matches an empty run at the end");
    eq(Glob.match("b", "**/b"), false,
       "'**/b' still wants its literal '/': zero-run, not zero-literal");
    eq(Glob.match("a/b", "**/b"), true, "and matches with the slash present");
    eq(Glob.match("b", "b"), true, "literal equality");
    eq(Glob.match("B", "b"), false, "literal match is case-sensitive");
}

/* ---- 2. differential: Glob.match == Glob#matches ---- */
{
    const patterns = ["*", "**", "*.*", "*.txt", "a/*", "a/**/c", "a/*/c",
                      "a**/c", "**/c",
                      "**/b", "x**y", "a?c", "?idden", "*idden", "[abc]bc",
                      "[a-c]bc", "[!a]bc", "[^a]bc", "a\\*b", "[a-", "",
                      "src/*.js", "src/**/*.js", "!neg", "**/*.tgz"];
    const paths = ["a", "b", "ab", "abc", "a/b", "a/b/c", "a/c", "a/b.txt",
                   "a/b/x/c", "xYYy", "xy", "a.txt", ".hidden", "idden",
                   "src/app.js", "src/a/b.js", "zbc", "a*b", "aXXb", "a-",
                   "a/", "b/c.tgz", "not!neg", "A"];
    for (const pat of patterns) {
        const g = new Glob(pat);
        for (const p of paths) {
            eq(Glob.match(p, pat), g.matches(new Path(p)),
               "differential pat=" + JSON.stringify(pat) + " path=" +
               JSON.stringify(p));
        }
    }
}

/* ---- 3. the Path-vs-string wrinkle is documented, not silent ---- */
{
    eq(Glob.match(new Path("a/b.txt"), "*.txt"), true, "a Path works");
    eq(Glob.match(new Path("a/b.txt"), "a/*"), true, "a Path works (2)");
    /* new Path("") normalises to "." -- the Path spelling therefore answers
       for "." while the string spelling answers for "" */
    eq(Glob.match("", ""), true, "string spelling: empty path");
    eq(Glob.match(new Path(""), ""), false,
       "Path spelling: the empty Path is '.' (normalised), pinned");
}

/* ---- 4. refusals ---- */
{
    throws(() => Glob.match("a", 7), TypeError, /pattern must be a string/,
           "non-string pattern refuses");
    throws(() => Glob.match("a"), TypeError, /pattern must be a string/,
           "missing pattern refuses");
    throws(() => Glob.match(), TypeError, /pattern must be a string/,
           "missing everything refuses");
    throws(() => Glob.match({}, "*"), TypeError, /must be a Path/,
           "a non-string non-Path path refuses");
    /* the constructor keeps its own contract */
    throws(() => new Glob(7), TypeError, /requires a string/,
           "non-string constructor pattern refuses");
}

print("test_glob_match: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
