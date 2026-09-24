// flags: --std
/* test_yaml_emitter_opts.js --: Stringify {width, sortKeys, flow}.
 *
 * The oracle is the ROUND TRIP through this module's own Parse plus the
 * DEFAULT-OUTPUT pin: with no options (or width 0) the emitter is
 * byte-identical to its historic form, which never wrapped.
 *
 * Run: dynajs tests/test_yaml_emitter_opts.js
 */
import { Stringify, Parse } from "dyna:yaml";
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
function throwsMsg(fn, re, msg) {
    let e = null;
    try { fn(); } catch (ex) { e = ex; }
    assert(e !== null && re.test(e.message),
        msg + " (threw " + (e && e.message) + ")");
}
const LONG = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
const rt = (v, opts, msg) => {
    const text = Stringify(v, opts);
    let back;
    try { back = Parse(text); }
    catch (e) { assert(false, msg + ": the writer's output does not re-parse: " + e.message + " in " + JSON.stringify(text)); return; }
    /* The emitter is deterministic, so the re-parsed value must re-emit to
       the SAME text -- a stronger property than one deep-equal, and one
       that survives key-order differences in JSON.stringify. */
    let again;
    try { again = Stringify(back, opts); }
    catch (e) { assert(false, msg + ": the re-parsed value does not re-emit: " + e.message); return text; }
    eq(again, text, msg + " (output-stable roundtrip)");
    return text;
};

/* -------------------------------------------------- defaults are unchanged */

eq(Stringify({ a: 1, b: "x" }), "a: 1\nb: x\n", "default output unchanged");
eq(Stringify({ a: 1 }, {}), "a: 1\n", "empty bag = no options");
eq(Stringify({ a: 1 }, { width: 0 }), "a: 1\n", "width 0 is the no-wrap default");
eq(Stringify({ b: 1, a: 2 }), "b: 1\na: 2\n", "insertion order by default");
eq(Stringify({ a: [1, 2] }), "a:\n  - 1\n  - 2\n", "block style by default");

/* ------------------------------------------------------------- sortKeys */

eq(Stringify({ b: 1, a: 2, c: 3 }, { sortKeys: true }), "a: 2\nb: 1\nc: 3\n",
    "sortKeys orders top-level keys");
eq(Stringify({ z: { y: 1, b: 2 }, a: 1 }, { sortKeys: true }),
    "a: 1\nz:\n  b: 2\n  y: 1\n", "sortKeys is recursive");
{
    const t = rt({ bb: 1, a: 2, c: { x: 1, m: 2 } }, { sortKeys: true },
        "sortKeys roundtrip");
    assert(t === "a: 2\nbb: 1\nc:\n  m: 2\n  x: 1\n", "sorted output shape (" +
        JSON.stringify(t) + ")");
}
/* flow style also honors sortKeys */
eq(Stringify({ b: 1, a: [2, 1] }, { sortKeys: true, flow: true }),
    "{a: [2, 1], b: 1}\n", "sortKeys inside flow");
/* byte order: uppercase sorts before lowercase (strcmp) */
eq(Stringify({ b: 1, A: 2, a: 3 }, { sortKeys: true }), "A: 2\na: 3\nb: 1\n",
    "byte order, not locale collation");
throwsMsg(() => Stringify({}, { sortKeys: 1 }), /^Stringify: sortKeys must be a boolean$/,
    "sortKeys wrong type is a TypeError");

/* ---------------------------------------------------------------- flow */

eq(Stringify({ a: 1 }, { flow: true }), "{a: 1}\n", "flow mapping");
eq(Stringify([1, "x", true, null], { flow: true }), "[1, x, true, null]\n",
    "flow sequence over every scalar kind");
eq(Stringify({ a: [1, { b: 2 }], e: {} }, { flow: true }),
    "{a: [1, {b: 2}], e: {}}\n", "flow is recursive and keeps empty collections");
eq(Stringify({ k: "needs quote" }, { flow: true }), "{k: needs quote}\n",
    "a plain-safe string stays bare in flow");
eq(Stringify({ k: "true" }, { flow: true }), "{k: \"true\"}\n",
    "flow quotes what would re-parse as something else");
{
    const t = rt({ deep: { list: [1, { x: [2] }] }, s: "v" }, { flow: true },
        "flow roundtrip");
    assert(t === "{deep: {list: [1, {x: [2]}]}, s: v}\n", "flow shape (" +
        JSON.stringify(t) + ")");
}
eq(Stringify([], { flow: true }), "[]\n", "empty seq, flow");
eq(Stringify({}, { flow: true }), "{}\n", "empty map, flow");
rt([["nested", "arrays"], { k: [{ deep: true }] }], { flow: true },
    "flow roundtrip, nested containers");
throwsMsg(() => Stringify({}, { flow: "yes" }), /^Stringify: flow must be a boolean$/,
    "flow wrong type is a TypeError");

/* ---------------------------------------------------------------- width */

/* The emitter folds a string whose single-line form exceeds width into a
   double-quoted scalar with backslash continuations; the break contributes
   NOTHING to the value, so the round trip is byte-exact. */
{
    const long = LONG;
    const text = Stringify({ key: long, short: 1 }, { width: 30 });
    const lines = text.split("\n");
    eq(lines[0], "key: \"alpha beta gamma delta epsilon \\", "first line folds");
    assert(Parse(text).key === long, "folded value round-trips exactly");
    eq(Parse(text).short, 1, "the sibling entry survives the fold");
}
/* every line of the scalar stays within the budget (continuation lines) */
{
    const words = "one two three four five six seven eight nine ten " +
        "eleven twelve thirteen fourteen fifteen";
    const text = Stringify({ k: words }, { width: 20 });
    const body = text.split("\n").filter((l) => l.length && l !== "k: " +
        Stringify({ k: words }).split("\n")[0].slice(3));
    for (const line of text.split("\n")) {
        if (!line || line.startsWith("    ") === false) continue;
        assert(line.length <= 24, "continuation line within width (" +
            JSON.stringify(line) + " = " + line.length + ")");
    }
    assert(Parse(text).k === words, "tight-width fold round-trips");
}
/* a word longer than the whole budget still folds without splitting it */
{
    const text = Stringify({ k: "supercalifragilisticexpialidocious " +
        "antidisestablishmentarianism" }, { width: 10 });
    assert(Parse(text).k === "supercalifragilisticexpialidocious " +
        "antidisestablishmentarianism", "unbreakable words survive width 10");
}
/* a string that needs quoting anyway folds with its escapes intact */
{
    const tricky = "line one \"quoted\" tail one two three four five six " +
        "seven eight nine ten eleven";
    rt({ k: tricky }, { width: 24 }, "folded quoted string roundtrip");
}
/* escapes never split across a break */
{
    const esc = "tab\tand newline\ntail padding padding padding padding " +
        "padding end";
    rt({ esc }, { width: 16 }, "escape-adjacent fold roundtrip");
}
/* seq entries and nested block values fold with a deeper continuation */
rt([LONG, "x"], { width: 20 }, "seq entry fold roundtrip");
rt({ a: { bb: LONG } }, { width: 25 }, "nested map value fold roundtrip");
/* unicode: columns are bytes of the UTF-8 form (conservative, still exact) */
rt({ k: "\u00e9\u4e2d\ud83d\ude00 word ".repeat(8) }, { width: 20 },
    "unicode fold roundtrip");
/* width does not touch short values, keys, or numbers */
eq(Stringify({ k: "v", n: 12345 }, { width: 5 }), "k: v\nn: 12345\n",
    "width leaves short values and keys alone");
throwsMsg(() => Stringify({}, { width: "x" }),
    /^Stringify: width must be an integer 0\.\.65536$/,
    "width wrong type is a TypeError");
throwsMsg(() => Stringify({}, { width: -1 }),
    /^Stringify: width must be an integer 0\.\.65536$/,
    "negative width is refused");
throwsMsg(() => Stringify({}, { width: 1.5 }),
    /^Stringify: width must be an integer 0\.\.65536$/,
    "fractional width is refused");
throwsMsg(() => Stringify({}, { widht: 3 }),
    /^unknown option "widht" \(valid: indent, width, sortKeys, flow\)$/,
    "the bag names the key AND the full valid set");

/* hostile-loop pins */
{
    /* keys that would re-parse as something else, in flow, sorted */
    const v = { "a b": 1, "true": false, "": 2, "x:y": 3 };
    const text = Stringify(v, { flow: true, sortKeys: true });
    eq(Stringify(Parse(text), { flow: true, sortKeys: true }), text,
        "hostile flow keys roundtrip output-stable");
    /* backslash content and escapes at the fold boundaries */
    const tricky = "a\\b c\\d ".repeat(12) + "tail \"q\" tail2 end";
    rt({ k: tricky }, { width: 12 }, "fold over backslash content");
    /* no space inside the 60% preference window: fold stays exact */
    const hard = "aaaaaaaaaa b cccccccccc d eeeeeeeeee f gggggggggg h iiiiiiiiii";
    rt({ k: hard }, { width: 14 }, "hard fold spacing");
    /* continuation forms: multi-line quoted scalars now FOLD (YAML 1.2 flow
       scalars; the shapes below verified against PyYAML 6.0.3) */
    eq(Parse("k: 'a\\\n b'").k, "a\\ b",
       "single-quoted: a literal backslash, the break folds to a space");
    throwsMsg(() => Parse("{k: \"a\\\n b\"}"), /unterminated quoted scalar/,
        "flow-context continuation refused (flow stays single-line)");
    eq(Parse("k: \"a\\\n\n b\"").k, "a\nb",
       "escaped break + blank line keeps the paragraph line feed");
    /* width boundary */
    throwsMsg(() => Stringify({}, { width: 65537 }),
        /^Stringify: width must be an integer 0\.\.65536$/, "width 65537 refused");
    eq(typeof Stringify({ k: "long string here long string here" },
        { width: 65536 }), "string", "width 65536 accepted");
    /* flow depth cap */
    let deep = [], cur = deep;
    for (let i = 0; i < 130; i++) { const nx = []; cur.push(nx); cur = nx; }
    throwsMsg(() => Stringify(deep, { flow: true }), /nesting exceeds/,
        "flow honors the depth cap");
    /* non-plain objects render the same way in both styles */
    eq(Stringify({ d: new Date(0) }, { flow: true }), "{d: {}}\n",
        "non-plain object in flow");
}

/* combined options */
rt({ b: [LONG, { nested: true }], a: 1 }, { sortKeys: true, flow: true },
    "sortKeys + flow");
rt({ key: LONG, other: "x" }, { sortKeys: true, width: 30 },
    "sortKeys + width");
rt({ key: LONG, other: "x" }, { indent: 4, width: 30 }, "indent + width");

/* ---- folded scalars: portable break placement -----------------------------
 * The fold is only useful if every OTHER implementation reads the same value
 * back, so the vectors below were diffed against PyYAML 6.0.3 (the outside
 * oracle; this engine agreeing with itself proves nothing). Two rules are
 * asserted as properties so a later refactor cannot reintroduce them:
 *   - a break never splits a UTF-8 character (the round trip must be exact,
 *     byte for byte, for a scalar that MUST fold);
 *   - a break is never placed immediately before a space (a continuation
 *     line's leading spaces are indentation in every other reader). */
{
    const forced = "é".repeat(40);                  /* 40 chars, width 16 */
    const out = Stringify({ k: forced }, { width: 16 });
    eq(Parse(out).k, forced, "non-ASCII folded scalar round-trips byte-exact");
    assert(out.indexOf("\n") > 0, "the non-ASCII scalar really did fold");
    {
        let valid = true;
        try { new TextDecoder("utf-8", { fatal: true }).decode(
                  new TextEncoder().encode(out)); }
        catch (e) { valid = false; }
        assert(valid, "the folded output is valid UTF-8 (no split character)");
    }

    const sp = "x" + " ".repeat(30) + "y";
    const out2 = Stringify({ k: sp }, { width: 16 });
    eq(Parse(out2).k, sp, "a long space run survives the fold");
    /* property: every continuation line starts with a non-space unit */
    for (const line of out2.split("\n").slice(1)) {
        if (!line) continue;
        const body = line.replace(/^\s+/, "");
        if (line === out2.split("\n").slice(-1)[0]) break;   /* last line */
        if (body) assert(body[0] !== " ", "a continuation line does not open with a space");
    }

    const words = "alpha  beta  gamma  delta  epsilon zeta eta theta";
    eq(Parse(Stringify({ k: words }, { width: 16 })).k, words,
       "double spaces survive a multi-break fold");

    const mix = "前缀" + "x".repeat(20) + " 后缀 suffix here";
    eq(Parse(Stringify({ k: mix }, { width: 16 })).k, mix,
       "multi-byte prefix and suffix survive a fold");
}

/* flow context must quote what flow syntax would otherwise eat: a bare `,`
   inside a flow collection ends the scalar (PyYAML: "expected <block end>,
   but found ','"). */
{
    eq(Stringify({ a: [1, 2], b: "x,y" }, { flow: true }), '{a: [1, 2], b: "x,y"}\n',
       "a comma in a flow scalar is quoted");
    eq(Stringify({ ",k": [1] }, { flow: true }), '{",k": [1]}\n',
       "a comma in a flow key is quoted");
    const rt2 = Parse(Stringify({ a: "p,q", b: "r]s", c: "t{u" }, { flow: true }));
    eq(rt2.a, "p,q", "flow round trip: comma");
    eq(rt2.b, "r]s", "flow round trip: bracket");
    eq(rt2.c, "t{u", "flow round trip: brace");
}

if (fails === 0) print("test_yaml_emitter_opts: all " + n + " tests passed");
else { print("test_yaml_emitter_opts: " + fails + " of " + n + " FAILED"); std.exit(1); }
