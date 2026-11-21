// covers: dyna:config.TOML.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_toml_stringify_spec");
import { TOML } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqd(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b),
        msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) { let t = false; try { fn(); } catch (e) { t = true; } assert(t, msg); }
function throwsMsg(fn, substr, msg) {
    let t = false;
    try { fn(); } catch (e) { t = String(e).indexOf(substr) >= 0; }
    assert(t, msg + " (want a throw containing: " + substr + ")");
}

__kitS.section("a = 1 b = \"x\" c = [1, 2] d = true", () => {
    eq(TOML.stringify({ a: 1, b: "x", c: [1, 2], d: true }),
       '\na = 1\nb = "x"\nc = [1, 2]\nd = true', "stringify: root object is a document");
    eq(TOML.stringify({ s: "a\nb\"c\\" }),
       '\ns = "a\\nb\\"c\\\\"', "stringify: escapes");
    eq(TOML.stringify({ n: { a: 1 } }), '\nn = {a = 1}', "stringify: nested objects stay inline");
    throws(() => TOML.stringify(3.5), "stringify refuses a scalar root");
    throws(() => TOML.stringify([1, 2]), "stringify refuses an array root");
    throws(() => TOML.stringify(), "stringify requires a value");
    const v = { x: 1, y: [true, "s"], z: { n: 1 } };
    eqd(TOML.parse(TOML.stringify(v)), v, "stringify/parse round trip");

    throwsMsg(() => TOML.parse("x = " + "1".repeat(81)), "integer out of 64-bit",
        "81-digit integer refuses by name");
    throwsMsg(() => TOML.parse("x = " + "1".repeat(127)), "integer out of 64-bit",
        "127-digit integer (max token) refuses by name");
    throwsMsg(() => TOML.parse(("a.").repeat(64) + "b = 1"), "key nesting exceeds 64",
        "65-segment dotted key refuses at the depth cap");
});

__kitS.section("f = 5.0", () => {
    eq(TOML.stringify(TOML.parse("f = 5.0")), "\nf = 5.0",
       "a parsed 5.0 stringifies as 5.0");
    eq(TOML.parse(TOML.stringify(TOML.parse("f = 5.0"))).f, 5,
       "and it round-trips to the number 5");
    eq(TOML.stringify(TOML.parse("i = 5")), "\ni = 5",
       "an int never grows a .0");
    eq(TOML.stringify(TOML.parse("n = -0.0")), "\nn = -0.0",
       "negative zero keeps its sign");
    throws(() => TOML.stringify({ a: null }),
           "null is refused (TOML 1.0 has no null)");
    throws(() => TOML.stringify({ a: undefined }),
           "undefined is refused (TOML 1.0 has no null)");
    throws(() => TOML.stringify(null), "and a null root");
    eq(JSON.stringify(TOML.parse(TOML.stringify({ a: 1, b: [2, 3] }))),
       JSON.stringify(TOML.parse("a = 1\nb = [2, 3]")),
       "a null-free document still round-trips");
});

eq(TOML.stringify({ a: -Infinity }), "\na = -inf", "stringify: -inf renders whole");

__kitS.section("a = 0.1", () => {
    // floats are written in the shortest form that reads back as the same
    // double (what String(n) prints), never through printf: no locale, no
    // 0.10000000000000001; an integer-looking result gets ".0" so it stays
    // a float (9223372036854776000 alone is an out-of-range integer)
    eq(TOML.stringify({ a: 0.1 }), "\na = 0.1", "0.1 is written as 0.1");
    eq(TOML.stringify({ a: -0 }), "\na = -0.0", "-0 keeps its sign");
    eq(TOML.stringify({ a: 2 ** 63 }), "\na = 9223372036854776000.0", "an integral float past int64 stays a float");
    eq(TOML.stringify({ a: 1e-7 }), "\na = 1e-7", "small floats use an exponent");
    let s = 5, bad = 0;
    const rnd = () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296;
    const buf = new Float64Array(1), u = new Uint32Array(buf.buffer);
    for (let i = 0; i < 20000; i++) {
        u[0] = rnd() * 2 ** 32; u[1] = rnd() * 2 ** 32;
        const v = buf[0];
        if (!isFinite(v)) continue;
        try { if (!Object.is(TOML.parse(TOML.stringify({ v })).v, v)) bad++; } catch (e) { bad++; }
    }
    for (let e = -1074; e <= 1023; e++)
        for (const m of [1, 1.5]) {
            const v = m * 2 ** e;
            if (!isFinite(v)) continue;
            try { if (!Object.is(TOML.parse(TOML.stringify({ v })).v, v)) bad++; } catch (x) { bad++; }
        }
    eq(bad, 0, "every finite double round-trips exactly (20000 random bit patterns + every exponent)");
});
__kitS.end();

if (fails) {
    print("test_toml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_toml failed");
}
print("test_toml: " + n + " assertions, 0 failures");
