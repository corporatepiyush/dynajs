// covers: dyna:json.Ndjson.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_ndjson_parse_leak");
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

__kitS.section("100k lines parse to 100k values", () => {
    const parts = [];
    for (let i = 0; i < 100000; i++) parts.push('{"i":' + i + ',"pad":"' + "x".repeat(i % 24) + '"}');
    const big = parts.join("\n") + "\n";
    const out = Ndjson.parse(big);
    eq(out.length, 100000, "100k lines parse to 100k values");
    eq(out[0].i, 0, "first line intact");
    eq(out[99999].i, 99999, "last line intact");
});

__kitS.section("a trailing partial line (no newline) still parses", () => {
    const v = Ndjson.parse('{"a":1}');
    eq(v.length, 1, "a trailing partial line (no newline) still parses");
    const v2 = Ndjson.parse('{"a":1}\n\n\n{"b":2}');
    eq(v2.length, 2, "blank lines between values are skipped");
    eq(v2[1].b, 2, "and the second value is intact");
    eq(Ndjson.parse("\n\n\n").length, 0, "newline-only input yields no values");
    eq(Ndjson.parse("   \n\t\n  ").length, 0, "whitespace-only lines are skipped");
    eq(Ndjson.parse("").length, 0, "empty input yields no values");

    const crlf = Ndjson.parse('{"a":1}\r\n{"b":2}\r\n');
    eq(crlf.length, 2, "CRLF input parses to two values");
    eq(crlf[0].a, 1, "CRLF first value intact");
    throwsMatch(() => Ndjson.parse('{"a":1}\r{"b":2}'), /line 1|control/,
        "a lone CR is content, not a line break");
    const esc = Ndjson.parse('{"a":"x\\r\\ny\\u0000z"}');
    eq(esc[0].a, "x\r\ny\u0000z", "escaped CR/LF/NUL inside strings survive");

    for (const size of [1, 255, 256, 4095, 4096, 65535, 65536, 1 << 18]) {
        const line = '{"k":"' + "h".repeat(size) + '"}';
        const got = Ndjson.parse(line + "\n" + line);
        eq(got.length, 2, "two " + size + "-char payload lines parse");
        eq(got[1].k.length, size, "huge line of " + size + " keeps its payload");
    }

    throwsMatch(() => Ndjson.parse('{"a":1}\n\nbroken\n'), /line 3/,
        "the error names the 1-based file line across blanks");
    throwsMatch(() => Ndjson.parse('{"a":1}\r\n{"b":'), /line 2/,
        "CRLF lines are numbered like LF ones");
    throwsMatch(() => Ndjson.stringify([undefined]), /no JSON form|undefined/,
        "stringify refuses values with no JSON form");
    throwsMatch(() => Ndjson.stringify([1n]), /BigInt|no JSON form|form/,
        "stringify refuses a BigInt by name");
});
__kitS.end();

print("test_json_ndjson_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)") );
if (fails) throw new Error(fails + " failures");
