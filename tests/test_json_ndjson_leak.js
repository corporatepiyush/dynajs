/* test_json_ndjson_leak.js -- Ndjson parse/stringify churn and boundaries.
 *
 * The retention half is measured by LSan at exit: run this file under
 * CONFIG_ASAN=y with ASAN_OPTIONS=detect_leaks=1 and the process must exit
 * CLEAN -- every growth buffer and string the Ndjson path hands out is
 * released before the function returns. The historical failure retained the
 * stringify growth buffer (its largest doubling) on EVERY successful call,
 * so one wide line at the tail of a churn made the whole run's resident set
 * survive to exit.
 *
 * Churn: 100k parse lines and 20k stringify rounds with growing payloads.
 * Boundary matrix: trailing partial line, empty lines, whitespace-only lines,
 * CRLF input, a single huge line, blank-line paragraphs, and a 100k-line
 * file assembled from alternating shapes.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_json_ndjson_leak.js
 *      (under CONFIG_ASAN=y ASAN_OPTIONS=detect_leaks=1 for the leak half) */
import { Ndjson } from "dyna:json";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}

/* --------------------------------------------- 100k-line parse churn */
{
    const parts = [];
    for (let i = 0; i < 100000; i++) parts.push('{"i":' + i + ',"pad":"' + "x".repeat(i % 24) + '"}');
    const big = parts.join("\n") + "\n";
    const out = Ndjson.parse(big);
    eq(out.length, 100000, "100k lines parse to 100k values");
    eq(out[0].i, 0, "first line intact");
    eq(out[99999].i, 99999, "last line intact");
}

/* ------------------------------------------- 20k stringify churn (growth) */
{
    for (let round = 0; round < 20; round++) {
        const items = [];
        for (let i = 0; i < 1000; i++)
            items.push({ r: round, i, s: "s".repeat((i * round) % 512) });
        const text = Ndjson.stringify(items);
        const back = Ndjson.parse(text);
        eq(back.length, 1000, "round " + round + ": stringify/parse round trip");
    }
    /* a single wide line at the end forces the growth buffer past every
       small-doubling watermark before the call returns */
    const wide = Ndjson.stringify([{ w: "W".repeat(1 << 20) }]);
    assert(wide.length > (1 << 20), "a 1MiB payload stringifies");
    eq(Ndjson.parse(wide)[0].w.length, 1 << 20, "and parses back at full width");
}

/* -------------------------------------------------- boundary matrix */
{
    const v = Ndjson.parse('{"a":1}');                 /* trailing partial line */
    eq(v.length, 1, "a trailing partial line (no newline) still parses");
    const v2 = Ndjson.parse('{"a":1}\n\n\n{"b":2}');   /* blank-line paragraphs */
    eq(v2.length, 2, "blank lines between values are skipped");
    eq(v2[1].b, 2, "and the second value is intact");
    eq(Ndjson.parse("\n\n\n").length, 0, "newline-only input yields no values");
    eq(Ndjson.parse("   \n\t\n  ").length, 0, "whitespace-only lines are skipped");
    eq(Ndjson.parse("").length, 0, "empty input yields no values");

    /* CRLF */
    const crlf = Ndjson.parse('{"a":1}\r\n{"b":2}\r\n');
    eq(crlf.length, 2, "CRLF input parses to two values");
    eq(crlf[0].a, 1, "CRLF first value intact");
    /* a lone CR is not a line break (pinned): it lands inside the JSON
       source and the combined line refuses as one */
    throwsMatch(() => Ndjson.parse('{"a":1}\r{"b":2}'), /line 1|control/,
        "a lone CR is content, not a line break");
    /* escaped control characters inside strings survive round trips */
    const esc = Ndjson.parse('{"a":"x\\r\\ny\\u0000z"}');
    eq(esc[0].a, "x\r\ny\u0000z", "escaped CR/LF/NUL inside strings survive");

    /* huge lines: boundary sizes around the growth doublings */
    for (const size of [1, 255, 256, 4095, 4096, 65535, 65536, 1 << 18]) {
        const line = '{"k":"' + "h".repeat(size) + '"}';
        const got = Ndjson.parse(line + "\n" + line);
        eq(got.length, 2, "two " + size + "-char payload lines parse");
        eq(got[1].k.length, size, "huge line of " + size + " keeps its payload");
    }

    /* malformed lines still name their FILE line number, blanks counted */
    throwsMatch(() => Ndjson.parse('{"a":1}\n\nbroken\n'), /line 3/,
        "the error names the 1-based file line across blanks");
    throwsMatch(() => Ndjson.parse('{"a":1}\r\n{"b":'), /line 2/,
        "CRLF lines are numbered like LF ones");
    throwsMatch(() => Ndjson.stringify([undefined]), /no JSON form|undefined/,
        "stringify refuses values with no JSON form");
    throwsMatch(() => Ndjson.stringify([1n]), /BigInt|no JSON form|form/,
        "stringify refuses a BigInt by name");
}

/* stringify -> parse identity across the whole shape mix */
{
    const items = [null, 1, "x", true, {}, [], { a: [1, { b: "c" }] },
                   "\u0000\u001f\n\"", 1e308, -0];
    const back = Ndjson.parse(Ndjson.stringify(items));
    eq(back.length, items.length, "mixed-shape round trip keeps every line");
    eq(JSON.stringify(back), JSON.stringify(items), "and every value");
}

print("test_json_ndjson_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)") );
if (fails) throw new Error(fails + " failures");
