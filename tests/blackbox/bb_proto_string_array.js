// Parametric black-box contract test, generated from dynajs.d.ts lines 7344-7674. Engine sources not consulted.
// Coverage: String.prototype extensions (lazy .. toWellFormed), Array.prototype extensions
// (isEmpty .. lazy), ArrayConstructor.repeat/fromAsync, Map.getOrInsert/getOrInsertComputed, Set.groupBy.
//
// Pinning policy (per brief): EXACT outputs only where derivable from dynajs.d.ts's own words or universal
// conventions (escapeHTML/&amp;, slug rules, toNumber(base), base64 of ASCII); everything else is asserted as a
// structural/property fact (returns string, idempotent, round-trips, purity) via predicate expectations.
//
// Table convention: every row is [label, () => actual, expectation] where expectation is
//   - an exact value -> assertEq (NaN-safe), an array -> deep equality via JSON,
//   - a predicate fn -> structural fact (row comment says which),
//   - {throws: patternOrNull, type: ErrCtorOrNull} -> must throw.
// One loop per table; failure messages name the row label.

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

function runRows(rows) {
  for (const [label, get, exp] of rows) {
    if (exp !== null && typeof exp === "object" && "throws" in exp) { assertThrows(() => get(), label, exp.type, exp.throws); continue; }
    const actual = get();
    if (typeof exp === "function") { assert(exp(actual), label + " [structural predicate]"); continue; }
    if (Array.isArray(exp)) { assertDeepEq(actual, exp, label); continue; }
    assertEq(actual, exp, label);
  }
}
const astral = "\u{10348}"; // U+10348 Gothic hwair -- one astral code point

// ===========================================================================
// STRING.prototype extensions (dynajs.d.ts lines 7344-7465)
// ===========================================================================

// lazy/isEmpty/trim*/contains/indexOf*/equals/compare/splitN/isBlank (lines 7344-7374)
runRows([
  ["lazy: an astral character is ONE entry (code-point iteration, doc runtime-verified)", () => { const r = [...("a" + astral + "b").lazy()]; return r.length === 3 && r[1] === astral; }, true],
  ["lazy: ARGUMENTS ARE IGNORED (doc) -- lazy(999) still yields 2 entries for 'ab'", () => [...("ab").lazy(999)].length, 2],
  ["lazy composes with the Iterator helpers (doc's own example shape)", () => "a,b".lazy().map((s) => s.toUpperCase()).toArray(), ["A", ",", "B"]], // code points: the comma passes through
  ["isEmpty('') === true", () => "".isEmpty(), true],
  ["isEmpty(' ') === false (space is not empty)", () => " ".isEmpty(), false],
  ["isEmpty('a') === false", () => "a".isEmpty(), false],
  ["trimPrefix strips a present prefix", () => "hello".trimPrefix("he"), "llo"],
  ["trimPrefix leaves the string alone when absent", () => "hello".trimPrefix("x"), "hello"],
  ["trimPrefix full-string match -> ''", () => "ab".trimPrefix("ab"), ""],
  ["trimSuffix strips a present suffix", () => "hello".trimSuffix("lo"), "hel"],
  ["trimSuffix leaves the string alone when absent", () => "hello".trimSuffix("x"), "hello"],
  ["trimSuffix full-string match -> ''", () => "ab".trimSuffix("ab"), ""],
  ["trimChars strips the set from both ends only", () => "xxabxx".trimChars("x"), "ab"],
  ["trimChars with a multi-char set", () => "xyayx".trimChars("xy"), "a"],
  ["trimChars of all-set string -> ''", () => "xxx".trimChars("x"), ""],
  ["containsAny hit", () => "hello".containsAny("lo"), true],
  ["containsAny miss", () => "hello".containsAny("xyz"), false],
  ["containsAny with empty set -> false", () => "".containsAny("a"), false],
  ["indexOfAny first position of any set char", () => "hello".indexOfAny("ol"), 2],
  ["indexOfAny miss -> -1", () => "hello".indexOfAny("xyz"), -1],
  ["indexOfAny empty set -> -1", () => "abc".indexOfAny(""), -1],
  ["DOC: indexOfAll counts OVERLAPS -- 'aaa' / 'aa' -> [0,1]", () => "aaa".indexOfAll("aa"), [0, 1]],
  ["indexOfAll ascending positions", () => "ababab".indexOfAll("ab"), [0, 2, 4]],
  ["indexOfAll miss -> []", () => "abc".indexOfAll("x"), []],
  ["equalsIgnoreCase case-insensitive hit", () => "AbC".equalsIgnoreCase("abc"), true],
  ["equalsIgnoreCase miss", () => "abc".equalsIgnoreCase("abd"), false],
  ["equalsIgnoreCase mixed both sides", () => "aBc".equalsIgnoreCase("AbC"), true],
  ["compareBytes less -> -1", () => "a".compareBytes("b"), -1],
  ["compareBytes equal -> 0", () => "abc".compareBytes("abc"), 0],
  ["compareBytes greater -> 1", () => "b".compareBytes("a"), 1],
  ["splitN limit 2: last piece keeps the separators", () => "a,b,c,d".splitN(",", 2), ["a", "b,c,d"]],
  ["splitN n larger than piece count", () => "a,b".splitN(",", 5), ["a", "b"]],
  ["splitN n = 1 -> whole string", () => "a,b".splitN(",", 1), ["a,b"]],
  ["isBlank('') === true (doc: empty or all whitespace)", () => "".isBlank(), true],
  ["isBlank(all whitespace) === true", () => " \t\n ".isBlank(), true],
  ["isBlank(' a ') === false", () => " a ".isBlank(), false],
]);

// first/last/from/to/chars/codes/reverse/insert/remove*/compact/shift/pad/capitalize/case-conv (7375-7410)
runRows([
  ["first(2)", () => "hello".first(2), "he"],
  ["first(0) -> '' (doc: or '')", () => "hello".first(0), ""],
  ["first(n > length) -> whole string", () => "hello".first(99), "hello"],
  ["last(2)", () => "hello".last(2), "lo"],
  ["last(0) -> ''", () => "hello".last(0), ""],
  ["last(n > length) -> whole string", () => "hello".last(99), "hello"],
  ["from(1) to end", () => "hello".from(1), "ello"],
  ["from(1,3) exclusive end", () => "hello".from(1, 3), "el"],
  ["from(beyond length) -> ''", () => "hello".from(99), ""],
  ["to(3)", () => "hello".to(3), "hel"],
  ["to(0) -> ''", () => "hello".to(0), ""],
  ["to(beyond length) -> whole string", () => "hello".to(99), "hello"],
  ["chars: code points (astral is ONE entry)", () => ("a" + astral + "b").chars(), ["a", astral, "b"]],
  ["chars ascii", () => "abc".chars(), ["a", "b", "c"]],
  ["codes: UTF-8 byte values ('é' -> 195,169)", () => "é".codes(), [195, 169]],
  ["codes ascii", () => "AB".codes(), [65, 66]],
  ["reverse ascii", () => "abc".reverse(), "cba"],
  ["reverse is astral-safe (code points, not code units)", () => ("a" + astral + "b").reverse(), "b" + astral + "a"],
  ["reverse('') -> ''", () => "".reverse(), ""],
  ["insert default i = end (doc: default end)", () => "abcd".insert("X"), "abcdX"],
  ["insert at 0", () => "abcd".insert("X", 0), "Xabcd"],
  ["insert at 2", () => "abcd".insert("X", 2), "abXcd"],
  ["remove first occurrence only", () => "abcabc".remove("b"), "acabc"],
  ["remove absent -> unchanged", () => "abc".remove("z"), "abc"],
  ["removeAll every occurrence", () => "abcabc".removeAll("b"), "acac"],
  ["removeAll consumes matches successively ('aaaa'/'aa' -> '')", () => "aaaa".removeAll("aa"), ""],
  ["compact collapses internal whitespace runs and trims", () => "  a   b  ".compact(), "a b"],
  ["compact('') -> ''", () => "".compact(), ""],
  ["shift default n = 0 -> unchanged (doc: default 0)", () => "abc".shift(), "abc"],
  ["shift(1) caesar-shifts ASCII letters", () => "abc".shift(1), "bcd"],
  ["shift(-1) shifts back", () => "abc".shift(-1), "zab"],
  ["shift wraps around the alphabet", () => "xyz".shift(1), "yza"],
  ["shift leaves non-letters untouched", () => "a1!".shift(1), "b1!"],
  ["shift is astral-safe: surrogate units pass through unshifted", () => ("a" + astral + "z").shift(1), "b" + astral + "a"],
  ["pad even total is unambiguous centering", () => "ab".pad(6, "*"), "**ab**"],
  ["pad no-op at len <= length", () => "ab".pad(2, "*"), "ab"],
  ["pad default padding is trimmable (structural: default char not pinned)", () => { const p = "ab".pad(4); return p.length === 4 && p.trim() === "ab"; }, true],
  ["pad odd total: length exact, only pad chars added (structural)", () => { const p = "ab".pad(5, "*"); return p.length === 5 && p.split("*").join("") === "ab"; }, true],
  ["capitalize first character", () => "hello".capitalize(), "Hello"],
  ["capitalize(lower) lowercases the rest", () => "hELLO".capitalize(true), "Hello"],
  ["capitalize(all) uppercases each word's first char", () => "hello world".capitalize(false, true), "Hello World"],
  ["capitalize(lower, all) per word", () => "hELLO wORLD".capitalize(true, true), "Hello World"],
  ["capitalize('') -> ''", () => "".capitalize(), ""],
  ["underscore CamelCase -> snake_case", () => "FooBar".underscore(), "foo_bar"],
  ["underscore idempotent on snake_case", () => "already_snake".underscore(), "already_snake"],
  ["dasherize CamelCase -> dash-separated", () => "FooBar".dasherize(), "foo-bar"],
  ["dasherize lowerCamel", () => "fooBar".dasherize(), "foo-bar"],
  ["spacify underscores -> spaces", () => "foo_bar".spacify(), "foo bar"],
  ["spacify dashes -> spaces", () => "foo-bar".spacify(), "foo bar"],
  ["camelize default upper -> UpperCamelCase (doc: upper true, default)", () => "foo_bar".camelize(), "FooBar"],
  ["camelize(false) -> lowerCamelCase", () => "foo-bar".camelize(false), "fooBar"],
  ["camelize multi-separator", () => "foo_bar_baz".camelize(), "FooBarBaz"],
]);

// truncate/escape/tags/count/toNumber/humanize/titleize/parameterize/plural/singular/removeTags/forEach (7411-7438)
runRows([
  // dynajs.d.ts: `length` is the KEPT length, ellipsis added OUTSIDE it
  // ("abcdef".truncate(4) -> "abcd..."); the d.ts's "to len characters"
  // is the loose wording, resolved to the dynajs.d.ts example
  ["truncate right: kept head + ellipsis outside", () => "hello world".truncate(8, "right", "..."), "hello wo..."],
  ["truncate left: ellipsis + kept tail", () => "hello world".truncate(8, "left", "..."), "...lo world"],
  ["truncate middle (dynajs.d.ts: kept head + ... + kept tail)", () => { const r = "hello world".truncate(8, "middle", "..."); return r.length === 11 && r.startsWith("hell") && r.endsWith("orld") && r.includes("..."); }, true],
  ["truncate no-op when the string already fits", () => "hi".truncate(8, "right", "..."), "hi"],
  ["truncateOnWord stops at a word boundary (structural shape)", () => { const r = "hello world".truncateOnWord(8, "right", "..."); return r.startsWith("hello") && r.includes("...") && r.length <= 8; }, true],
  ["truncateOnWord no-op when short", () => "hi".truncateOnWord(8, "right", "..."), "hi"],
  ["escapeHTML '<'", () => "<".escapeHTML(), "&lt;"],
  ["escapeHTML '>'", () => ">".escapeHTML(), "&gt;"],
  ["escapeHTML '&' (universal convention)", () => "&".escapeHTML(), "&amp;"],
  // dynajs.d.ts pins the set: escapeHTML() turns & < > " ' into entities, so the
  // output is safe inside a quoted attribute as well as in element content.
  ["escapeHTML escapes the double quote (dynajs.d.ts entity set)", () => "\"".escapeHTML(), "&quot;"],
  ["escapeHTML escapes the single quote (dynajs.d.ts entity set)", () => "'".escapeHTML(), "&#39;"],
  ["escapeHTML attribute breakout is neutralised", () => "x\" onmouseover=\"alert(1)".escapeHTML().includes("\""), false],
  ["unescapeHTML entities back to characters", () => "&lt;b&gt;&amp;&quot;&#65;".unescapeHTML(), "<b>&\"A"],
  ["HTML escape/unescape round-trip (structural)", () => { const s = "<a class=\"x\">&</a>"; return s.escapeHTML().unescapeHTML() === s; }, true],
  ["stripTags removes tags, keeps content", () => "<p>hi <b>there</b>!</p>".stripTags(), "hi there!"],
  ["stripTags on tag-free text -> unchanged", () => "no tags".stripTags(), "no tags"],
  // removeTags([name]): with NO name the general form applies — every element
  // goes, content included; text outside tags survives
  ["removeTags() removes EVERY element, content included", () => "<div><p>a</p><span>b</span></div>".removeTags(), ""],
  ["removeTags() keeps text outside elements", () => "<p>x</p>plain".removeTags(), "plain"],
  ["removeTags(name) removes only that element, content included", () => "<p>keep</p><b>drop</b>".removeTags("b"), "<p>keep</p>"],
  ["count occurrences", () => "aaa".count("a"), 3],
  ["count is non-overlapping (indexOfAll documents overlaps explicitly; count does not)", () => "aaaa".count("aa"), 2],
  ["count miss -> 0", () => "abc".count("z"), 0],
  ["toNumber default base 10", () => "42".toNumber(), 42],
  ["toNumber base 16", () => "ff".toNumber(16), 255],
  ["toNumber base 2", () => "101".toNumber(2), 5],
  ["toNumber base 36 ('z' = 35)", () => "z".toNumber(36), 35],
  ["toNumber garbage -> NaN (doc: or NaN)", () => "garbage".toNumber(), NaN],
  ["humanize snake_case -> human label", () => "employee_salary".humanize(), "Employee salary"],
  ["titleize title-cases words", () => "hello world".titleize(), "Hello World"],
  // dynajs.d.ts: "capitalizes each word, lowercasing small stop words unless first"
  ["titleize lowercases small stop words unless first (dynajs.d.ts)", () => "man from the boondocks".titleize(), "Man from the Boondocks"],
  ["parameterize slug (classic 'Donald E. Knuth' shape)", () => "Donald E. Knuth".parameterize(), "donald-e-knuth"],
  ["parameterize strips punctuation runs to one dash", () => "Hello, World!".parameterize(), "hello-world"],
  ["pluralize naive: append-s form", () => "cat".pluralize(), "cats"],
  ["pluralize never returns empty (structural)", () => "x".pluralize().length >= 1, true],
  ["singularize strips plain 's'", () => "cats".singularize(), "cat"],
  ["singularize 'es' form", () => "boxes".singularize(), "box"],
  ["forEach walks each character", () => { const seen = []; "abc".forEach((ch) => seen.push(ch)); return seen; }, ["a", "b", "c"]],
  ["forEach on '' -> no calls", () => { const seen = []; "".forEach((ch) => seen.push(ch)); return seen; }, []],
]);

// format/words/lines/base64/url/ansi/width/wrap/graphemes/wellformed (7439-7464)
runRows([
  ["format {0} positional", () => "hello {0}".format("world"), "hello world"],
  ["format {0}/{1} positional", () => "{0}-{1}".format("x", "y"), "x-y"],
  ["format {name} from ONE object arg (doc: positional or one object)", () => "{a}+{b}".format({ a: 1, b: 2 }), "1+2"],
  ["format {{ }} escapes a literal brace (doc)", () => "{{literal}}".format(), "{literal}"],
  ["format repeated placeholder", () => "{0}{0}".format("x"), "xx"],
  ["words split on whitespace runs", () => "hello  world\tfoo".words(), ["hello", "world", "foo"]],
  ["words('') -> []", () => "".words(), []],
  ["words trims padding", () => " one ".words(), ["one"]],
  ["lines split on \\n", () => "a\nb\nc".lines(), ["a", "b", "c"]],
  ["lines of a single line", () => "single".lines(), ["single"]],
  ["lines keep empty interior lines (conventional split semantics)", () => "a\n\nb".lines(), ["a", "", "b"]],
  ["encodeBase64 ASCII", () => "hello".encodeBase64(), "aGVsbG8="],
  ["encodeBase64 is UTF-8 (doc: 'é' -> 'w6k=')", () => "é".encodeBase64(), "w6k="],
  ["decodeBase64 ASCII", () => "aGVsbG8=".decodeBase64(), "hello"],
  ["base64 round-trip on non-ASCII (structural)", () => "héllo".encodeBase64().decodeBase64() === "héllo", true],
  ["escapeURL default is encodeURI semantics (reserved / ? = kept)", () => "a b/c?d".escapeURL(), "a%20b/c?d"],
  ["escapeURL(true) is encodeURIComponent semantics", () => "a b&c=d".escapeURL(true), "a%20b%26c%3Dd"],
  ["unescapeURL default is decodeURIComponent (%2F decoded)", () => "a%2Fb".unescapeURL(), "a/b"],
  ["unescapeURL(true) is decodeURI (reserved %26 kept)", () => "a%20b%26c".unescapeURL(true), "a b%26c"],
  ["DOC ASYMMETRY: escapeURL(true) encodes like encodeURIComponent but unescapeURL(true) decodes like decodeURI, so %26 survives", () => "a%26b".unescapeURL(true), "a%26b"],
  ["unescapeURL(true) still decodes unreserved %20", () => "a%20b".unescapeURL(true), "a b"],
  ["stripAnsi removes escape sequences", () => "\x1B[31mred\x1B[0m".stripAnsi(), "red"],
  ["stripAnsi on plain text -> unchanged", () => "plain".stripAnsi(), "plain"],
  ["displayWidth ASCII count", () => "abc".displayWidth(), 3],
  ["displayWidth('') -> 0", () => "".displayWidth(), 0],
  ["displayWidth wide characters count twice (doc)", () => "你好".displayWidth(), 4],
  ["displayWidth ANSI sequences contribute zero width (pairs with stripAnsi)", () => "\x1B[1mab\x1B[0m".displayWidth(), 2],
  ["ambiguous-width char counts 1 by default (U+00B0, East Asian Ambiguous)", () => "\u00B0".displayWidth(), 1],
  ["ambiguousAsWide option counts it 2 (doc option)", () => "\u00B0".displayWidth({ ambiguousAsWide: true }), 2],
  ["wrapAnsi word-wraps at the width (word wrap, no hard break needed)", () => "hello world".wrapAnsi(5), "hello\nworld"],
  ["wrapAnsi hard option breaks long words", () => "abcdef".wrapAnsi(3, { hard: true }), "abc\ndef"],
  ["wrapAnsi without hard keeps an unbreakable word whole", () => "abcdef".wrapAnsi(3), "abcdef"],
  ["graphemes: astral char is one cluster", () => { const g = ("a" + astral + "b").graphemes(); return g.length === 3 && g[1] === astral; }, true],
  ["graphemes: combining mark stays in one cluster (structural: no composed form pinned)", () => { const g = "e\u0301".graphemes(); return g.length === 1 && g[0].length === 2 && g[0][0] === "e" && g[0].charCodeAt(1) === 0x0301; }, true],
  ["graphemes ascii", () => "ab".graphemes(), ["a", "b"]],
  ["isWellFormed clean string -> true", () => "abc".isWellFormed(), true],
  ["isWellFormed lone surrogate -> false", () => "\uD800".isWellFormed(), false],
  ["toWellFormed replaces lone surrogates with U+FFFD (doc)", () => "\uD800".toWellFormed(), "\uFFFD"],
  ["toWellFormed leaves well-formed text unchanged", () => "ok".toWellFormed(), "ok"],
  ["toWellFormed replaces only the lone surrogate in context", () => "a\uD800b".toWellFormed(), "a\uFFFDb"],
]);

// ===========================================================================
// ARRAY.prototype extensions (dynajs.d.ts lines 7467-7655)
// ===========================================================================

// basics/aggregation (isEmpty..max, 7468-7493)
runRows([
  ["isEmpty([]) true", () => [].isEmpty(), true],
  ["isEmpty([1]) false", () => [1].isEmpty(), false],
  ["first", () => [1, 2].first(), 1],
  ["first([]) -> undefined", () => [].first(), undefined],
  ["last", () => [1, 2].last(), 2],
  ["last([]) -> undefined", () => [].last(), undefined],
  ["sum", () => [1, 2, 3].sum(), 6],
  ["sum([]) -> 0 (additive identity)", () => [].sum(), 0],
  ["sum fractional", () => [1.5, 2.5].sum(), 4],
  ["average (canonical spelling)", () => [1, 2, 3].average(), 2],
  ["mean (LEGACY alias, doc) same result", () => [1, 2, 3].mean(), 2],
  ["parity: average === mean on the same input", () => [2, 4].average() === [2, 4].mean(), true],
  ["both spellings exist on the prototype", () => typeof [].average === "function" && typeof [].mean === "function", true],
  ["compact removes null/undefined", () => [1, null, 2, undefined, 3].compact(), [1, 2, 3]],
  ["compact all-null -> []", () => [null, undefined].compact(), []],
  ["count by value", () => [1, 2, 2, 3].count(2), 2],
  ["count by predicate", () => [1, 2, 3].count((v) => v > 1), 2],
  ["count by RegExp", () => ["a1", "bb", "c2"].count(/\d/), 2],
  ["count() with no matcher -> all elements", () => [1, 2, 3].count(), 3],
  ["none true", () => [1, 2].none((v) => v > 5), true],
  ["none false", () => [1, 2].none((v) => v > 1), false],
  ["any true/false", () => [[1, 2].any((v) => v > 1), [1, 2].any((v) => v > 5)], [true, false]],
  ["all true/false", () => [[1, 2].all((v) => v > 0), [1, 2].all((v) => v > 1)], [true, false]],
  ["min identity", () => [3, 1, 2].min(), 1],
  ["min by mapper", () => ["apple", "kiwi"].min((s) => s.length), "kiwi"],
  ["min by property key", () => [{ a: 2 }, { a: 1 }].min("a"), (r) => r.a === 1],
  ["max identity", () => [3, 1, 2].max(), 3],
  ["max by property key", () => [{ a: 2 }, { a: 1 }].max("a"), (r) => r.a === 2],
]);

// take/drop family + whiles (7494-7501, 7568-7571)
runRows([
  ["take", () => [1, 2, 3].take(2), [1, 2]],
  ["take(0) -> []", () => [1, 2, 3].take(0), []],
  ["take(n > length) -> whole array", () => [1, 2, 3].take(9), [1, 2, 3]],
  ["drop", () => [1, 2, 3].drop(1), [2, 3]],
  ["drop(0) -> all", () => [1, 2, 3].drop(0), [1, 2, 3]],
  ["drop(n > length) -> []", () => [1, 2, 3].drop(9), []],
  ["takeLast", () => [1, 2, 3].takeLast(2), [2, 3]],
  ["takeLast(n > length) -> all", () => [1, 2, 3].takeLast(9), [1, 2, 3]],
  ["dropLast", () => [1, 2, 3].dropLast(1), [1, 2]],
  ["dropLast(n > length) -> []", () => [1, 2, 3].dropLast(9), []],
  ["take/drop are pure (source untouched)", () => { const a = [1, 2, 3]; a.take(2); a.drop(1); return JSON.stringify(a) === "[1,2,3]"; }, true],
  ["takeWhile prefix", () => [1, 2, 3, 1].takeWhile((v) => v < 3), [1, 2]],
  ["dropWhile prefix", () => [1, 2, 3, 1].dropWhile((v) => v < 3), [3, 1]],
  ["takeLastWhile suffix", () => [1, 3, 1, 2].takeLastWhile((v) => v < 3), [1, 2]],
  ["dropLastWhile suffix", () => [1, 3, 1, 2].dropLastWhile((v) => v < 3), [1, 3]],
]);

// sortBy/sortedIndexOf/shuffle/sample/unique family/parity aliases (7502-7520)
runRows([
  ["sortBy key fn", () => [3, 1, 2].sortBy((x) => x), [1, 2, 3]],
  ["sortBy is STABLE (doc): equal keys keep input order", () => {
    const out = [{ k: 1, i: "a" }, { k: 0, i: "b" }, { k: 1, i: "c" }].sortBy((o) => o.k);
    return out.map((o) => o.i).join("");
  }, "bac"],
  // DOC-TENSION resolved: dynajs.d.ts pins indexOf semantics ("binary search ...
  // returning the index or -1"); the d.ts's "insertion index" was the outlier
  ["sortedIndexOf missing value -> -1", () => [1, 3, 5].sortedIndexOf(4), -1],
  ["sortedIndexOf existing element -> its index", () => [1, 3, 5].sortedIndexOf(3), 1],
  ["sortedIndexOf below-all and above-all -> -1", () => [[1, 3, 5].sortedIndexOf(0), [1, 3, 5].sortedIndexOf(6)], [-1, -1]],
  ["sortedIndexOf with a matching descending comparator", () => [5, 3, 1].sortedIndexOf(3, (a, b) => b - a), 1],
  ["shuffle: multiset preserved, source untouched (GLOBAL RNG -- structure only, never order)", () => {
    const a = [1, 2, 3, 4, 5];
    const s = a.shuffle();
    return JSON.stringify([...s].sort((x, y) => x - y)) === "[1,2,3,4,5]" && a.length === 5 && a[0] === 1;
  }, true],
  ["sample(n): n distinct elements from the source (without replacement)", () => {
    const a = [1, 2, 3, 4, 5];
    const s = a.sample(3);
    return s.length === 3 && new Set(s).size === 3 && s.every((x) => a.includes(x));
  }, true],
  ["sample(n > length) -> whole array", () => { const s = [1, 2].sample(9); return JSON.stringify([...s].sort()) === "[1,2]"; }, true],
  ["sample(0) -> []", () => [1, 2].sample(0), []],
  ["unique keeps first occurrence", () => [1, 2, 1, 3, 2].unique(), [1, 2, 3]],
  ["unique(map) dedups by key, first kept", () => [{ id: 2 }, { id: 1 }, { id: 2 }].unique((o) => o.id).map((o) => o.id), [2, 1]],
  ["unique('k') property-key form", () => [{ k: 1 }, { k: 2 }, { k: 1 }].unique("k").length, 2],
  ["uniq (LEGACY alias, doc) same result", () => [1, 2, 1, 3, 2].uniq(), [1, 2, 3]],
  ["parity: unique === uniq on the same input; both spellings exist", () => {
    const a = [1, 2, 1];
    return JSON.stringify(a.unique()) === JSON.stringify(a.uniq()) && typeof [].unique === "function" && typeof [].uniq === "function";
  }, true],
  ["uniqBy key fn", () => [{ k: 1 }, { k: 1 }, { k: 2 }].uniqBy((o) => o.k).length, 2],
]);

// set operations + parity aliases (7521-7531, 7617-7623)
{
  const floorEq = (a, b) => Math.floor(a) === Math.floor(b);
  runRows([
    ["intersect keeps this-array order", () => [3, 1, 2].intersect([1, 2]), [1, 2]],
    ["intersect basic", () => [1, 2, 3].intersect([2, 3, 4]), [2, 3]],
    ["intersection (LEGACY alias, doc) same result", () => [1, 2, 3].intersection([2, 3, 4]), [2, 3]],
    ["parity: intersect === intersection; both spellings exist", () => {
      const a = [1, 2, 3], b = [2, 3];
      return JSON.stringify(a.intersect(b)) === JSON.stringify(a.intersection(b)) && typeof [].intersect === "function" && typeof [].intersection === "function";
    }, true],
    ["difference", () => [1, 2, 3].difference([2]), [1, 3]],
    ["without (doc: a copy) same result and source untouched", () => { const a = [1, 2, 3]; const r = a.without([2]); return JSON.stringify(r) === "[1,3]" && a.length === 3; }, true],
    ["union this-first then new elements", () => [2].union([1, 2]), [2, 1]],
    ["union basic", () => [1, 2].union([2, 3]), [1, 2, 3]],
    ["unionWith custom eq (2.5 equals 2, 3 is new)", () => [1, 2].unionWith(floorEq, [2.5, 3]), [1, 2, 3]],
    ["differenceWith custom eq", () => [1, 2, 2.5].differenceWith(floorEq, [2]), [1]],
    ["symmetricDifference", () => [1, 2, 3].symmetricDifference([3, 4]), [1, 2, 4]],
    ["symmetricDifferenceWith custom eq", () => [1, 2].symmetricDifferenceWith(floorEq, [2.5, 3]), [1, 3]],
  ]);
}

// partition/pluck/zip/intersperse/flatten/transpose/xprod/aperture/splitEvery/splitAt (7532-7551)
runRows([
  ["partition [passing, failing]", () => [1, 2, 3, 4].partition((v) => v % 2 === 0), [[2, 4], [1, 3]]],
  ["pluck property per element", () => [{ a: 1 }, { a: 2 }].pluck("a"), [1, 2]],
  ["zip pairs", () => [1, 2].zip(["a", "b"]), [[1, "a"], [2, "b"]]],
  ["zip uneven (structural: length = shorter, shape conventional)", () => { const r = [1, 2, 3].zip(["a"]); return r.length === 1 && r[0][0] === 1 && r[0][1] === "a"; }, true],
  ["zipWith fn", () => [1, 2].zipWith((a, b) => a + b, [10, 20]), [11, 22]],
  ["intersperse between every pair", () => [1, 2, 3].intersperse(0), [1, 0, 2, 0, 3]],
  ["intersperse single/no elements", () => [[1].intersperse(0), [].intersperse(0)], [[1], []]],
  ["flatten one level", () => [[1, 2], [3]].flatten(), [1, 2, 3]],
  // dynajs.d.ts: flatten() is RECURSIVE (guard 512); unnest() flattens ONE level —
  // the d.ts's old "flattens one level / alias of flatten" wording corrected
  ["flatten is recursive (dynajs.d.ts)", () => [1, [2, [3]]].flatten(), [1, 2, 3]],
  ["unnest flattens exactly ONE level", () => [1, [2, [3]]].unnest(), [1, 2, [3]]],
  ["transpose square", () => [[1, 2], [3, 4]].transpose(), [[1, 3], [2, 4]]],
  ["transpose non-square (structural: column count = longest row; ragged padding not pinned)", () => {
    const r = [[1, 2, 3], [4]].transpose();
    return r.length === 3 && JSON.stringify(r[0]) === "[1,4]" && r[1][0] === 2 && r[2][0] === 3;
  }, true],
  ["xprod this-array-major order", () => [1, 2].xprod(["a", "b"]), [[1, "a"], [1, "b"], [2, "a"], [2, "b"]]],
  ["aperture sliding windows", () => [1, 2, 3, 4].aperture(2), [[1, 2], [2, 3], [3, 4]]],
  ["aperture(n > length) -> []", () => [1, 2].aperture(9), []],
  ["aperture(1) -> singleton windows", () => [1, 2].aperture(1), [[1], [2]]],
  ["splitEvery chunks of exactly n (last may be short, doc)", () => [1, 2, 3, 4, 5].splitEvery(2), [[1, 2], [3, 4], [5]]],
  ["splitEvery on [] -> []", () => [].splitEvery(3), []],
  ["splitAt(i) partition", () => [1, 2, 3].splitAt(1), [[1], [2, 3]]],
  ["splitAt(0)", () => [1, 2, 3].splitAt(0), [[], [1, 2, 3]]],
  ["splitAt(n > length)", () => [1, 2, 3].splitAt(9), [[1, 2, 3], []]],
]);

// immutable updates (7552-7584)
runRows([
  ["adjust maps one index", () => [1, 2, 3].adjust(1, (v) => v * 10), [1, 20, 3]],
  ["adjust purity: source untouched (doc: a copy)", () => { const a = [1, 2, 3]; a.adjust(1, (v) => v * 10); return JSON.stringify(a) === "[1,2,3]"; }, true],
  ["update sets one index", () => [1, 2, 3].update(1, "x"), [1, "x", 3]],
  ["update purity: source untouched", () => { const a = [1, 2]; a.update(0, 9); return a[0] === 1; }, true],
  ["move from -> to", () => [1, 2, 3].move(0, 2), [2, 3, 1]],
  ["move back to front", () => [1, 2, 3].move(2, 0), [3, 1, 2]],
  ["swap", () => [1, 2, 3].swap(0, 2), [3, 2, 1]],
  ["swap/move purity: source untouched", () => { const a = [1, 2, 3]; a.swap(0, 2); a.move(0, 1); return a[0] === 1 && a[2] === 3; }, true],
  ["nth / nth out of range -> undefined", () => [[1, 2, 3].nth(1), [1, 2, 3].nth(9)], [2, undefined]],
  ["init: everything except the last", () => [1, 2, 3].init(), [1, 2]],
  ["tail: everything except the first", () => [1, 2, 3].tail(), [2, 3]],
  ["head is the documented LEGACY alias of first", () => [1, 2].head(), 1],
  ["append", () => [1, 2].append(3), [1, 2, 3]],
  ["prepend", () => [1, 2].prepend(0), [0, 1, 2]],
  ["append/prepend purity", () => { const a = [1]; a.append(2); a.prepend(0); return a.length === 1; }, true],
  ["reject is the complement of filter", () => [1, 2, 3].reject((v) => v < 2), [2, 3]],
  ["insert at i", () => ["a", "c"].insert(1, "b"), ["a", "b", "c"]],
  ["insert at 0", () => [1, 2, 3].insert(0, 0), [0, 1, 2, 3]],
  ["insertAll", () => ["a", "d"].insertAll(1, ["b", "c"]), ["a", "b", "c", "d"]],
  ["removeAt", () => [1, 2, 3].removeAt(1), [1, 3]],
  ["removeAt(0)", () => [1, 2].removeAt(0), [2]],
  ["insert/removeAt purity", () => { const a = [1, 2, 3]; a.insert(1, 9); a.removeAt(0); return a.length === 3; }, true],
]);

// zipObj/fromPairs/median/product/scan/countBy/indexBy/groupBy/reduceBy (7583-7596, 7624-7625)
runRows([
  ["zipObj", () => JSON.stringify(["a", "b"].zipObj([1, 2])), JSON.stringify({ a: 1, b: 2 })],
  ["fromPairs", () => JSON.stringify([["a", 1], ["b", 2]].fromPairs()), JSON.stringify({ a: 1, b: 2 })],
  ["zipObj/fromPairs round-trip (structural)", () => JSON.stringify(Object.entries([["a", 1], ["b", 2]].fromPairs())) === JSON.stringify([["a", 1], ["b", 2]]), true],
  ["median odd count", () => [1, 3, 2].median(), 2],
  ["median even count -> mean of middles", () => [4, 1, 3, 2].median(), 2.5],
  ["product", () => [2, 3, 4].product(), 24],
  ["product single", () => [5].product(), 5],
  ["scan running accumulation (DOC-TENSION: seed is required by the signature but the shape comment '[x0, f(x0,x1), ...]' omits it -- pin last element and length, allow first to be seed or x0)", () => {
    const r = [1, 2, 3].scan((a, v) => a + v, 0);
    return (r.length === 3 || r.length === 4) && r[r.length - 1] === 6 && (r[0] === 0 || r[0] === 1);
  }, true],
  ["countBy counts per key", () => JSON.stringify(["aa", "b", "cc"].countBy((s) => s.length)), JSON.stringify({ "2": 2, "1": 1 })],
  ["indexBy last element per key wins (structural: single key)", () => JSON.stringify([{ id: "x", v: 1 }].indexBy((o) => o.id)), JSON.stringify({ x: { id: "x", v: 1 } })],
  ["groupBy groups into a Record, keys in first-occurrence order", () => {
    const g = [{ t: "a" }, { t: "b" }, { t: "a" }].groupBy((o) => o.t);
    return JSON.stringify(Object.keys(g)) === "[\"a\",\"b\"]" && g.a.length === 2 && g.b.length === 1;
  }, true],
  ["groupBy numeric parity groups", () => {
    const g = [1, 2, 3, 4].groupBy((x) => x % 2);
    return JSON.stringify(g[1]) === "[1,3]" && JSON.stringify(g[0]) === "[2,4]";
  }, true],
  ["reduceBy per-key fold", () => JSON.stringify([{ t: "a", v: 1 }, { t: "b", v: 2 }, { t: "a", v: 3 }].reduceBy((acc, o) => acc + o.v, 0, (o) => o.t)), JSON.stringify({ a: 4, b: 2 })],
]);

// remove family/splitWhen/innerJoin/startsWith/endsWith/dropRepeats family/sortWith (7597-7616, 7612-7616)
runRows([
  // dynajs.d.ts remove family: a matcher (or value-as-matcher) removes EVERY
  // accepted element; the d.ts's old "first occurrence" comment corrected
  ["remove drops every matching element", () => [1, 2, 1].remove(1), [2]],
  ["remove accepts a matcher", () => [1, 2, 3].remove((x) => x > 1), [1]],
  ["remove absent -> unchanged copy", () => [1, 2].remove(9), [1, 2]],
  ["exclude every occurrence", () => [1, 2, 1].exclude(1), [2]],
  // removeRange(start, count) per dynajs.d.ts
  ["removeRange(start, count)", () => [1, 2, 3, 4].removeRange(1, 3), [1]],
  ["removeRange count 0 -> unchanged", () => [1, 2, 3].removeRange(1, 0), [1, 2, 3]],
  ["splitWhen at the first pred-true index", () => [1, 2, 3, 1].splitWhen((v) => v > 2), [[1, 2], [3, 1]]],
  ["splitWhen all passing -> second half starts at index 0", () => [1, 2].splitWhen((v) => v > 0), [[], [1, 2]]],
  ["splitWhen none passing", () => [1, 2].splitWhen((v) => v > 5), [[1, 2], []]],
  ["innerJoin keeps this-elements matching some other-element", () => [1, 2, 3].innerJoin((a, b) => a === b, [2, 3, 4]), [2, 3]],
  ["innerJoin objects", () => [{ id: 1 }, { id: 2 }].innerJoin((a, b) => a.id === b.id, [{ id: 1 }]).length, 1],
  ["startsWith prefix", () => [1, 2, 3].startsWith([1, 2]), true],
  ["startsWith empty prefix -> true", () => [1, 2].startsWith([]), true],
  ["startsWith mismatch -> false", () => [1, 2].startsWith([2]), false],
  ["endsWith suffix", () => [1, 2, 3].endsWith([2, 3]), true],
  ["endsWith mismatch -> false", () => [1, 2, 3].endsWith([1]), false],
  ["dropRepeats consecutive duplicates", () => [1, 1, 2, 2, 1].dropRepeats(), [1, 2, 1]],
  ["dropRepeatsWith custom eq", () => [1, 1.2, 2].dropRepeatsWith((a, b) => Math.floor(a) === Math.floor(b)), [1, 2]],
  ["dropRepeatsBy key", () => [{ k: 1 }, { k: 1 }, { k: 2 }].dropRepeatsBy((o) => o.k).length, 2],
  ["sortWith multi-comparator: age then name", () => {
    const byAge = (a, b) => a.a - b.a;
    const byName = (a, b) => (a.n < b.n ? -1 : a.n > b.n ? 1 : 0);
    return [{ n: "b", a: 1 }, { n: "a", a: 1 }, { n: "c", a: 0 }].sortWith([byAge, byName]).map((o) => o.n + o.a).join(",");
  }, "c0,a1,b1"],
]);

// transducers (7626-7632): xf protocol undocumented in dynajs.d.ts -- identity transducer only (per brief).
runRows([
  ["transduce with an identity transducer (xf protocol undocumented -- minimal row)", () => [1, 2, 3].transduce((rf) => rf, (a, v) => a + v, 0), 6],
  ["into with an identity transducer (minimal row)", () => { const r = [1, 2, 3].into([], (rf) => rf); return Array.isArray(r) && r.join(",") === "1,2,3"; }, true],
  // sequence/traverse: applicative protocol shape NOT derivable from the doc -- intentionally not asserted.
]);

// FromIndex family (7633-7649)
runRows([
  ["mapFromIndex from startIndex", () => [10, 20, 30, 40].mapFromIndex(2, (v, i) => v + "@" + i), ["30@2", "40@3"]],
  ["mapFromIndex loop=true wraps to index 0 (doc)", () => [10, 20, 30, 40].mapFromIndex(2, true, (v, i) => v + "@" + i), ["30@2", "40@3", "10@0", "20@1"]],
  ["forEachFromIndex", () => { const seen = []; [10, 20, 30, 40].forEachFromIndex(1, (v) => seen.push(v)); return seen; }, [20, 30, 40]],
  ["filterFromIndex", () => [10, 20, 30, 40].filterFromIndex(1, (v) => v >= 30), [30, 40]],
  ["findFromIndex", () => [10, 20, 30, 40].findFromIndex(1, (v) => v > 20), 30],
  ["findFromIndex miss -> undefined", () => [10, 20].findFromIndex(1, (v) => v > 99), undefined],
  ["findIndexFromIndex", () => [10, 20, 30, 40].findIndexFromIndex(0, (v) => v === 30), 2],
  ["findIndexFromIndex miss (no loop) -> -1", () => [10, 20, 30, 40].findIndexFromIndex(2, (v) => v === 10), -1],
  ["someFromIndex", () => [10, 20, 30, 40].someFromIndex(3, (v) => v === 40), true],
  ["everyFromIndex", () => [10, 20, 30, 40].everyFromIndex(2, (v) => v >= 30), true],
  // reduceFromIndex(s) covers [s..len); reduceRightFromIndex(s) mirrors it:
  // the prefix [0..s] walked downward ("start iteration at a given index",
  // dynajs.d.ts). Truthy seeds: a falsy reduce seed is dropped (doc's seed quirk).
  ["reduceFromIndex with seed", () => [10, 20, 30, 40].reduceFromIndex(1, (a, v) => a + v, 100), 190],
  ["reduceRightFromIndex mirrors over the prefix [0..s]", () => [10, 20, 30, 40].reduceRightFromIndex(1, (a, v) => a + v, 100), 130],
]);

// lazy (7650-7654)
runRows([
  ["lazy: iterates the elements", () => [...[1, 2, 3].lazy()], [1, 2, 3]],
  ["lazy: ARGUMENTS ARE IGNORED (doc) -- lazy(999) still yields 2", () => [...[1, 2].lazy(999)].length, 2],
  ["lazy composes with Iterator helpers", () => [1, 2, 3].lazy().map((x) => x * 2).toArray(), [2, 4, 6]],
]);

// deep-audit2 v2-SEC-2 regression row. Contract: ES Array growth -- a push at the
// INT32 length boundary keeps exact ES semantics (length 2147483648, element readable).
// The sparse door is asserted here. The dense door (a fast array with count == 2^31-1
// then push) needs a ~34GB live array and is unreachable on a 32GB host; the fixed
// expand_fast_array takes a clean RangeError("invalid array length") there -- the
// same contract js_allocate_fast_array pins for lengths beyond INT32_MAX -- instead
// of the pre-fix int-truncated new_size that skipped the grow and wild-stored
// values[0x7fffffff].
runRows([
  ["push at the INT32 length boundary: length 2147483648, element readable (v2-SEC-2)", () => {
    const a = new Array(2147483647);
    if (a.length !== 2147483647) return false;
    a.push(1);
    return a.length === 2147483648 && a[2147483647] === 1;
  }, true],
  ["push growth stays correct across doublings below the boundary (v2-SEC-2)", () => {
    const a = [];
    for (let i = 0; i < 100000; i++) a.push(i);
    return a.length === 100000 && a[0] === 0 && a[99999] === 99999 && a[50000] === 50000;
  }, true],
]);

// ===========================================================================
// ArrayConstructor / Map.prototype / SetConstructor (dynajs.d.ts lines 7657-7674)
// ===========================================================================
// No top-level await in this module context: run the async block as a chain
(async () => {
  const rows = [
    ["Array.repeat('x', 3)", () => Array.repeat("x", 3), ["x", "x", "x"]],
    ["Array.repeat(0, 0) -> []", () => Array.repeat(1, 0), []],
    ["Array.repeat(null, 2) keeps element identity", () => { const out = Array.repeat(null, 2); return out.length === 2 && out[0] === null && out[1] === null; }, true],
    ["Array.fromAsync over an async iterable", () => { async function* gen() { yield 1; yield 2; } return Array.fromAsync(gen()); }, [1, 2]],
    ["Array.fromAsync over an iterable of promises (awaits each)", () => Array.fromAsync([Promise.resolve(1), Promise.resolve(2)]), [1, 2]],
    ["getOrInsert inserts (and returns) the value when absent", () => { const m = new Map(); const v = m.getOrInsert("k", 5); return v === 5 && m.get("k") === 5; }, true],
    ["getOrInsert returns the EXISTING value and does not overwrite", () => { const m = new Map([["k", 5]]); return m.getOrInsert("k", 9) === 5 && m.get("k") === 5; }, true],
    ["getOrInsert omitted value inserts undefined but the key becomes present", () => { const m = new Map(); m.getOrInsert("k"); return m.has("k") && m.get("k") === undefined; }, true],
    ["getOrInsertComputed inserts fn(key) when absent", () => { const m = new Map(); const v = m.getOrInsertComputed("abc", (k) => k.length); return v === 3 && m.get("abc") === 3; }, true],
    ["getOrInsertComputed does NOT call fn when the key is present (doc)", () => { const m = new Map([["k", 1]]); let calls = 0; const v = m.getOrInsertComputed("k", () => { calls++; return 99; }); return v === 1 && calls === 0; }, true],
    ["Set.groupBy returns a Map, keys in first-occurrence order, values in input order", () => {
      const m = Set.groupBy(["apple", "avocado", "banana"], (s) => s[0]);
      return m instanceof Map && JSON.stringify(Array.from(m.keys())) === "[\"a\",\"b\"]" && JSON.stringify(m.get("a")) === "[\"apple\",\"avocado\"]";
    }, true],
    ["Set.groupBy numeric keys", () => { const m = Set.groupBy([1, 2, 3, 4], (x) => x % 2); return JSON.stringify(m.get(1)) + "|" + JSON.stringify(m.get(0)); }, "[1,3]|[2,4]"],
    ["Set.groupBy empty input -> empty Map", () => Set.groupBy([], (x) => x).size, 0],
  ];
  for (const [label, get, exp] of rows) {
    const actual = await get();
    if (typeof exp === "function") { assert(exp(actual), label + " [structural predicate]"); continue; }
    if (Array.isArray(exp)) { assertDeepEq(actual, exp, label); continue; }
    assertEq(actual, exp, label);
  }
})().then(() => {
  print("bb_proto_string_array: all tests passed (" + n + " assertions)");
}, (e) => {
  print("bb_proto_string_array FAILED after " + n + " assertions: " + (e && e.message ? e.message : e));
});
