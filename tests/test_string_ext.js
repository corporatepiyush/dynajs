/* test_string_ext.js — native String extension methods (unprefixed,
 * non-enumerable). Core-engine builtins present in every build.
 * Run: dynajs tests/test_string_ext.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

/* ---- batch 1: isEmpty / isBlank / first / last / from / to / chars / codes ---- */
assert("".isEmpty() === true, "isEmpty of empty");
assert(" ".isEmpty() === false, "isEmpty of a space is false");
assert("x".isEmpty() === false, "isEmpty of non-empty");

assert("".isBlank() === true, "isBlank of empty");
assert("   ".isBlank() === true, "isBlank of spaces");
assert("\t\n\r\f\v ".isBlank() === true, "isBlank of assorted whitespace");
assert("  x  ".isBlank() === false, "isBlank false with a non-space");
assert(" ".isBlank() === true, "isBlank of NBSP (matches trim)");

eq("hello".first(), "h", "first() → first char");
eq("hello".first(2), "he", "first(2)");
eq("hello".first(9), "hello", "first(n>len) clamps");
eq("hello".first(0), "", "first(0) → ''");
eq("hello".first(-1), "", "first(-1) → ''");
eq("".first(), "", "first() of empty → ''");
eq("hello".last(), "o", "last() → last char");
eq("hello".last(2), "lo", "last(2) in order");
eq("hello".last(9), "hello", "last(n>len) clamps");
eq("".last(), "", "last() of empty → ''");

eq("hello".from(1), "ello", "from(index)");
eq("hello".from(), "hello", "from() → whole");
eq("hello".from(-2), "lo", "from(-2) from end");
eq("hello".from(99), "", "from(n>len) → ''");
eq("hello".to(3), "hel", "to(index) exclusive");
eq("hello".to(), "hello", "to() → whole");
eq("hello".to(-1), "hell", "to(-1) from end");
eq("hello".to(0), "", "to(0) → ''");

eq("abc".chars(), ["a", "b", "c"], "chars()");
eq("".chars(), [], "chars() of empty → []");
eq("AB".codes(), [65, 66], "codes()");
eq("".codes(), [], "codes() of empty → []");

/* wide (UTF-16) string correctness: emoji is a surrogate pair (2 code units) */
{
    const s = "a\u{1F600}b";              /* 'a', high+low surrogate, 'b' = 4 code units */
    assert(s.length === 4, "astral char is 2 code units");
    eq(s.chars().length, 4, "chars() is code-unit based (astral = 2 entries)");
    eq(s.first(1), "a", "first on a wide string");
    eq(s.last(1), "b", "last on a wide string");
    eq("café".first(3), "caf", "first on an accented (wide) string");
    eq("café".last(1), "é", "last returns the accented char");
    assert("café".isBlank() === false, "isBlank on accented text");
}

/* rope input: a concatenation the engine may keep as a rope until linearized */
{
    let r = "";
    for (let i = 0; i < 50; i++) r += "ab";
    eq(r.first(2), "ab", "first on a (possibly rope) built string");
    eq(r.last(2), "ab", "last on a rope-built string");
    assert(r.chars().length === 100, "chars on a rope-built string");
}

/* non-enumerable + does not shadow ES methods */
assert(!Object.keys(String.prototype).includes("first"), "first is non-enumerable");
assert(Object.getOwnPropertyNames(String.prototype).includes("first"), "first is an own property");
assert(typeof "x".at === "function" && "abc".at(-1) === "c", "ES String.prototype.at untouched");
assert(typeof "x".slice === "function" && "abc".slice(1) === "bc", "ES slice untouched");

/* array-like/boxed receiver + reentrant valueOf arg must not corrupt the result */
eq("hello".first({ valueOf() { return 2; } }), "he", "first coerces an object arg");
eq(String.prototype.first.call("world", 3), "wor", "first via .call on a primitive");

/* ---- batch 2: reverse / insert / remove / removeAll / compact ---- */
eq("hello".reverse(), "olleh", "reverse");
eq("".reverse(), "", "reverse of empty");
eq("a".reverse(), "a", "reverse single");
eq("café".reverse(), "éfac", "reverse BMP wide string (accents fine)");
eq("abcabc".reverse(), "cbacba", "reverse repeats");

eq("hello".insert("!"), "hello!", "insert() appends by default");
eq("hello".insert(" world", 5), "hello world", "insert(str, index)");
eq("hello".insert("X", 0), "Xhello", "insert at front");
eq("hello".insert("X", -1), "hellXo", "insert negative index from end");
eq("hello".insert("X", 99), "helloX", "insert index>len clamps to end");
eq("hi".insert(5), "hi5", "insert coerces a non-string arg");

eq("hello".remove("l"), "helo", "remove first occurrence");
eq("hello world".remove("o"), "hell world", "remove first o");
eq("hello".remove("z"), "hello", "remove no match → unchanged");
eq("hello".remove(""), "hello", "remove empty needle → unchanged");
eq("aXbXc".removeAll("X"), "abc", "removeAll");
eq("aaa".removeAll("a"), "", "removeAll everything");
eq("ababab".removeAll("ab"), "", "removeAll multi-char");
eq("aaaa".removeAll("aa"), "", "removeAll non-overlapping (2 matches)");
eq("banana".remove("na"), "bana", "remove multi-char first");

eq("  hello   world  ".compact(), "hello world", "compact trims + collapses");
eq("a\t\n b".compact(), "a b", "compact assorted whitespace → single space");
eq("   ".compact(), "", "compact all-whitespace → ''");
eq("nospace".compact(), "nospace", "compact no whitespace unchanged");
eq("".compact(), "", "compact empty");

/* wide + rope */
{
    let r = ""; for (let i = 0; i < 40; i++) r += "ab ";
    eq(r.compact().length, 119, "compact on a rope-built string");   /* "ab "*40 → "ab ...ab" */
    eq("café crème".remove("è"), "café crme", "remove on a wide string");
    eq("café".insert("!", 2), "ca!fé", "insert into a wide string");
}
/* reentrant valueOf/toString args must not corrupt the result */
eq("hello".insert({ toString() { return "!" } }, { valueOf() { return 2 } }), "he!llo", "insert with object args");

/* compact differential oracle: native (SIMD path >= 64 narrow chars, scalar
 * below) must byte-match `s.trim().replace(/\s+/g,' ')` — an exact reference
 * because trim() and \s share this engine's lre_is_space whitespace set. Cross
 * the 64-byte threshold and include NBSP (a Latin1 whitespace byte). */
{
    const ref = s => s.trim().replace(/\s+/g, " ");
    const parts = ["word", "  ", "\t", " ", "x", "  \n ", "aVeryLongTokenIndeed", " "];
    let rng = 123456789;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 4000; t++) {
        let s = "";
        const par8 = 1 + ((rng >> 3) % 40);          /* vary length across the 64 threshold */
        for (let k = 0; k < par8; k++) s += parts[(rand() * parts.length) | 0];
        eq(s.compact(), ref(s), "compact differential t=" + t + " len=" + s.length);
    }
    /* explicit long narrow with NBSP exercises the SIMD path's derived ws set */
    const nb = ("alpha\u00A0\u00A0beta   gamma\t").repeat(8);  /* > 64, narrow, NBSP */
    eq(nb.compact(), ref(nb), "compact SIMD path collapses NBSP runs");
    assert(nb.length >= 64 && ![...nb].some(c => c.charCodeAt(0) > 255), "NBSP test string is long + narrow");
}

/* ---- batch 3: shift / pad / capitalize / underscore / dasherize / spacify / camelize ---- */
eq("a".shift(1), "b", "shift +1");
eq("hello".shift(1), "ifmmp", "shift word");
eq("ifmmp".shift(-1), "hello", "shift -1 reverses +1");
eq("abc".shift(0), "abc", "shift 0 is identity");
eq("".shift(5), "", "shift empty");
{   /* shift wraps mod 2^16, per fromCharCode */
    const s = String.fromCharCode(0xFFFF).shift(1);
    assert(s.charCodeAt(0) === 0, "shift wraps 0xFFFF+1 -> 0");
}

eq("wasabi".pad(8, "-"), "-wasabi-", "pad center dash");
eq("hi".pad(5), " hi  ", "pad center space (floor front, ceil back)");
eq("hi".pad(6), "  hi  ", "pad even");
eq("hello".pad(3), "hello", "pad no-op when already long enough");
eq("x".pad(5, "ab"), "abxab", "pad multi-char cycles");
eq("x".pad(5, ""), "x", "pad empty padding is a no-op");

eq("HELLO".capitalize(true), "Hello", "capitalize(lower) downcases rest");
eq("hello world".capitalize(), "Hello world", "capitalize default: first letter only");
eq("hello world".capitalize(false, true), "Hello World", "capitalize(all) every word");
eq("hello WORLD".capitalize(), "Hello WORLD", "capitalize default leaves rest untouched");
eq("o'clock".capitalize(false, true), "O'clock", "capitalize keeps apostrophe intra-word");
eq("rew-mid".capitalize(false, true), "Rew-Mid", "capitalize(all) treats dash as boundary");
eq("élan".capitalize(), "Élan", "capitalize unicode first letter");
eq("".capitalize(), "", "capitalize empty");
eq("123abc".capitalize(true), "123abc", "capitalize leading digits");

eq("capsLock".dasherize(), "caps-lock", "dasherize camel");
eq("a-farewell-to-arms".underscore(), "a_farewell_to_arms", "underscore dashes");
eq("helloWorld".underscore(), "hello_world", "underscore camel");
eq("HTMLParser".underscore(), "html_parser", "underscore acronym boundary");
eq("innerHTML".underscore(), "inner_html", "underscore trailing acronym");
eq("camelCase".spacify(), "camel case", "spacify");
eq("The   Quick".underscore(), "the_quick", "underscore collapses whitespace runs");
eq("moz-border-radius".camelize(false), "mozBorderRadius", "camelize lower");
eq("moz-border-radius".camelize(), "MozBorderRadius", "camelize upper (default)");
eq("hello_world".camelize(), "HelloWorld", "camelize from snake");
eq("".underscore(), "", "underscore empty");

{   /* differential oracle: a JS reference mirroring the C hump/collapse rules,
     * fuzzed over random ASCII/BMP tokens (code-unit semantics match). NOTE: a
     * DOCUMENTED divergence from the conventional inflector behaviour — we
     * collapse EVERY delimiter run (including a literal '_') to one separator,
     * giving cleaner snake_case. */
    const cls = c => /[A-Z]/.test(c) ? 2 : /[a-z0-9]/.test(c) ? 1 : 0;
    const isDelim = c => c === "-" || c === "_" || /\s/.test(c);
    function inflectRef(s, sep) {
        let out = "", prevCls = 0, gotSep = 0;
        for (let i = 0; i < s.length; i++) {
            const c = s[i];
            if (isDelim(c)) { if (!gotSep) { out += sep; gotSep = 1; } prevCls = 0; continue; }
            const cl = cls(c);
            if (cl === 2) {
                let hump = prevCls === 1;
                if (!hump && prevCls === 2) hump = /[a-z]/.test(s[i + 1] || "");
                if (hump && !gotSep) out += sep;
            }
            out += c.toLowerCase();
            gotSep = 0; prevCls = cl;
        }
        return out;
    }
    function camelRef(s, upper = true) {
        let out = "", prevCls = 0, cap = upper ? 1 : 0;
        for (let i = 0; i < s.length; i++) {
            const c = s[i];
            if (isDelim(c)) { cap = 1; prevCls = 0; continue; }
            const cl = cls(c);
            if (cl === 2) {
                let hump = prevCls === 1;
                if (!hump && prevCls === 2) hump = /[a-z]/.test(s[i + 1] || "");
                if (hump) cap = 1;
            }
            out += cap ? c.toUpperCase() : c.toLowerCase();
            cap = 0; prevCls = cl;
        }
        return out;
    }
    const toks = ["Foo", "bar", "HTML", "id", "42", "-", "_", " ", "aB", "Xy", "café".slice(0,3)];
    let rng = 987654321;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 3000; t++) {
        let s = "";
        const nt = 1 + ((rng >> 5) % 6);
        for (let k = 0; k < nt; k++) s += toks[(rand() * toks.length) | 0];
        eq(s.underscore(), inflectRef(s, "_"), "underscore differential t=" + t + " [" + s + "]");
        eq(s.dasherize(), inflectRef(s, "-"), "dasherize differential t=" + t);
        eq(s.spacify(), inflectRef(s, " "), "spacify differential t=" + t);
        eq(s.camelize(), camelRef(s, true), "camelize upper differential t=" + t);
        eq(s.camelize(false), camelRef(s, false), "camelize lower differential t=" + t);
    }
}

{   /* reentrancy: a valueOf arg must not corrupt the receiver (coerce-first) */
    let hits = 0;
    const eviln = { valueOf() { hits++; return 2; } };
    eq("abcd".shift(eviln), "cdef", "shift coerces object arg via valueOf");
    assert(hits === 1, "shift valueOf ran exactly once");
    eq("x".pad({ valueOf() { return 5; } }, "-"), "--x--", "pad coerces num arg");
}

/* ---- batch 3b: truncate / truncateOnWord ---- */
eq("just sittin on the dock".truncate(18), "just sittin on the...", "truncate right, default from");
eq("sittin on the dock".truncate(10, "left"), "...n the dock", "truncate left keeps the tail");
eq("hello".truncate(10), "hello", "truncate no-op when short");
eq("hello".truncate(5), "hello", "truncate no-op at exact length");
eq("hello world".truncate(5), "hello...", "truncate right, ellipsis is extra");
eq("hello world".truncate(5, "right", "…"), "hello…", "truncate custom ellipsis");
eq("here we go".truncateOnWord(5), "here...", "truncateOnWord trims to word");
eq("here we go".truncateOnWord(6), "here...", "truncateOnWord backs off a partial word");
eq("abcdefghij".truncate(4, "middle"), "ab...ij", "truncate middle even");
eq("abcdefghij".truncate(5, "middle"), "abc...ij", "truncate middle odd -> front");
eq("hi".truncate(-3), "...", "truncate negative length clamps to 0 (ellipsis still added)");

{   /* differential oracle mirroring the C truncation rules, fuzzed */
    const isWs = c => /\s/.test(c);
    function wordPrefixCut(s, want) {
        let cut = Math.min(want, s.length);
        if (cut < s.length && !isWs(s[cut]) && cut > 0 && !isWs(s[cut - 1]))
            while (cut > 0 && !isWs(s[cut - 1])) cut--;
        while (cut > 0 && isWs(s[cut - 1])) cut--;
        return cut;
    }
    function wordSuffixStart(s, want) {
        let start = want >= s.length ? 0 : s.length - want;
        if (start > 0 && !isWs(s[start]) && !isWs(s[start - 1]))
            while (start < s.length && !isWs(s[start])) start++;
        while (start < s.length && isWs(s[start])) start++;
        return start;
    }
    function truncRef(s, length, from, ell, onWord) {
        if (length < 0) length = 0;
        if (s.length <= length) return s;
        if (from === "left") {
            const ss = onWord ? wordSuffixStart(s, length) : s.length - length;
            return ell + s.slice(ss);
        }
        if (from === "middle") {
            const front = (length + 1) >> 1, back = length - front;
            const fc = onWord ? wordPrefixCut(s, front) : front;
            const ss = onWord ? wordSuffixStart(s, back) : s.length - back;
            return s.slice(0, fc) + ell + s.slice(ss);
        }
        const fc = onWord ? wordPrefixCut(s, length) : length;
        return s.slice(0, fc) + ell;
    }
    const words = ["alpha", "b", "quick", "fox", "  ", " ", "hi", "x"];
    let rng = 424242;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 2500; t++) {
        let s = "";
        const nt = 1 + ((rng >> 4) % 8);
        for (let k = 0; k < nt; k++) s += words[(rand() * words.length) | 0] + " ";
        const length = (rand() * (s.length + 3)) | 0;
        for (const from of ["right", "left", "middle"]) {
            eq(s.truncate(length, from), truncRef(s, length, from, "...", false), "truncate diff t=" + t);
            eq(s.truncateOnWord(length, from), truncRef(s, length, from, "...", true), "truncateOnWord diff t=" + t);
        }
    }
}

{   /* reentrancy: length/from/ellipsis coerced before the receiver is read */
    eq("hello world".truncate({ valueOf() { return 5; } }), "hello...", "truncate coerces length");
    eq("hello world".truncate(5, { toString() { return "right"; } }, "!"), "hello!", "truncate coerces from + ellipsis");
}

/* ---- batch 4: HTML escape / unescape / stripTags (SIMD scan + scalar oracle) ---- */
eq("a<b>&c".escapeHTML(), "a&lt;b&gt;&amp;c", "escapeHTML basic");
eq('"\'quotes'.escapeHTML(), '"\'quotes', "escapeHTML leaves quotes unescaped");
eq("a&lt;b&gt;&amp;c".unescapeHTML(), "a<b>&c", "unescapeHTML named");
eq("&nbsp;&quot;&apos;".unescapeHTML(), " \"'", "unescapeHTML nbsp/quot/apos");
eq("&#65;&#x42;&#x1F600;".unescapeHTML(), "AB\u{1F600}", "unescapeHTML numeric dec/hex/astral");
eq("&bogus; &amp".unescapeHTML(), "&bogus; &amp", "unescapeHTML leaves unknown/unterminated literal");
eq("<p>just <b>some</b> text</p>".stripTags(), "just some text", "stripTags keeps inner text");
eq("a<>b".stripTags(), "a<>b", "stripTags leaves empty <>");
eq("open <tag no close".stripTags(), "open <tag no close", "stripTags unterminated is literal");

{   /* differential oracles over BOTH the scalar (<64) and SIMD (>=64) paths */
    const escRef = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    const stripRef = s => s.replace(/<[^]+?>/g, "");        /* [^] = any incl newline, matches C */
    const toks = ["hello", " ", "&", "<b>", "</b>", "world", ">", "<", "x&y", "café".slice(0,3), "\n", "<a href>"];
    let rng = 20240724;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 3000; t++) {
        let s = "";
        const nt = 1 + ((rng >> 3) % 40);                  /* spans both sides of the 64B gate */
        for (let k = 0; k < nt; k++) s += toks[(rand() * toks.length) | 0];
        eq(s.escapeHTML(), escRef(s), "escapeHTML differential t=" + t + " len=" + s.length);
        eq(s.stripTags(), stripRef(s), "stripTags differential t=" + t + " len=" + s.length);
        /* round-trip: escapeHTML then unescapeHTML restores the original (no bare & to confuse) */
        const clean = s.replace(/&/g, "n");
        eq(clean.escapeHTML().unescapeHTML(), clean, "escape/unescape round-trip t=" + t);
    }
    /* explicit long narrow input to force the SIMD path */
    const big = "plain text <b>bold</b> & more ".repeat(8);   /* >64, narrow */
    assert(big.length >= 64 && ![...big].some(c => c.charCodeAt(0) > 255), "HTML SIMD input is long+narrow");
    eq(big.escapeHTML(), escRef(big), "escapeHTML SIMD path");
    eq(big.stripTags(), stripRef(big), "stripTags SIMD path");
    /* wide-string path (specials are ASCII, rest is wide) */
    const w = "π<b>δ</b>&ε".repeat(10);
    eq(w.escapeHTML(), escRef(w), "escapeHTML wide path");
    eq(w.stripTags(), stripRef(w), "stripTags wide path");
}

/* ---- batch 5: words / lines (SIMD scan) + base64 (SIMD kernel) ---- */
eq("  the quick  brown\tfox ".words(), ["the", "quick", "brown", "fox"], "words basic");
eq("".words(), [], "words empty -> []");
eq("   ".words(), [], "words all-ws -> []");
eq("one".words(), ["one"], "words single");
eq("a\nb\r\nc".lines(), ["a", "b", "c"], "lines LF + CRLF");
eq("  \n line1 \n\n line3 \n ".lines(), ["line1 ", "", " line3"], "lines trim whole, keep interior spaces");
eq("".lines(), [""], "lines empty -> ['']");
eq("solo".lines(), ["solo"], "lines single");
eq("Man".encodeBase64(), "TWFu", "base64 Man");
eq("hello".encodeBase64(), "aGVsbG8=", "base64 hello (padding)");
eq("hi".encodeBase64(), "aGk=", "base64 hi");
eq("TWFu".decodeBase64(), "Man", "base64 decode");
eq("café ☕ π".encodeBase64().decodeBase64(), "café ☕ π", "base64 unicode round-trip");
{ let threw = false; try { "not valid!!".decodeBase64(); } catch (e) { threw = true; } assert(threw, "decodeBase64 throws on invalid"); }

{   /* differential oracles, fuzzed over BOTH scalar and SIMD (>=64B) paths */
    const wordsRef = s => s.match(/\S+/g) || [];
    const linesRef = s => {
        const t = s.replace(/^[ \t\r\n]+|[ \t\r\n]+$/g, "");
        if (t === "") return [""];
        return t.split("\n").map(l => l.endsWith("\r") ? l.slice(0, -1) : l);
    };
    const toks = ["word", " ", "  ", "\t", "\n", "\r\n", "aVeryLongUnbrokenTokenThatExceedsSixtyFourBytesForSureXXXXXXXXXXXX", "x", "\r"];
    let rng = 55555;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 3000; t++) {
        let s = "";
        const nt = 1 + ((rng >> 3) % 30);
        for (let k = 0; k < nt; k++) s += toks[(rand() * toks.length) | 0];
        eq(s.words(), wordsRef(s), "words differential t=" + t + " len=" + s.length);
        eq(s.lines(), linesRef(s), "lines differential t=" + t + " len=" + s.length);
    }
    /* base64 round-trip fuzz (ASCII + unicode) */
    for (let t = 0; t < 500; t++) {
        let s = "";
        const nc = (rand() * 40) | 0;
        for (let k = 0; k < nc; k++) s += String.fromCodePoint(1 + ((rand() * 0x2000) | 0));
        eq(s.encodeBase64().decodeBase64(), s, "base64 round-trip t=" + t);
    }
    /* explicit long narrow input forces the lines SIMD path */
    const big = ("line of text number here\n").repeat(6);   /* >64, narrow, many \n */
    assert(big.length >= 64, "lines SIMD input long");
    eq(big.lines(), linesRef(big), "lines SIMD path");
}

/* ---- batch 5b: escapeURL / unescapeURL (delegate to spec URI builtins) ---- */
eq("a b/c?d=e&f".escapeURL(), encodeURI("a b/c?d=e&f"), "escapeURL = encodeURI");
eq("a b/c?d=e&f".escapeURL(true), encodeURIComponent("a b/c?d=e&f"), "escapeURL(true) = encodeURIComponent");
eq("a%20b%2Fc".unescapeURL(), decodeURIComponent("a%20b%2Fc"), "unescapeURL = decodeURIComponent");
eq("a%20b%2Fc".unescapeURL(true), decodeURI("a%20b%2Fc"), "unescapeURL(true) = decodeURI (asymmetric)");
{
    let rng = 31337;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 500; t++) {
        let s = "";
        const nc = (rand() * 30) | 0;
        for (let k = 0; k < nc; k++) s += String.fromCodePoint(0x20 + ((rand() * 0x3000) | 0));
        eq(s.escapeURL(), encodeURI(s), "escapeURL differential t=" + t);
        eq(s.escapeURL(true), encodeURIComponent(s), "escapeURL(true) differential t=" + t);
        eq(s.escapeURL(true).unescapeURL(), s, "URL component round-trip t=" + t);
    }
}

print("test_string_ext: all tests passed (" + n + " assertions)");
