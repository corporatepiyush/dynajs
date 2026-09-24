// flags: --std
/* test_json_ndjson.js --: the Ndjson buffer forms in dyna:json.
 *
 * The oracle is the ROUND TRIP plus dyna:stream agreement: the buffer form
 * and stream.ndjson over the same bytes must produce the same value list,
 * and a malformed line must be named by its 1-based FILE line number --
 * blank lines count toward the number, because it is a position in the
 * file, not an index into the result.
 *
 * Run: dynajs tests/test_json_ndjson.js
 */
import { Ndjson } from "dyna:json";
import * as stream from "dyna:stream";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqj(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b), msg +
        " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(t, fn, msg) {
    let e = null;
    try { fn(); } catch (ex) { e = ex; }
    assert(e !== null, msg + " (nothing thrown)");
    if (e)
        assert(e instanceof t, msg + " (threw " + e.name + ": " + e.message + ")");
    return e;
}

/* ------------------------------------------------------------------ parse */

eqj(Ndjson.parse('{"a":1}\n{"b":2}\n'), [{ a: 1 }, { b: 2 }], "two objects");
eqj(Ndjson.parse('1\n"x"\ntrue\nnull\n{"k":[]}\n'), [1, "x", true, null, { k: [] }],
    "every JSON value kind");
eqj(Ndjson.parse(""), [], "empty input, no lines");
eqj(Ndjson.parse("\n\n\n"), [], "newline-only input");
eqj(Ndjson.parse('   \n\t\n\r\n'), [], "whitespace-only lines are skipped");
eqj(Ndjson.parse('{"a":1}\n\n\n{"b":2}'), [{ a: 1 }, { b: 2 }],
    "blank lines between values are skipped");
eqj(Ndjson.parse('{"a":1}'), [{ a: 1 }], "no trailing newline is fine");
eqj(Ndjson.parse('{"a":1}\r\n{"b":2}\r\n'), [{ a: 1 }, { b: 2 }], "CRLF input");
eqj(Ndjson.parse('{"a":1}   \n'), [{ a: 1 }], "trailing spaces after the value");
{
    const r = Ndjson.parse('"x"\n');
    eq(r.length, 1, "no phantom entry after the final newline");
}

/* Malformed lines: SyntaxError naming the 1-based FILE line number. */
{
    const e = throws(SyntaxError, () => Ndjson.parse('{"a":1}\n{oops}\n{"b":2}'),
        "a malformed line throws");
    assert(e && e.message.indexOf("line 2") >= 0, "the error names line 2 (got: " +
        (e && e.message) + ")");
}
{
    const e = throws(SyntaxError, () => Ndjson.parse('\n\n\n\n\nbroken\n'),
        "line number counts blank lines too");
    assert(e && e.message.indexOf("line 6") >= 0, "blank lines count (got: " +
        (e && e.message) + ")");
}
{
    const e = throws(SyntaxError, () => Ndjson.parse('{"a":1}\n{"a":2} extra\n'),
        "trailing garbage is an error, not a dropped suffix");
    assert(e && e.message.indexOf("line 2") >= 0, "names line 2");
}
throws(SyntaxError, () => Ndjson.parse('{"a":\n1}\n'),
    "a raw newline inside a value is a line error");

/* Wrong types. */
throws(TypeError, () => Ndjson.parse(42), "parse(number) is a TypeError");
throws(TypeError, () => Ndjson.parse(null), "parse(null) is a TypeError");

/* --------------------------------------------------------------- stringify */

eq(Ndjson.stringify([{ a: 1 }, { b: [1, 2] }]), '{"a":1}\n{"b":[1,2]}\n',
    "one compact JSON per line, trailing newline");
eq(Ndjson.stringify([]), "", "no items, empty string");
eq(Ndjson.stringify(["a\nb"]), '"a\\nb"\n', "an embedded newline is escaped");
eq(Ndjson.stringify(["a\u0000b"]), '"a\\u0000b"\n', "NUL is escaped");
eq(Ndjson.stringify(["\u00e9\u4e2d\ud83d\ude00"]), '"\u00e9\u4e2d\ud83d\ude00"\n',
    "unicode passes through as itself");
eq(Ndjson.stringify([{ nested: { deep: [true, null, 1.5] } }]),
    '{"nested":{"deep":[true,null,1.5]}}\n', "no spaces anywhere");
throws(TypeError, () => Ndjson.stringify([undefined]), "undefined has no JSON form");
throws(TypeError, () => Ndjson.stringify([1, () => {}]), "a function has no JSON form");
throws(TypeError, () => Ndjson.stringify("nope"), "stringify(string) is a TypeError");
throws(TypeError, () => Ndjson.stringify(), "stringify() is a TypeError");
throws(TypeError, () => Ndjson.stringify([{ bad: 1n }]),
    "a BigInt propagates the JSON error");

/* --------------------------------------------------------------- roundtrip */

{
    const items = [
        0, -1, 2.5, true, false, null, "", "hello world",
        "line1\nline2\ttabbed", "nul:\u0000end", "\u00e9\u4e2d\ud83d\ude00",
        { a: 1, b: [1, 2, 3], c: { d: null } },
        { empty: {}, arr: [], s: "x" },
        [["nested"], ["arrays"]],
        "quote:\" backslash:\\ slash:/",
    ];
    const back = Ndjson.parse(Ndjson.stringify(items));
    eq(back.length, items.length, "roundtrip keeps the item count");
    eqj(back, items, "roundtrip is the identity for JSON-safe values");
}

async function main() {
    /* 100k-line input, inside the time budget. */
    {
        let t = "";
        const want = [];
        for (let i = 0; i < 100000; i++) {
            t += '{"i":' + i + ',"s":"v' + i + '"}\n';
            want.push({ i, s: "v" + i });
        }
        const got = Ndjson.parse(t);
        eq(got.length, 100000, "100k lines all parse");
        eqj(got[99999], want[99999], "the last of 100k is intact");
        eq(Ndjson.stringify(want).length, t.length, "100k lines round-trip byte-count");
    }
    /* The malformed-line error position inside a 100k-line input. */
    {
        let t = "";
        for (let i = 0; i < 99999; i++) t += i + "\n";
        t += "not json\n";
        const e = throws(SyntaxError, () => Ndjson.parse(t), "malformed line at 100000");
        assert(e && e.message.indexOf("line 100000") >= 0,
            "names line 100000 (got: " + (e && e.message) + ")");
    }
    /* Hostile-loop pins: raw NUL, lone surrogates, U+2028/2029. */
    {
        let e = null;
        try { Ndjson.parse("1\n2\n3\u00004\n"); } catch (ex) { e = ex; }
        assert(e instanceof SyntaxError && e.message.indexOf("line 3") >= 0,
            "a raw NUL is a line error naming line 3");
        const odd = ["\uD800", "tail\uDFFF", "a\u2028b\u2029c"];
        const backOdd = Ndjson.parse(Ndjson.stringify(odd));
        eqj(backOdd, odd, "lone surrogates and U+2028/2029 roundtrip");
    }
    /* Agreement with the streaming form (dyna:stream owns ndjson(src)). */
    {
        const items = [];
        for (let i = 0; i < 1000; i++)
            items.push({ i, s: "v" + i, even: i % 2 === 0 });
        const text = Ndjson.stringify(items);
        const streamed = [];
        /* fromBytes copies; lines split on '\n' bytes wherever they fall. */
        for await (const v of stream.ndjson(stream.fromBytes(text)))
            streamed.push(v);
        eq(streamed.length, items.length, "stream form sees every value");
        eqj(streamed, items, "buffer and stream agree on the same bytes");
    }

    if (fails === 0) print("test_json_ndjson: all " + n + " tests passed");
    else { print("test_json_ndjson: " + fails + " of " + n + " FAILED"); std.exit(1); }
}

main().catch((e) => { print("test_json_ndjson: threw", e, e && e.stack); std.exit(1); });
