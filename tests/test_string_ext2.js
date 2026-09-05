/* test_string_ext2.js — String text-manipulation methods (batch 6).
 * Run: dynajs tests/test_string_ext2.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) {
    n++;
    const A = JSON.stringify(a), B = JSON.stringify(b);
    if (A !== B) throw new Error("eq failed: " + m + " (got " + A + ", want " + B + ")");
}

/* ---- count ---- */
eq("banana".count("a"), 3, "count single char");
eq("banana".count("an"), 2, "count substring");
eq("aaaa".count("aa"), 2, "count non-overlapping");
eq("hello".count("z"), 0, "count absent");
eq("hello".count(""), 0, "count empty");
eq("abcabcabc".count("abc"), 3, "count repeated");
eq("ééé".count("é"), 3, "count wide char");

/* ---- toNumber ---- */
eq("42".toNumber(), 42, "toNumber int");
eq("3.14".toNumber(), 3.14, "toNumber float");
eq("  10  ".toNumber(), 10, "toNumber leading ws");
eq("ff".toNumber(16), 255, "toNumber hex");
eq("101".toNumber(2), 5, "toNumber binary");
eq("-7".toNumber(), -7, "toNumber negative");
assert(Number.isNaN("abc".toNumber()), "toNumber non-numeric -> NaN");
eq("1e3".toNumber(), 1000, "toNumber exponent");

/* ---- humanize ---- */
eq("user_name".humanize(), "User name", "humanize underscore");
eq("author_id".humanize(), "Author", "humanize strips _id");
eq("first-name".humanize(), "First name", "humanize dash");
eq("hello".humanize(), "Hello", "humanize simple");

/* ---- parameterize ---- */
eq("Donald E. Knuth".parameterize(), "donald-e-knuth", "parameterize");
eq("  Leading  spaces  ".parameterize(), "leading-spaces", "parameterize trim+collapse");
eq("Hello_World!".parameterize(), "hello-world", "parameterize punctuation");
eq("already-ok".parameterize(), "already-ok", "parameterize idempotent-ish");

/* ---- titleize ---- */
eq("the man from earth".titleize(), "The Man from Earth", "titleize stop words");
eq("a tale of two cities".titleize(), "A Tale of Two Cities", "titleize first word capitalized");
eq("hello world".titleize(), "Hello World", "titleize basic");
eq("THE QUICK".titleize(), "The Quick", "titleize downcases rest");

/* ---- pluralize ---- */
eq("cat".pluralize(), "cats", "plural +s");
eq("bus".pluralize(), "buses", "plural -es (s)");
eq("box".pluralize(), "boxes", "plural -es (x)");
eq("church".pluralize(), "churches", "plural -es (ch)");
eq("city".pluralize(), "cities", "plural -ies");
eq("boy".pluralize(), "boys", "plural vowel+y -> +s");
eq("man".pluralize(), "men", "plural irregular");
eq("child".pluralize(), "children", "plural irregular child");
eq("sheep".pluralize(), "sheep", "plural uncountable");
eq("knife".pluralize(), "knives", "plural irregular knife");

/* ---- singularize ---- */
eq("cats".singularize(), "cat", "singular -s");
eq("boxes".singularize(), "box", "singular box (-xes)");
eq("glasses".singularize(), "glass", "singular -sses");
eq("cities".singularize(), "city", "singular -ies");
eq("men".singularize(), "man", "singular irregular");
eq("people".singularize(), "person", "singular irregular people");
eq("sheep".singularize(), "sheep", "singular uncountable");
eq("churches".singularize(), "church", "singular churches");
/* round-trip on unambiguous regulars (the -ses stem class is inherently
 * ambiguous: buses<->bus vs roses<->rose — documented, not asserted) */
for (const w of ["cat", "box", "city", "dog", "table", "church"])
    eq(w.pluralize().singularize(), w, "round-trip " + w);

/* ---- removeTags ---- */
eq("<b>bold</b> text".removeTags(), " text", "removeTags all: drop element+content");
eq("<p>a</p><span>b</span>".removeTags(), "", "removeTags all elements");
eq("<b>bold</b> and <i>italic</i>".removeTags("b"), " and <i>italic</i>", "removeTags named keeps others");
eq("keep <x>drop</x> keep".removeTags("x"), "keep  keep", "removeTags named");
eq("no tags here".removeTags(), "no tags here", "removeTags none");
eq("<a>unclosed".removeTags("a"), "", "removeTags unclosed -> to end");
eq("plain <b>x</b>".removeTags("z"), "plain <b>x</b>", "removeTags non-matching name keeps all");

/* ---- SECURITY: pathological tag input stays O(n) (must finish fast) ---- */
{
    const big = "<a>".repeat(50000) + "x";     /* 50k unclosed opens */
    const t0 = Date.now();
    const r = big.removeTags("a");
    const dt = Date.now() - t0;
    assert(dt < 2000, "removeTags O(n) on 50k unclosed opens (" + dt + "ms)");
    assert(typeof r === "string", "removeTags returns a string");
}
{
    const big = "a".repeat(200000);
    eq(big.count("a"), 200000, "count 200k (SIMD)");
}

/* ---- works on wrapper + non-enumerable ---- */
eq(new String("cat").pluralize(), "cats", "wrapper pluralize");
{
    const d = Object.getOwnPropertyDescriptor(String.prototype, "pluralize");
    assert(d && d.enumerable === false, "pluralize non-enumerable");
}

console.log("test_string_ext2.js OK — " + n + " assertions");
