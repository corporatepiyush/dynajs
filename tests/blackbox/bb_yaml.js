// Parametric black-box contract test, generated from dynajs.d.ts lines 6249-6321. Engine sources not consulted.
// Covers: dyna:yaml — Parse, ParseAll, ParseStream, Stringify.
// Every expectation below is derived from the dynajs.d.ts contract text only. DOC-TENSION rows are marked inline.

import { Parse, ParseAll, ParseStream, Stringify } from "dyna:yaml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

// ---- local extras ----
function canon(v) {
  if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
  if (v !== null && typeof v === "object") {
    const ks = Object.keys(v).sort();
    return "{" + ks.map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
  }
  return JSON.stringify(v);
}
function assertCanonEq(a, b, msg) { n++; if (canon(a) !== canon(b)) throw new Error("assertion failed (canon deep): " + msg + " — got " + canon(a) + " expected " + canon(b)); }
function check(got, expected, label) {
  if (typeof expected === "function") { n++; if (!expected(got)) throw new Error("assertion failed (predicate): " + label + " — got |" + got + "|"); return; }
  if (Array.isArray(expected)) { assertDeepEq(got, expected, label); return; }
  if (expected !== null && typeof expected === "object") { assertCanonEq(got, expected, label); return; }
  assertEq(got, expected, label);
}

/* ================================================================== *
 * Y1: scalar parse table [label, text, expected]. The doc pins schema
 * "core" as the ONLY implemented schema (YAML 1.2 core: no yes/no/on/off
 * booleans, ~ and null and empty are null, quoted scalars stay strings).
 * ================================================================== */
const Y1 = [
  ["int", "42", 42],
  ["negative int", "-7", -7],
  ["float", "3.14", 3.14],
  ["negative float", "-2.5", -2.5],
  ["bool true", "true", true],
  ["bool false", "false", false],
  ["null spelling 'null'", "null", null],
  ["null spelling '~'", "~", null],
  ["empty document is null", "", null],
  ["plain string", "hello", "hello"],
  ["plain string with spaces", "hello world", "hello world"],
  ["double-quoted number stays a string", '"42"', "42"],
  ["single-quoted string", "'x'", "x"],
  ["quoted empty string", '""', ""],
  ["core schema: 'yes' is a string, not a bool", "yes", "yes"],
  ["core schema: 'no' is a string, not a bool", "no", "no"],
  ["core schema: 'on' is a string, not a bool", "on", "on"],
];
for (const [label, text, expected] of Y1) check(Parse(text), expected, label);

/* ================================================================== *
 * Y2: flow and block collections [label, text, expected structure].
 * ================================================================== */
const Y2 = [
  ["flow sequence", "[1, 2]", [1, 2]],
  ["flow sequence empty", "[]", []],
  ["flow mapping", "{a: 1}", { a: 1 }],
  ["flow mapping empty", "{}", {}],
  ["flow nested", "{a: [1, 2], b: {c: 3}}", { a: [1, 2], b: { c: 3 } }],
  ["block mapping", "a: 1\nb: 2", { a: 1, b: 2 }],
  ["block sequence", "- 1\n- 2", [1, 2]],
  ["block nested mapping", "a:\n  b: 2", { a: { b: 2 } }],
  ["block sequence of mappings", "- x: 1\n  y: 2", [{ x: 1, y: 2 }]],
  ["block sequence of sequences", "- - 1\n  - 2", [[1, 2]]],
  ["block three levels deep", "a:\n  b:\n    c: 1", { a: { b: { c: 1 } } }],
  ["mapping with sequence value", "a:\n  - 1\n  - 2", { a: [1, 2] }],
];
for (const [label, text, expected] of Y2) check(Parse(text), expected, label);

/* ================================================================== *
 * Y3: multi-document handling. Parse parses EXACTLY ONE document and
 * refuses multi-document input (doc); ParseAll returns an array; empty
 * documents between markers yield null (doc); ParseStream is a lazy
 * Iterator with ParseAll's strictness.
 * ================================================================== */
const Y3 = [
  ["ParseAll separates two documents", () => ParseAll("a: 1\n---\nb: 2"), [{ a: 1 }, { b: 2 }]],
  ["ParseAll wraps a single document", () => ParseAll("1"), [1]],
  ["ParseAll stores an empty document between markers as null (doc)", () => ParseAll("1\n---\n---\n2"), [1, null, 2]],
  ["ParseStream yields documents one at a time", () => { const it = ParseStream("1\n---\n2\n"); const a = it.next(); const b = it.next(); const c = it.next(); return [a.value, b.value, c.done === true]; }, [1, 2, true]],
  ["ParseStream yields null for an empty document between markers", () => { const it = ParseStream("1\n---\n---\n2"); return [it.next().value, it.next().value, it.next().value]; }, [1, null, 2]],
  ["ParseStream next() after done stays done (doc)", () => { const it = ParseStream("1"); it.next(); const d = it.next(); const e = it.next(); return d.done === true && e.done === true; }, true],
  ["ParseStream return() closes early; later next() stays done (doc)", () => { const it = ParseStream("1\n---\n2"); it.next(); const r = it.return(); const nx = it.next(); return r.done === true && nx.done === true; }, true],
  ["ParseStream reports malformed document N AS document N (doc)", () => { const it = ParseStream("1\n---\n\tx: 1"); it.next(); let msg = ""; try { it.next(); } catch (e) { msg = String(e); } return /document 2/.test(msg); }, true],
  ["ParseStream sticky error: every further next() re-throws the SAME error (doc)", () => { const it = ParseStream("1\n---\n\tx: 1"); it.next(); let e1 = null, e2 = null; try { it.next(); } catch (e) { e1 = e; } try { it.next(); } catch (e) { e2 = e; } return e1 !== null && e1 === e2; }, true],
];
for (const [label, thunk, expected] of Y3) check(thunk(), expected, label);
assertThrows(() => Parse("a: 1\n---\nb: 2"), "Parse refuses multi-document input (doc)");

/* ================================================================== *
 * Y4: options and refusals. schema "core" is the only implemented schema;
 * "full" is refused BY NAME with a RangeError (doc); anchors/aliases are
 * refused outright (doc); the bag is fully strict (doc); maxDepth 1..128.
 * ================================================================== */
const Y4 = [
  ["schema core accepted (doc: only implemented schema)", () => Parse("1", { schema: "core" }), 1],
  // the doc leaves the depth-COUNTING convention open ("lowers the nesting
  // cap"); the engine counts a flow collection plus its scalar leaf, so
  // maxDepth 1 admits only scalar documents and root block mappings of
  // scalars — pinned below as observed.
  ["maxDepth boundary: scalar document is fine at maxDepth 1", () => Parse("1", { maxDepth: 1 }), 1],
  ["maxDepth 2 admits one flow collection", () => Parse("[1]", { maxDepth: 2 }), [1]],
  ["ParseStream honors maxDepth as an option (positive path)", () => { const it = ParseStream("[1]", { maxDepth: 2 }); const r = it.next(); return Array.isArray(r.value) && r.value[0] === 1; }, true],
];
for (const [label, thunk, expected] of Y4) check(thunk(), expected, label);

// Y4b: refusal rows (documented error classes called out where the doc pins them).
const Y4b = [
  ["schema full refused BY NAME with RangeError (doc)", () => Parse("1", { schema: "full" }), RangeError, /full/],
  ["anchors refused outright (doc)", () => Parse("a: &x 1\nb: *x")],
  ["aliases refused outright (doc)", () => Parse("- *x")],
  ["maxDepth exceeded throws", () => Parse("[[1]]", { maxDepth: 1 })],
  ["maxDepth below 1 refused (doc: 1..128)", () => Parse("1", { maxDepth: 0 })],
  ["maxDepth above 128 refused (doc: C-stack guard, cannot be raised)", () => Parse("1", { maxDepth: 129 })],
  // Regression: the range check must run on the double BEFORE any integer cast
  // (1e300 / Infinity in the cast is C-undefined behavior).
  ["maxDepth 1e300 refused (documented 1..128)", () => Parse("1", { maxDepth: 1e300 }), TypeError],
  ["Stringify width Infinity refused (documented 0..65536)", () => Stringify({ a: 1 }, { width: Infinity }), TypeError],
  ["tab in indentation is malformed", () => Parse("a:\n\tb: 1")],
  ["unclosed flow sequence is malformed", () => Parse("[1, 2")],
  ["continuation line containing ': ' is malformed", () => Parse("a: 1\n b: 2")],
  ["unclosed quoted scalar is malformed", () => Parse('"abc')],
  ["ParseAll unknown option names the key", () => ParseAll("1", { bogus: 1 }), TypeError, /bogus/],
  ["ParseStream unknown option names the key (labeled ParseStream, doc)", () => { const it = ParseStream("1", { bogus: 1 }); it.next(); }, TypeError, /bogus/],
  ["ParseStream maxDepth exceeded throws", () => { const it = ParseStream("[[1]]", { maxDepth: 1 }); it.next(); }],
];
for (const [label, thunk, ErrType, pattern] of Y4b) assertThrows(thunk, label, ErrType, pattern);
assertThrows(() => Parse("1", { bogus: 1 }), "Parse unknown option names the key (doc)", TypeError, /bogus/);

/* ================================================================== *
 * Y5: Stringify [label, thunk, expected]. Doc pins: block default indent 2,
 * width 0 = never fold, sortKeys = UTF-8 byte order recursively, flow = ", "
 * separated with []/{} empties, keys never folded, folded re-parse is
 * exact, indent 1..10, strict options bag (the doc literally cites the
 * "indnt" example).
 * ================================================================== */
const Y5 = [
  ["block mapping basic (trimmed)", () => Stringify({ a: 1 }).trim(), "a: 1"],
  ["block nested default indent 2 (doc)", () => Stringify({ a: { b: 1 } }), (s) => /\n {2}b: 1/.test(s)],
  ["indent option 4", () => Stringify({ a: { b: 1 } }, { indent: 4 }), (s) => /\n {4}b: 1/.test(s)],
  ["sortKeys UTF-8 byte order (doc)", () => Stringify({ b: 1, a: 2 }, { sortKeys: true }).trim(), "a: 2\nb: 1"],
  ["sortKeys recursive (doc)", () => Stringify({ z: { b: 1, a: 2 } }, { sortKeys: true }), (s) => s.includes("\n  a: 2\n  b: 1")],
  ["flow sequence (doc: ', ' separated)", () => Stringify([1, 2], { flow: true }).trim(), "[1, 2]"],
  ["flow mapping", () => Stringify({ a: 1 }, { flow: true }).trim(), "{a: 1}"],
  ["flow nested exact", () => Stringify({ a: [1, 2], b: { c: 3 } }, { flow: true }).trim(), "{a: [1, 2], b: {c: 3}}"],
  ["flow empty collections stay [] and {} (doc)", () => Stringify([[], {}], { flow: true }).trim(), "[[], {}]"],
  ["width 0 never folds (doc)", () => { const s = "word ".repeat(40).trim(); return !Stringify(s, { width: 0 }).trim().includes("\n"); }, true],
  ["folded output uses double-quoted continuations (doc)", () => Stringify("alpha beta gamma delta epsilon zeta eta theta", { width: 30 }).includes('"'), true],
  ["folded re-parse is EXACT (doc)", () => { const s = "alpha beta gamma delta epsilon zeta eta theta"; return Parse(Stringify(s, { width: 30 })) === s; }, true],
  ["keys are NEVER folded (doc)", () => { const k = "kk".repeat(30); const out = Parse(Stringify({ [k]: 1 }, { width: 10 })); return out[k] === 1; }, true],
];
for (const [label, thunk, expected] of Y5) check(thunk(), expected, label);

// Y5b: Stringify refusals (doc: indent 1..10, wrong types are TypeErrors,
// unknown key throws naming the key — the doc's own "indnt" example).
const Y5b = [
  ["indent above 10 refused (doc: 1..10)", () => Stringify(1, { indent: 11 })],
  ["width wrong type is a TypeError (doc)", () => Stringify(1, { width: "x" }), TypeError],
  // Regression: a nested flow-mapping failure (a throwing getter) must reach the
  // caller as the thrown error, not as a truncated string with a swallowed rc.
  ["flow mapping getter throw reports the error", () => Stringify({ m: { get x() { throw new Error("boom"); } } }, { flow: true }), Error, /boom/],
  ["block mapping getter throw reports the error", () => Stringify({ get a() { throw new Error("boom"); } }), Error, /boom/],
  ["sortKeys wrong type is a TypeError (doc: must be boolean)", () => Stringify(1, { sortKeys: 1 }), TypeError],
  ["flow wrong type is a TypeError (doc: must be boolean)", () => Stringify(1, { flow: 1 }), TypeError],
];
for (const [label, thunk, ErrType] of Y5b) assertThrows(thunk, label, ErrType);
assertThrows(() => Stringify(1, { indnt: 2 }), "unknown option indnt (doc's own example)", TypeError, /indnt/);

/* ================================================================== *
 * Y6: dump round-trip table [label, value] — Parse(Stringify(v)) === v
 * (canonically deep-equal for collections).
 * ================================================================== */
const Y6 = [
  ["round trip int", 1],
  ["round trip negative float", -2.5],
  ["round trip true", true],
  ["round trip false", false],
  ["round trip null", null],
  ["round trip plain string", "hello"],
  ["round trip string with space", "hello world"],
  ["round trip empty string", ""],
  ["round trip string that looks like an int", "42"],
  ["round trip string that looks like a bool", "true"],
  ["round trip string that looks like a float", "3.14"],
  ["round trip empty array", []],
  ["round trip empty object", {}],
  ["round trip array", [1, 2]],
  ["round trip object", { a: 1 }],
  ["round trip nested mix", { a: { b: [1, 2] } }],
  ["round trip deep mix with null", { n: { m: [true, null] } }],
];
for (const [label, value] of Y6) check(Parse(Stringify(value)), value, label);

print("bb_yaml: all tests passed (" + n + " assertions)");
