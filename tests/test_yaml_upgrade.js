/* test_yaml_upgrade.js --: Parse/ParseAll options exposing the parser's
 * fuzzer-hardening bounds.
 *
 * Probe result (plan correction): the "internal limits" are the nesting cap
 * (YML_MAX_DEPTH 128) and the outright REFUSAL of anchors/aliases/tags --
 * there are no alias tables to cap, so {maxAliases} would be fake surface
 * and is deliberately absent. Exposed:
 *   { maxDepth: 1..128 }   lower the nesting cap (the 128 ceiling is a
 *                          C-stack guard; JS can only tighten it).
 *   { schema }             "core" (default) only; "full" is refused BY NAME
 *                          with a RangeError instead of silently parsing as
 *                          core.
 * Both keys are STRICT: wrong types are TypeErrors, not coerced.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_yaml_upgrade.js
 */

import { Parse, ParseAll } from "dyna:yaml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg, needle) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor,
        msg + " (" + ctor.name + ", got " + caught + ")");
    if (needle)
        assert(caught.message.indexOf(needle) >= 0,
            msg + " (message mentions " + needle + "): " + caught.message);
}

/* ---------------- defaults unchanged ---------------- */
assert(JSON.stringify(Parse("a: [1, 2]")) === '{"a":[1,2]}',
    "Parse without options unchanged");
assert(Parse("a: 1", {}).a === 1, "empty options object ok");
assert(ParseAll("--- 1\n--- 2\n").length === 2, "ParseAll unchanged");
assert(Parse("a: 1", { schema: "core" }).a === 1, "schema core explicit ok");
assert(Parse("a: {b: {c: 3}}").a.b.c === 3, "nesting below the default cap");

/* ---------------- maxDepth ---------------- */
const deep = (d) => "k: " + "[".repeat(d) + "]".repeat(d);
assert(Parse(deep(100)).k[0][0][0] !== undefined, "100 levels under default cap");
assert(Parse(deep(100), { maxDepth: 128 }).k !== undefined,
    "maxDepth at the ceiling accepted");
throws(() => Parse(deep(100), { maxDepth: 50 }), Error, "lowered cap fires",
    "depth limit");
assert(Parse(deep(40), { maxDepth: 50 }) !== undefined,
    "input within a lowered cap passes");
throws(() => Parse("a: 1", { maxDepth: 0 }), TypeError, "maxDepth 0 refused");
throws(() => Parse("a: 1", { maxDepth: -1 }), TypeError, "maxDepth negative refused");
throws(() => Parse("a: 1", { maxDepth: 129 }), TypeError,
    "maxDepth above the ceiling refused");
throws(() => Parse("a: 1", { maxDepth: 2.5 }), TypeError,
    "maxDepth non-integer refused");
throws(() => Parse("a: 1", { maxDepth: "8" }), TypeError,
    "maxDepth string refused");
assert(Parse("a: 1", { maxDepth: null }).a === 1,
    "maxDepth null is treated as absent (bag convention)");

/* ---------------- schema ---------------- */
throws(() => Parse("a: 1", { schema: "full" }), RangeError,
    "schema full refused by name", "full schema is not implemented");
throws(() => Parse("a: 1", { schema: "bogus" }), TypeError,
    "unknown schema refused");
throws(() => Parse("a: 1", { schema: 42 }), TypeError, "schema number refused");

/* ---------------- anchors/aliases: refused outright (no maxAliases) ---- */
throws(() => Parse("&anchor v: 1"), Error, "anchors refused",
    "anchor");
throws(() => Parse("k: *alias"), Error, "aliases refused", "alias");
throws(() => Parse("k: *alias", { maxDepth: 8 }), Error,
    "alias refusal is independent of depth");

print("test_yaml_upgrade: all tests passed (" + n + " assertions)");
