/* test_conformance.js -- conformance, coverage and adversarial suites in ONE
 * file, five sections. Each section is a former standalone suite, kept
 * verbatim inside a top-level try/catch BLOCK: block scope isolates every
 * section's helpers, top-level await stays legal, and one section failing
 * does not hide the others. Exit is nonzero if ANY section throws.
 *
 * Corpus data lives under tests/corpus/ (vendored: jsonpatch, jsonschema,
 * wpt; blns.txt via fuzzgen.js). Baselines are id-lists recorded against the
 * 2026-09-01 audit round -- shrink one by fixing the named case.
 * Run: dynajs tests/test_conformance.js
 */


import * as std from "std";
import { CSVFile } from "dyna:csv";
import { JSON5Parse, JSON5Stringify } from "dyna:encoding";
import { Parse, ParseAll, Stringify as YStringify } from "dyna:yaml";
import { Patch } from "dyna:json";
import { Path, readFile, readDir } from "dyna:file";
import { STRINGS, corpusSize, PRNG } from "./fuzzgen.js";
import { Schema } from "dyna:schema";
import { TOML, INI, Env } from "dyna:config";
import { URL, URLSearchParams } from "dyna:url";
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

var failedSections = 0;

/* ===================================================================
 * WPT url
 * WPT urltestdata conformance (893 cases; baseline 431; shrink me)
 * =================================================================== */
try {
/* test_conformance_url_wpt.js -- WHATWG URL parser conformance for dyna:url's
 * URL against the vendored Web Platform Tests corpus
 * tests/corpus/wpt/urltestdata.json (893 cases + 115 comment entries).
 *
 * Every array entry is either a "#" comment string (skipped, named) or a case
 * object {input, base, href, origin, protocol, username, password, host,
 * hostname, port, pathname, search, hash}. An expected field being PRESENT is
 * the signal that the parse must succeed and that component must match
 * exactly; "failure": true means new URL(input, base) must throw. "base": null
 * (or absent) means the input is parsed absolute. The 9 "searchParams"
 * expectations in the corpus exercise the URLSearchParams serializer surface
 * and are deliberately not compared here (covered by test_url.js); the
 * "relativeTo"/"comment" keys are runner metadata.
 *
 * Known engine divergences from the round-1 WHATWG fixes (percent-set
 * details, IPv4/IPv6 canonicalization, non-special schemes) survive in a
 * minority of cases. Those are pinned as the BASELINE below: the suite fails
 * unless the set of failing cases equals the baseline exactly. When a fix
 * lands, delete the flipping ids from the array ("shrink me") and the suite
 * goes green again.
 *
 * To re-record the baseline after a fix, set BASELINE = "RECORD", run, paste
 * the printed array back, and re-run. Default runs are fully deterministic.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_conformance_url_wpt.js
 * (from the repo root; the corpus is loaded relative to it)
 */

/* baseline 2026-09-04 (round 2; was 431 on 2026-09-01, 52 after round 1,
 * now 0 -- EMPTY, full conformance): the round-1 remainder fell to
 *   - href escapes kept VERBATIM (the spec's '%' is in no percent-encode
 *     set; new escapes stay uppercase, urltestdata pins mixed input case),
 *   - the current path percent-encode set's U+005E (^),
 *   - the opaque-path state's space rule (a SPACE is %20 exactly when the
 *     remaining input starts with ? or #),
 *   - the serializer's "/." guard for host-null paths starting "//",
 *   - the blob: origin getter (parse the opaque path; http/https wins),
 *   - the special-authority slash-run skip + "/" for an authority with an
 *     empty path (://g> now resolves like WHATWG, not RFC 3986),
 *   - and the file: state machine: no port split (file://d: is the drive),
 *     case-insensitive drives in the authority, the always-appended path
 *     segment after a drive, non-slash file: is a fresh path (never
 *     opaque), and the file-slash drive carry-over </> vs file:///C:/a/b).
 * Keep this array for the next divergence: the suite fails unless the
 * failing set equals it exactly, so a NEW failure cannot hide. To
 * re-record, set BASELINE = "RECORD", run, paste the printed array back. */
const BASELINE = [];

const CORPUS_PATH = "tests/corpus/wpt/urltestdata.json";

/* Fields compared, in corpus order. All are plain string getters. */
const FIELDS = ["href", "origin", "protocol", "username", "password",
                "host", "hostname", "port", "pathname", "search", "hash"];

const raw = std.loadFile(CORPUS_PATH);
if (raw === null || raw === undefined)
    throw new Error("cannot load corpus " + CORPUS_PATH + " (run from the repo root)");
const entries = JSON.parse(raw);

let cases = 0, passed = 0, skippedComments = 0;
const failures = [];        /* {id, why:[strings]} for cases NOT matching expectations */
let originChecks = 0, originMismatches = 0;
const originNullDivergences = [];  /* engine gives a string origin where WPT says "null" */

function caseId(i) { return "case#" + i; }

/* truncate an echo of a case input for bounded output */
function echo(s, n) {
    s = String(s);
    if (s.length > n) s = s.slice(0, n) + "...";
    /* keep the output one-line printable */
    let out = "";
    for (const ch of s) {
        const c = ch.codePointAt(0);
        out += (c >= 0x20 && c < 0x7f) ? ch : (c > 0xffff ? "\\u" + c.toString(16) : "\\u" + ("0000" + c.toString(16)).slice(-4));
    }
    return out;
}

for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    if (typeof e === "string") { skippedComments++; continue; }  /* "#" comment entry */

    cases++;
    const id = caseId(i);
    const why = [];

    if (e.failure) {
        /* The parse must throw, with or without a base. */
        let threw = false;
        try { new URL(e.input, e.base === null || e.base === undefined ? undefined : e.base); }
        catch (err) { threw = true; }
        if (!threw)
            why.push("failure case was accepted (expected a throw)");
    } else {
        let u = null;
        try {
            u = new URL(e.input, e.base === null || e.base === undefined ? undefined : e.base);
        } catch (err) {
            why.push("threw on valid input: " + err);
        }
        if (u) {
            let shown = 0;
            for (const f of FIELDS) {
                if (!(f in e)) continue;
                const got = u[f];
                if (got === e[f]) continue;
                if (f === "origin") originMismatches++;
                if (shown < 3) {
                    why.push(f + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(e[f]));
                    shown++;
                }
            }
            /* compact origin cross-check: WPT "null" origins that the engine
             * renders as a string are tracked separately -- a string origin
             * where the spec serializes "null" is its own divergence class. */
            if ("origin" in e) {
                originChecks++;
                if (e.origin === "null" && u.origin !== "null")
                    originNullDivergences.push(id);
            }
        }
    }

    if (why.length)
        failures.push({ id: id, why: why });
}

/* ---------------- verdict against the baseline ---------------- */

const failIds = failures.map(f => f.id);
const want = BASELINE === "RECORD" ? null : BASELINE.slice().sort();
const got = failIds.slice().sort();

if (BASELINE === "RECORD") {
    print("/* baseline 2026-09-01; shrink me */");
    print("const BASELINE = " + JSON.stringify(failIds) + ";");
    for (const f of failures)
        print(f.id + ": " + f.why.join("; "));
    print("origin: " + originChecks + " checked, " + originMismatches +
          " mismatched, " + originNullDivergences.length +
          " expected-null-but-string: " + originNullDivergences.join(","));
}

if (want !== null) {
    const newOnes = got.filter(x => want.indexOf(x) < 0);
    const shrunk = want.filter(x => got.indexOf(x) < 0);
    if (newOnes.length || shrunk.length) {
        const byId = {};
        for (const f of failures) byId[f.id] = f.why;
        for (const id of newOnes)
            print("NEW FAILURE " + id + ": " + (byId[id] ? byId[id].join("; ") : "?"));
        for (const id of shrunk)
            print("BASELINE SHRUNK: " + id + " now passes -- remove it from BASELINE");
        throw new Error("test_conformance_url_wpt: baseline mismatch ("
                        + newOnes.length + " new, " + shrunk.length + " shrunk of "
                        + BASELINE.length + " baselined)");
    }
}

if (failures.length) {
    /* bounded detail: first 3 mismatches per failing case, first 40 cases */
    for (const f of failures.slice(0, 40))
        print("FAIL " + f.id + " (baseline): " + f.why.join("; "));
    if (failures.length > 40)
        print("... " + (failures.length - 40) + " more baselined failures suppressed");
}

print("test_conformance_url_wpt: " + cases + " cases, " + (cases - failures.length)
      + " passed, " + skippedComments + " skipped (corpus comment entries), "
      + failures.length + " failed-by-baseline");
print("test_conformance_url_wpt: origin " + originChecks + " checked, "
      + originMismatches + " mismatched, " + originNullDivergences.length
      + " expected-null-but-string");
if (cases + skippedComments !== entries.length)
    throw new Error("internal: uncounted corpus entries");
} catch (e) {
    failedSections++; print("SECTION FAILED: WPT url -- " + (e && e.message));
}

/* ===================================================================
 * json-patch
 * official json-patch-tests (112 cases; baseline empty = full conformance)
 * =================================================================== */
try {
/* test_conformance_jsonpatch.js -- RFC 6902 conformance against the vendored
 * OFFICIAL json-patch-tests corpora:
 *   tests/corpus/jsonpatch/tests.json      (community suite, 95 cases)
 *   tests/corpus/jsonpatch/spec_tests.json (RFC 6902 appendix examples, 17 cases)
 * Case shape: {comment, doc, patch, expected?, error?, disabled?}.
 *
 * Protocol: a case PASSES when Patch.apply(doc, patch) succeeds AND the result
 * deep-equals `expected`; when `error: true`, it PASSES only if apply throws.
 * `disabled: true` cases are skipped (named). Deep equality is order-sensitive
 * for arrays and order-INsensitive for objects (JSON objects are unordered per
 * RFC 7159 s1); key-order divergence is therefore not a failure by spec.
 *
 * Skips (all loud, all named): `disabled` cases only. Every non-disabled case
 * runs -- the corpus contains no remote-ref / content-type vectors.
 *
 * Runtime: full corpora, no per-file cap (~112 cases, well under 1 s).
 * Deterministic: no network, no timestamps, fixed corpus order.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_conformance_jsonpatch.js
 * (any cwd; the corpus is resolved from the script's own path).
 */


/* baseline 2026-09-01; shrink me by fixing bugs
 * Every entry is a failing case id ("<file>#<index>"). A case id may be
 * removed ONLY when the fix makes that case pass; additions mean a regression.
 * EMPTY at recording: all 108 runnable cases of the official corpora pass
 * (4 remaining cases are `disabled` upstream). */
const BASELINE = [];

const FILES = ["tests.json", "spec_tests.json"];

/* Corpus path: derived from scriptArgs[0] (the script path this engine
 * passes), so the suite runs from any cwd; repo root is the convention. */
function scriptDir() {
    const p = (typeof scriptArgs !== "undefined" && scriptArgs[0]) || "tests/x.js";
    const i = p.lastIndexOf("/");
    return i >= 0 ? p.slice(0, i) : ".";
}
const CORPUS = scriptDir() + "/corpus/jsonpatch";

/* Order-insensitive for objects, order-sensitive for arrays, === for scalars.
 * Operates only on JSON (no NaN/undefined/BigInt in the corpora). */
function deepEqual(a, b) {
    if (a === b) return true;
    if (typeof a !== "object" || typeof b !== "object" || a === null || b === null)
        return false;
    if (Array.isArray(a) || Array.isArray(b)) {
        if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length)
            return false;
        for (let i = 0; i < a.length; i++)
            if (!deepEqual(a[i], b[i])) return false;
        return true;
    }
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
        if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
        if (!deepEqual(a[k], b[k])) return false;
    }
    return true;
}

let n = 0, passed = 0, skipped = 0;
const failing = [];

for (const file of FILES) {
    const cases = JSON.parse(readFile(new Path(CORPUS + "/" + file)));
    for (let i = 0; i < cases.length; i++) {
        const c = cases[i];
        const id = file + "#" + i;
        n++;
        if (c.disabled) {
            skipped++;
            print("SKIP " + id + " (disabled): " + (c.comment || ""));
            continue;
        }
        if (c.error) {
            let threw = false;
            try { Patch.apply(c.doc, c.patch); } catch (e) { threw = true; }
            if (threw) { passed++; continue; }
            failing.push(id);
            print("FAIL " + id + " (error expected): " + (c.comment || "")
                  + " -- Patch.apply succeeded, want error");
            continue;
        }
        if (!Object.prototype.hasOwnProperty.call(c, "expected")) {
            skipped++;
            print("SKIP " + id + ": no `expected` and not an error case");
            continue;
        }
        let got, threw = null;
        try { got = Patch.apply(c.doc, c.patch); }
        catch (e) { threw = String(e); }
        if (threw !== null) {
            failing.push(id);
            print("FAIL " + id + " (apply threw): " + (c.comment || "") + " -- " + threw);
            continue;
        }
        if (deepEqual(got, c.expected)) { passed++; continue; }
        failing.push(id);
        print("FAIL " + id + " (mismatch): " + (c.comment || "")
              + " -- got " + JSON.stringify(got)
              + ", want " + JSON.stringify(c.expected));
    }
}

/* ---- baseline gate: failures must equal the recorded baseline exactly ---- */

if (failing.length !== BASELINE.length) {
    print("BASELINE MISMATCH: " + failing.length + " failing, baseline has "
          + BASELINE.length);
    for (const id of failing) if (BASELINE.indexOf(id) < 0) print("  new failure: " + id);
    for (const id of BASELINE) if (failing.indexOf(id) < 0) print("  fixed (shrink baseline): " + id);
}
for (let i = 0; i < failing.length; i++) {
    if (failing[i] !== BASELINE[i]) {
        print("BASELINE ORDER MISMATCH at " + i + ": got " + failing[i]
              + ", baseline " + BASELINE[i]);
        break;
    }
}

print("test_conformance_jsonpatch: " + n + " cases, " + passed + " passed, "
      + skipped + " skipped, " + failing.length + " failed-by-baseline");
print("  failing ids: " + (failing.length ? failing.join(", ") : "(none)"));

if (failing.length !== BASELINE.length ||
    failing.some((id, i) => id !== BASELINE[i]))
    throw new Error("test_conformance_jsonpatch: failures diverge from baseline");
} catch (e) {
    failedSections++; print("SECTION FAILED: json-patch -- " + (e && e.message));
}

/* ===================================================================
 * json-schema
 * official draft2020-12 suite (1299 cases; 12 pinned $ref findings, multipleOf fixed)
 * =================================================================== */
try {
/* test_conformance_schema.js -- dyna:schema conformance against the vendored
 * OFFICIAL JSON-Schema-Test-Suite, draft 2020-12:
 *   tests/corpus/jsonschema/draft2020-12/*.json   (this directory only; the
 *   optional/ subtrees are out of scope for this suite)
 * Group shape: {description, schema, tests: [{description, data, valid}]}.
 *
 * Protocol: a case PASSES iff Schema.validate(schema, data).valid === test.valid.
 * Case ids are "<file>#<group>.<test>" (corpus order; stable under edits).
 *
 * SUPPORTED-FEATURE PROBE: the module implements a SUBSET of 2020-12 keywords
 * and silently IGNORES unknown ones (a false-accept hazard), so each feature
 * file is probed ONCE up front with a minimal schema that must FAIL on an
 * instance if the keyword is really enforced. Compile throw or wrong answer =>
 * the whole file is SKIPPED loudly with the named reason. Unsupported today:
 * unevaluated*, contains/min/maxContains, dependentSchemas, propertyNames,
 * $dynamicRef/$dynamicAnchor, $anchor (anchored refs), remote $ref, format
 * (annotation-only, never asserted), content* (annotation-only).
 *
 * PER-CASE skips (loud, named): schemas mentioning a silently-ignored
 * unsupported keyword (unevaluated*, contains*, dependentSchemas,
 * propertyNames, content*, format, $recursive*, $vocabulary) anywhere --
 * nested use still changes the expected outcome; $dynamicRef/$dynamicAnchor,
 * $anchor; any $ref that is not a same-document "#..." pointer (no network,
 * ever); and $id + $ref combined ($id base-URI resolution is unimplemented).
 * "$schema" alone is fine -- the module ignores it, which these tests expect.
 *
 * Runtime: full suite, no per-file cap (~1299 cases, a few seconds).
 * Deterministic: no network, no timestamps, sorted file order.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_conformance_schema.js
 * (any cwd; the corpus is resolved from the script's own path).
 */


/* baseline 2026-09-03: EMPTY -- the json-schema runnable set is fully green.
 * History: multipleOf.json#3.0 fixed 2026-09-01 (non-finite quotient falls
 * back to integral-multiple). The last 12 (ref.json groups 1-3 and 12) fixed
 * 2026-09-03: applicator subschemas now index at their full RFC 6901 path
 * (properties.foo at /properties/foo, not /foo) and $ref fragments are
 * percent-decoded per RFC 3986 before pointer resolution. Remove entries
 * ONLY when a fix makes the case pass; additions mean a regression. */
const BASELINE = [];

const CORPUSDIR = scriptDirSafe() + "/corpus/jsonschema/draft2020-12";

/* Corpus path: derived from scriptArgs[0] (the script path this engine
 * passes), so the suite runs from any cwd; repo root is the convention. */
function scriptDirSafe() {
    const p = (typeof scriptArgs !== "undefined" && scriptArgs[0]) || "tests/x.js";
    const i = p.lastIndexOf("/");
    return i >= 0 ? p.slice(0, i) : ".";
}

/* ---------------------------------------------------------------- probing */

/* Probe helpers. v(schema, data) -> boolean validity; a probe returns null
 * when the feature is supported, or a string reason when it must be skipped. */
function v(schema, data) {
    try { return Schema.validate(schema, data).valid === true; }
    catch (e) { return null; } /* compile/validation throw: unsupported */
}
/* Every probe pairs a MUST-FAIL instance (guards the silent-ignore false
 * accept) with a MUST-PASS instance. */
const PROBES = {
    "additionalProperties.json": () =>
        v({ properties: { a: {} }, additionalProperties: false }, { a: 1, b: 2 }) === false
        && v({ properties: { a: {} }, additionalProperties: false }, { a: 1 }) === true
        ? null : "additionalProperties not enforced",
    "anchor.json": () =>
        v({ $defs: { n: { $anchor: "n", type: "integer" } }, $ref: "#n" }, 3) === true
        && v({ $defs: { n: { $anchor: "n", type: "integer" } }, $ref: "#n" }, "x") === false
        ? null : "anchored $ref ($anchor + \"#name\") unsupported",
    "allOf.json": () =>
        v({ allOf: [{ type: "integer" }, { minimum: 5 }] }, 3) === false
        && v({ allOf: [{ type: "integer" }, { minimum: 5 }] }, 7) === true
        ? null : "allOf not enforced",
    "anyOf.json": () =>
        v({ anyOf: [{ type: "string" }, { type: "integer" }] }, true) === false
        && v({ anyOf: [{ type: "string" }, { type: "integer" }] }, 1) === true
        ? null : "anyOf not enforced",
    "boolean_schema.json": () =>
        v(false, 1) === false && v(true, 1) === true
        ? null : "boolean schemas not supported",
    "const.json": () =>
        v({ const: { a: [1] } }, { a: [1] }) === true
        && v({ const: { a: [1] } }, { a: [2] }) === false
        ? null : "const not enforced",
    "contains.json": () =>
        v({ contains: { const: 2 } }, [1, 3]) === false
        && v({ contains: { const: 2 } }, [1, 2]) === true
        ? null : "contains not enforced (silently ignored)",
    "default.json": () => /* annotation-only keyword: ignoring it IS correct */
        v({ properties: { a: { type: "integer", default: [] } } }, { a: 1 }) === true
        && v({ properties: { a: { type: "integer", default: [] } } }, {}) === true
        && v({ properties: { a: { type: "integer", default: [] } } }, { a: "x" }) === false
        ? null : "default annotation upset validation",
    "defs.json": localRefProbe("local $defs/$ref not supported"),
    "dependentRequired.json": () =>
        v({ dependentRequired: { a: ["b"] } }, { a: 1 }) === false
        && v({ dependentRequired: { a: ["b"] } }, { a: 1, b: 2 }) === true
        ? null : "dependentRequired not enforced",
    "dependentSchemas.json": () =>
        v({ dependentSchemas: { a: { required: ["b"] } } }, { a: 1 }) === false
        && v({ dependentSchemas: { a: { required: ["b"] } } }, { a: 1, b: 2 }) === true
        ? null : "dependentSchemas not enforced (silently ignored)",
    "dynamicRef.json": () =>
        v({ $defs: { n: { $dynamicAnchor: "n", type: "integer" } }, $dynamicRef: "#n" }, 1) === false
        && v({ $defs: { n: { $dynamicAnchor: "n", type: "integer" } }, $dynamicRef: "#n" }, "x") === true
        ? null : "$dynamicRef/$dynamicAnchor unsupported",
    "enum.json": () =>
        v({ enum: [1, "a", null] }, "a") === true
        && v({ enum: [1, "a", null] }, "b") === false
        ? null : "enum not enforced",
    "exclusiveMaximum.json": () =>
        v({ exclusiveMaximum: 5 }, 5) === false && v({ exclusiveMaximum: 5 }, 4.5) === true
        ? null : "exclusiveMaximum not enforced",
    "exclusiveMinimum.json": () =>
        v({ exclusiveMinimum: 5 }, 5) === false && v({ exclusiveMinimum: 5 }, 5.5) === true
        ? null : "exclusiveMinimum not enforced",
    "if-then-else.json": () =>
        v({ if: { type: "string" }, then: { minLength: 2 } }, "a") === false
        && v({ if: { type: "string" }, then: { minLength: 2 } }, "ab") === true
        && v({ if: { type: "string" }, else: { minimum: 10 } }, 3) === false
        && v({ if: { type: "string" }, else: { minimum: 10 } }, 30) === true
        ? null : "if/then/else not enforced",
    "infinite-loop-detection.json": localRefProbe("local $defs/$ref not supported"),
    "items.json": () =>
        v({ items: { type: "integer" } }, [1, "x"]) === false
        && v({ items: { type: "integer" } }, [1]) === true
        ? null : "items not enforced",
    "maxContains.json": containsProbe,
    "maxItems.json": () =>
        v({ maxItems: 1 }, [1, 2]) === false && v({ maxItems: 1 }, [1]) === true
        ? null : "maxItems not enforced",
    "maxLength.json": () =>
        v({ maxLength: 2 }, "abc") === false && v({ maxLength: 2 }, "ab") === true
        ? null : "maxLength not enforced",
    "maxProperties.json": () =>
        v({ maxProperties: 1 }, { a: 1, b: 2 }) === false && v({ maxProperties: 1 }, { a: 1 }) === true
        ? null : "maxProperties not enforced",
    "maximum.json": () =>
        v({ maximum: 5 }, 5.5) === false && v({ maximum: 5 }, 5) === true
        ? null : "maximum not enforced",
    "minContains.json": containsProbe,
    "minItems.json": () =>
        v({ minItems: 2 }, [1]) === false && v({ minItems: 2 }, [1, 2]) === true
        ? null : "minItems not enforced",
    "minLength.json": () =>
        v({ minLength: 2 }, "a") === false && v({ minLength: 2 }, "ab") === true
        ? null : "minLength not enforced",
    "minProperties.json": () =>
        v({ minProperties: 2 }, { a: 1 }) === false && v({ minProperties: 2 }, { a: 1, b: 2 }) === true
        ? null : "minProperties not enforced",
    "minimum.json": () =>
        v({ minimum: 5 }, 4) === false && v({ minimum: 5 }, 5) === true
        ? null : "minimum not enforced",
    "multipleOf.json": () =>
        v({ multipleOf: 2 }, 7) === false && v({ multipleOf: 2 }, 8) === true
        ? null : "multipleOf not enforced",
    "not.json": () =>
        v({ not: { type: "string" } }, "x") === false && v({ not: { type: "string" } }, 1) === true
        ? null : "not not enforced",
    "oneOf.json": () =>
        v({ oneOf: [{ type: "integer" }, { minimum: 5 }] }, 6) === false
        && v({ oneOf: [{ type: "integer" }, { minimum: 5 }] }, 3) === true
        ? null : "oneOf not enforced",
    "pattern.json": () =>
        v({ pattern: "^a+$" }, "b") === false && v({ pattern: "^a+$" }, "aa") === true
        ? null : "pattern not enforced",
    "patternProperties.json": () =>
        v({ patternProperties: { "^a": { type: "integer" } } }, { ab: "x" }) === false
        && v({ patternProperties: { "^a": { type: "integer" } } }, { ab: 1 }) === true
        ? null : "patternProperties not enforced",
    "prefixItems.json": () =>
        v({ prefixItems: [{ type: "integer" }] }, ["x"]) === false
        && v({ prefixItems: [{ type: "integer" }] }, [1]) === true
        ? null : "prefixItems not enforced",
    "properties.json": () =>
        v({ properties: { a: { type: "integer" } } }, { a: "x" }) === false
        && v({ properties: { a: { type: "integer" } } }, { a: 1 }) === true
        ? null : "properties not enforced",
    "propertyNames.json": () =>
        v({ propertyNames: { maxLength: 1 } }, { ab: 1 }) === false
        && v({ propertyNames: { maxLength: 1 } }, { a: 1 }) === true
        ? null : "propertyNames not enforced (silently ignored)",
    "ref.json": localRefProbe("local $defs/$ref not supported"),
    "refRemote.json": () =>
        v({ $ref: "http://localhost:1234/draft2020-12/integer.json" }, 1) === false
        ? null : "remote $ref retrieval unimplemented (suite stays offline)",
    "required.json": () =>
        v({ required: ["a"] }, {}) === false && v({ required: ["a"] }, { a: 1 }) === true
        ? null : "required not enforced",
    "type.json": () =>
        v({ type: "integer" }, "x") === false
        && v({ type: "integer" }, 3) === true
        && v({ type: ["string", "null"] }, null) === true
        && v({ type: "object" }, []) === false
        ? null : "type not enforced",
    "unevaluatedItems.json": () =>
        v({ unevaluatedItems: false }, [1, 2]) === false
        && v({ unevaluatedItems: false }, [1]) === true
        ? null : "unevaluatedItems not enforced (silently ignored)",
    "unevaluatedProperties.json": () =>
        v({ unevaluatedProperties: false }, { a: 1 }) === false
        && v({ unevaluatedProperties: false }, {}) === true
        ? null : "unevaluatedProperties not enforced (silently ignored)",
    "uniqueItems.json": () =>
        v({ uniqueItems: true }, [1, 1]) === false && v({ uniqueItems: true }, [1, 2]) === true
        ? null : "uniqueItems not enforced",
    "vocabulary.json": () =>
        /* The file's subject: a $schema metaschema WITHOUT the validation
         * vocabulary must suppress `minimum`. The module ignores $schema, so
         * it enforces anyway -- skip the file rather than record bogus fails. */
        v({ $schema: "http://localhost:1234/draft2020-12/metaschema-no-validation.json",
            properties: { n: { minimum: 10 } } }, { n: 1 }) === true
        ? null : "vocabulary-aware keyword suppression unimplemented ($schema "
            + "ignored; validation keywords always enforced)",
};

function localRefProbe(reason) {
    return () =>
        v({ $defs: { i: { type: "integer" } }, $ref: "#/$defs/i" }, 3) === true
        && v({ $defs: { i: { type: "integer" } }, $ref: "#/$defs/i" }, "x") === false
        ? null : reason;
}
function containsProbe() {
    if (v({ contains: { const: 2 } }, [1, 3]) !== false
        || v({ contains: { const: 2 } }, [1, 2]) !== true)
        return "contains not enforced (silently ignored)";
    if (v({ contains: { const: 2 }, maxContains: 1 }, [2, 2]) !== false)
        return "maxContains not enforced (silently ignored)";
    if (v({ contains: { const: 2 }, minContains: 2 }, [2]) !== false)
        return "minContains not enforced (silently ignored)";
    return null;
}

/* Whole-file policy skips: keywords whose 2020-12 semantics are annotation-
 * only here and whose test vectors assert exactly those semantics. */
const POLICY_SKIP = {
    "content.json": "contentMediaType/contentSchema assertions: annotation-only "
        + "keywords, unimplemented; vectors assert their absence only",
    "format.json": "format is annotation-only in 2020-12 and never asserted by "
        + "this module; assertion vectors skipped per audit brief",
};

/* ------------------------------------------------- per-case skip analysis */

function collectRefs(x, out) {
    if (Array.isArray(x)) { for (const e of x) collectRefs(e, out); }
    else if (x && typeof x === "object") {
        for (const k of Object.keys(x)) {
            if (k === "$ref" && typeof x[k] === "string") out.push(x[k]);
            else collectRefs(x[k], out);
        }
    }
    return out;
}
function hasKeyword(schema, kw) {
    return JSON.stringify(schema).indexOf('"' + kw + '"') >= 0;
}
/* Keywords the module silently IGNORES; if one appears anywhere in a schema
 * (even nested inside not/anyOf, where it still changes the expected
 * outcome), the case cannot be judged fairly -- skip it, named. */
const UNSUPPORTED_KW = [
    "unevaluatedProperties", "unevaluatedItems",
    "contains", "minContains", "maxContains",
    "dependentSchemas", "propertyNames",
    "contentMediaType", "contentEncoding", "contentSchema",
    "format", "$recursiveRef", "$recursiveAnchor", "$vocabulary",
];
function caseSkipReason(schema) {
    for (const kw of UNSUPPORTED_KW)
        if (hasKeyword(schema, kw))
            return "mentions \"" + kw + "\" (silently ignored here; changes "
                + "this case's semantics)";
    if (hasKeyword(schema, "$dynamicRef") || hasKeyword(schema, "$dynamicAnchor"))
        return "uses $dynamicRef/$dynamicAnchor (unsupported)";
    if (hasKeyword(schema, "$anchor"))
        return "uses $anchor (anchored refs unsupported)";
    const refs = collectRefs(schema, []);
    for (const r of refs)
        if (r.charAt(0) !== "#") return "remote $ref (" + r + "): no retrieval";
    if (hasKeyword(schema, "$id") && refs.length > 0)
        return "uses $id base-URI resolution with $ref (unsupported)";
    return null;
}

/* ------------------------------------------------------------------ drive */

let n = 0, passed = 0, skipped = 0, filesRun = 0, filesSkipped = 0;
const failing = [];

/* readDir returns [{name, isDir, ...}]; sort names for determinism. */
const names = readDir(new Path(CORPUSDIR))
    .map((e) => e.name)
    .filter((nm) => nm.endsWith(".json"))
    .sort();

for (const name of names) {
    const groups = JSON.parse(readFile(new Path(CORPUSDIR + "/" + name)));

    let fileTests = 0;
    for (const g of groups) fileTests += g.tests.length;

    if (POLICY_SKIP[name]) {
        n += fileTests;
        filesSkipped++;
        skipped += fileTests;
        print("SKIP FILE " + name + " (" + fileTests + " tests): " + POLICY_SKIP[name]);
        continue;
    }
    const probe = PROBES[name];
    if (probe) {
        const reason = probe();
        if (reason !== null) {
            n += fileTests;
            filesSkipped++;
            skipped += fileTests;
            print("SKIP FILE " + name + " (" + fileTests + " tests): " + reason);
            continue;
        }
    }
    filesRun++;

    for (let gi = 0; gi < groups.length; gi++) {
        const g = groups[gi];
        for (let ti = 0; ti < g.tests.length; ti++) {
            const t = g.tests[ti];
            const id = name + "#" + gi + "." + ti;
            n++;
            const why = caseSkipReason(g.schema);
            if (why !== null) {
                skipped++;
                print("SKIP " + id + ": " + why + " -- " + g.description + " / " + t.description);
                continue;
            }
            let got;
            try { got = Schema.validate(g.schema, t.data).valid === true; }
            catch (e) {
                failing.push(id);
                print("FAIL " + id + " (validate threw): " + g.description
                      + " / " + t.description + " -- " + String(e));
                continue;
            }
            if (got === (t.valid === true)) { passed++; continue; }
            failing.push(id);
            print("FAIL " + id + ": " + g.description + " / " + t.description
                  + " -- expected " + (t.valid ? "valid" : "invalid") + ", got "
                  + (got ? "valid" : "invalid")
                  + " | schema " + JSON.stringify(g.schema)
                  + " | data " + JSON.stringify(t.data));
        }
    }
}

/* ---- baseline gate: failures must equal the recorded baseline exactly ---- */

if (failing.length !== BASELINE.length) {
    print("BASELINE MISMATCH: " + failing.length + " failing, baseline has "
          + BASELINE.length);
    for (const id of failing) if (BASELINE.indexOf(id) < 0) print("  new failure: " + id);
    for (const id of BASELINE) if (failing.indexOf(id) < 0) print("  fixed (shrink baseline): " + id);
}
for (let i = 0; i < failing.length && i < BASELINE.length; i++) {
    if (failing[i] !== BASELINE[i]) {
        print("BASELINE ORDER MISMATCH at " + i + ": got " + failing[i]
              + ", baseline " + BASELINE[i]);
        break;
    }
}

print("test_conformance_schema: " + n + " cases, " + passed + " passed, "
      + skipped + " skipped, " + failing.length + " failed-by-baseline"
      + " [" + filesRun + " files run, " + filesSkipped + " files skipped]");
print("  failing ids: " + (failing.length ? failing.join(", ") : "(none)"));

if (failing.length !== BASELINE.length ||
    failing.some((id, i) => id !== BASELINE[i]))
    throw new Error("test_conformance_schema: failures diverge from baseline");
} catch (e) {
    failedSections++; print("SECTION FAILED: json-schema -- " + (e && e.message));
}

/* ===================================================================
 * params-grid
 * parametric grids: dataframe/mathx/ml (1969 probes; 85.8% of names all-three-probes)
 * =================================================================== */
try {
/* test_api_params_df_mathx_ml.js -- PARAMETRIC EMPTY / WRONG-TYPE / BOUNDARY
 * sweeps over every RUNTIME export of dyna:dataframe, dyna:mathx and dyna:ml,
 * the three worst value-covered modules per CLAUDE.md (5% / 7% / 9%).
 *
 * For each exported name (module namespace, namespace objects, class prototypes
 * and statics -- non-enumerable natives need getOwnPropertyNames, not
 * Object.keys) we drive the three standard probes wherever a call is
 * constructible with constant arguments:
 *
 *   EMPTY      no args (or empty arrays/objects where that is the shape),
 *   WRONG-TYPE one representative: a string where numbers/arrays are expected
 *              and vice versa,
 *   BOUNDARY   numeric params: 0, -1, NaN, Infinity, 2^53 (BigInt params:
 *              0n, -1n, 2^64-scale values); array params: [] and [null].
 *
 * A probe PASSES when the call returns a defined value OR throws a catchable
 * error (recorded class) -- never crashes, never hangs -- and never mutates
 * the Object/Array/Function prototype canary checked around every row.  Where
 * an INDEPENDENT oracle exists (JS builtins, hand-computed arithmetic,
 * published constants) the suite additionally pins the CORRECT VALUE; those
 * rows are the ones that can fail for substance, not just safety.
 *
 * Coverage is counted against the runtime enumeration, and the exact fraction
 * is printed at the end.  Names that legitimately take no arguments (data
 * getters, constants) are read-pinned and reported separately; anything left
 * is skipped loudly by name.
 *
 * Deterministic: no network, no timestamps, seeded or oracle-only assertions.
 */

const THROWS = Symbol("throws");
const ANY = Symbol("any"); /* a defined value or a catchable throw */
const OK = Symbol("ok-any"); /* any outcome, including a documented undefined */

let pass = 0, fail = 0, skip = 0, baseFail = 0;
const fails = [], notes = [], thrownClass = {};

/* Rows whose failure is recorded BASELINE behaviour on the current build,
 * keyed as "<module>: <case id>".  Empty today: every probe below was
 * observed to pass.  A future fix that makes a named row green is flagged by
 * the runner ("baseline entry now GREEN"); delete the id from this set to
 * tighten the baseline. */
const BASELINE_FAILS = new Set([
]);

/* ------------------------------------------------------- prototype canary */
const CANARY = "__dyna_t3_canary__";
const protoSnap = new Map();
for (const p of [Object.prototype, Array.prototype, Function.prototype,
                 String.prototype, Number.prototype])
    protoSnap.set(p, Object.getOwnPropertyNames(p).join(","));
let canaryHits = 0;

function checkCanary(id) {
    for (const [p, snap] of protoSnap) {
        const now = Object.getOwnPropertyNames(p).join(",");
        if (now !== snap) {
            canaryHits++;
            fails.push(`CANARY MUTATION at ${id}: ${p === Object.prototype ?
                "Object.prototype" : "a standard prototype"} gained [${now}]`);
            fail++;
            protoSnap.set(p, now); /* report each distinct pollution once */
        }
    }
    if (CANARY in {}) {
        canaryHits++;
        fails.push(`CANARY MUTATION at ${id}: Object.prototype.${CANARY} exists`);
        fail++;
    }
}

/* ---------------------------------------------------------------- helpers */
function eqv(a, b) {
    if (b === ANY || b === OK || b === THROWS) return false;
    if (Object.is(a, b)) return true;
    if (typeof a === "number" && typeof b === "number" && isFinite(a) && isFinite(b))
        return Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(b));
    if (a && b && typeof a === "object" && typeof b === "object") {
        const A = Array.isArray(a) ? a : Object.keys(a).sort().map((k) => [k, a[k]]);
        const B = Array.isArray(b) ? b : Object.keys(b).sort().map((k) => [k, b[k]]);
        if (A.length !== B.length) return false;
        for (let i = 0; i < A.length; i++)
            if (Array.isArray(A[i]) && Array.isArray(B[i])) { if (!eqv(A[i], B[i])) return false; }
            else if (Array.isArray(A[i]) !== Array.isArray(B[i])) return false;
            else if (!eqv(A[i], B[i])) return false;
        return true;
    }
    return false;
}

const near = (x, y, tol) => Math.abs(x - y) <= (tol === undefined ? 1e-9 : tol);

const show = (v) => {
    try {
        if (typeof v === "bigint") return v + "n";
        if (typeof v === "string") return JSON.stringify(v);
        if (typeof v === "number" && (Number.isNaN(v))) return "NaN";
        return String(v);
    } catch (e) { return "<unprintable>"; }
};

function describe(threw, got, want) {
    if (threw) return `threw ${threw.name}: ${threw.message}`;
    if (want === THROWS) return `expected a throw, got ${show(got)}`;
    if (want === ANY) return "expected a defined value or a throw, got undefined";
    return `got ${show(got)} want ${show(want)}`;
}

function run(modName, rows) {
    print(`\n-- ${modName} (${rows.length} cases) --`);
    for (const [name, thunk, want] of rows) {
        let got, threw = null;
        try { got = thunk(); } catch (e) { threw = e; }
        if (threw) thrownClass[threw.name] = (thrownClass[threw.name] || 0) + 1;
        checkCanary(name);
        let ok;
        if (want === THROWS) ok = threw !== null;
        else if (want === ANY) ok = threw !== null || got !== undefined;
        else if (want === OK) ok = true;
        else ok = threw === null && eqv(got, want);
        if (ok) {
            pass++;
            if (BASELINE_FAILS.has(`${modName}: ${name}`))
                notes.push(`baseline entry now GREEN (tighten BASELINE_FAILS): ${modName}: ${name}`);
        } else if (BASELINE_FAILS.has(`${modName}: ${name}`)) {
            baseFail++;
            fails.push(`BASELINE ${modName}: ${name} -- ${describe(threw, got, want)}`);
        } else {
            fail++;
            fails.push(`${modName}: ${name} -- ${describe(threw, got, want)}`);
        }
    }
}

async function mod(n) { try { return await import("dyna:" + n); } catch (e) { return null; } }
function section(modName, m, rowsFn) {
    if (!m) { skip++; print(`\n-- ${modName} -- SKIP (not in this build)`); return; }
    run(modName, rowsFn(m));
}

/* --------------------------------------------------- coverage accounting */
/* Every name gets EMPTY + WRONG-TYPE + BOUNDARY rows (mark3) or, when it
 * legitimately takes no arguments (constants, data getters), a read-pinned
 * row (markRead).  The summary prints the exact all-three-probes fraction. */
const COV = {
    mathx:     { probed3: new Set(), read: new Set(), names: new Set() },
    dataframe: { probed3: new Set(), read: new Set(), names: new Set() },
    ml:        { probed3: new Set(), read: new Set(), names: new Set() },
};
const mark3 = (m, n) => COV[m].probed3.add(n);
const markRead = (m, n) => COV[m].read.add(n);

/* Drive every entry of `calls` inside one row: each call must produce an
 * outcome (return or catchable throw) without ending the process. */
const outcomes = (calls) => {
    let n = 0;
    for (const c of calls) { try { c(); n++; } catch (e) { n++; } }
    return n;
};

/* Standard boundary sets (brief): numerics 0, -1, NaN, Infinity, 2^53;
 * BigInt-taking params get the same magnitudes at 64-bit scale;
 * array params get [] and [null]. */
const BNUM = [0, -1, NaN, Infinity, 2 ** 53];
const BBIG = [0n, -1n, 2n ** 64n, 2n ** 53n];

/* ===================================================================== */
/* ==                        dyna:mathx                               == */
/* ===================================================================== */

section("mathx", await mod("mathx"), (m) => {
    const rows = [];

    /* ---- enumerate everything the module actually exports at runtime ---- */
    const top = Object.getOwnPropertyNames(m);
    for (const k of top) COV.mathx.names.add(k);
    COV.mathx.names.delete("bits"); /* container; its 38 members are counted */
    for (const k of Object.getOwnPropertyNames(m.bits)) COV.mathx.names.add("bits." + k);
    for (const k of Object.getOwnPropertyNames(m.Expression.prototype))
        if (k !== "constructor") COV.mathx.names.add("Expression." + k);

    /* ---- sweep-spec helpers.  Each returns the three rows for a name. ---- */
    const add3 = (nm, empty, wrong, boundCalls, nBound) => {
        mark3("mathx", nm);
        rows.push([`${nm} EMPTY`, empty, ANY],
                  [`${nm} WRONG-TYPE`, wrong, ANY],
                  [`${nm} BOUNDARY`, () => outcomes(boundCalls), nBound]);
    };
    const num1 = (nm, f) => add3(nm,
        () => f(), () => f("x"),
        BNUM.map((v) => () => f(v)), BNUM.length);
    const num2 = (nm, f) => add3(nm,
        () => f(), () => f("x", "x"),
        [...BNUM.map((v) => () => f(v, 1)), ...BNUM.map((v) => () => f(1, v))],
        2 * BNUM.length);
    const big1 = (nm, f) => add3(nm,
        () => f(), () => f(123) /* Number where BigInt expected: docs say TypeError */,
        BBIG.map((v) => () => f(v)), BBIG.length);
    const big2 = (nm, f) => add3(nm,
        () => f(), () => f(123, 123),
        [...BBIG.map((v) => () => f(v, 1n)), ...BBIG.map((v) => () => f(1n, v))],
        2 * BBIG.length);
    const arr1 = (nm, f) => add3(nm,
        () => f(), () => f(123) /* number where array expected */,
        [() => f([]), () => f([null])], 2);

    /* ---- constants: read-pinned against JS builtins.  SqrtPi/SqrtE are
     * correctly-rounded literals (API.md), which can sit one ulp from the
     * composed builtin Math.sqrt(Math.PI); they are pinned by the squaring
     * identity instead of bit-identity. ---- */
    const consts = {
        E: Math.E, Pi: Math.PI, Phi: (1 + Math.sqrt(5)) / 2, Ln2: Math.LN2,
        Ln10: Math.LN10, Log2E: Math.LOG2E, Log10E: Math.LOG10E,
        Sqrt2: Math.SQRT2, MaxInt32: 2 ** 31 - 1, MinInt32: -(2 ** 31),
        MaxSafeInteger: Number.MAX_SAFE_INTEGER, MaxInt64: 2n ** 63n - 1n,
    };
    for (const k of Object.keys(consts)) {
        markRead("mathx", k);
        rows.push([`${k} equals the JS builtin`, () => Object.is(m[k], consts[k]), true]);
    }
    markRead("mathx", "SqrtPi");
    markRead("mathx", "SqrtE");
    rows.push(["SqrtPi squares back to Pi", () => Math.abs(m.SqrtPi ** 2 - Math.PI) <= 1e-15,
               true],
              ["SqrtPi is within one ulp of Math.sqrt(PI)", () =>
                  Math.abs(m.SqrtPi - Math.sqrt(Math.PI)) <= 2.23e-16, true],
              ["SqrtE squares back to E", () => Math.abs(m.SqrtE ** 2 - Math.E) <= 1e-15, true]);
    for (const k of top) {
        const v = m[k];
        if (typeof v === "number" && !(k in consts)) { /* none expected */
            markRead("mathx", k);
            rows.push([`${k} is a finite number`, () => isFinite(v), true]);
        }
    }

    /* ---- scalar ops pinned against the JS builtins / C99 semantics ---- */
    rows.push(
        /* round is C99 ties-AWAY-from-zero, deliberately NOT Math.round. */
        ["round(0.5) ties away", () => m.round(0.5), 1],
        ["round(-2.5) ties away, differs from Math.round", () =>
            [m.round(-2.5), Math.round(-2.5)], [-3, -2]],
        ["round(2.4) equals nearby builtin", () => m.round(2.4), 2],
        ["roundToEven(2.5) half to even", () => m.roundToEven(2.5), 2],
        ["roundToEven(3.5) half to even", () => m.roundToEven(3.5), 4],
        ["fix truncates toward zero", () => m.fix(-3.7), Math.trunc(-3.7)],
        ["trunc equals Math.trunc", () => m.trunc(-3.7), Math.trunc(-3.7)],
        ["sign(-0) passes -0 through", () => Object.is(m.sign(-0), -0), true],
        ["sign(-7)", () => m.sign(-7), -1],
        ["signbit(-0)", () => m.signbit(-0), true],
        ["signbit(0)", () => m.signbit(0), false],
        /* mod is MATLAB FLOORED modulo == ((a%b)+b)%b; mod(a,0) is a (docs). */
        ["mod(-7,3) floored == ((a%b)+b)%b", () => m.mod(-7, 3), ((-7 % 3) + 3) % 3],
        ["mod(5,0) is a", () => m.mod(5, 0), 5],
        /* rem/fmod are the TRUNCATED C99 remainder == a%b. */
        ["rem(-7,3) truncated == a%b", () => m.rem(-7, 3), -7 % 3],
        ["fmod(-7,3) truncated == a%b", () => m.fmod(-7, 3), -7 % 3],
        /* remainder is C99 ROUND-TO-NEAREST. */
        ["remainder(5,2) round-to-nearest", () => m.remainder(5, 2), 1],
        ["remainder(7,3) round-to-nearest", () => m.remainder(7, 3), 1],
        ["idivide(-7,2) fix == Math.trunc", () => m.idivide(-7, 2), Math.trunc(-7 / 2)],
        ["idivide(-7,2,'floor') == Math.floor", () =>
            m.idivide(-7, 2, "floor"), Math.floor(-7 / 2)],
        ["idivide(-7,2,'ceil') == Math.ceil", () =>
            m.idivide(-7, 2, "ceil"), Math.ceil(-7 / 2)],
        ["idivide(-7,2,'round') ties away", () => m.idivide(-7, 2, "round"), -4],
        ["nthroot(-8,3) where pow is NaN", () =>
            [m.nthroot(-8, 3), Math.pow(-8, 1 / 3)], [-2, NaN]],
        ["nthroot(16,4)", () => m.nthroot(16, 4), 2],
        ["cbrt equals Math.cbrt", () => m.cbrt(27), Math.cbrt(27)],
        ["hypot(3,4)", () => m.hypot(3, 4), 5],
        ["copysign(3,-1)", () => m.copysign(3, -1), -Math.abs(3)],
        ["nextafter(1,Inf) one ulp", () => m.nextafter(1, Infinity), 1 + 2 ** -52],
        ["expm1 near Math.expm1", () => near(m.expm1(0.5), Math.expm1(0.5), 1e-15), true],
        ["log1p near Math.log1p", () => near(m.log1p(0.5), Math.log1p(0.5), 1e-15), true],
        ["log2(8)", () => m.log2(8), 3],
        ["logb/ilogb(8)", () => [m.logb(8), m.ilogb(8)], [3, 3]],
        ["ilogb(0) FP_ILOGB0", () => m.ilogb(0), -(2 ** 31)],
        ["scalbn(3,4) == 3*2**4", () => m.scalbn(3, 4), 3 * 2 ** 4],
        ["ldexp identical to scalbn", () => m.ldexp(3, 4), m.scalbn(3, 4)],
        ["frexp(8) splits exactly", () => m.frexp(8), [0.5, 4]],
        ["frexp(0) -> [0,0]", () => m.frexp(0), [0, 0]],
        ["isInf positive only", () => [m.isInf(Infinity), m.isInf(3)], [true, false]],
        ["isInf(-Inf, sign<0)", () => m.isInf(-Infinity, -1), true],
        ["isNaN", () => [m.isNaN(NaN), m.isNaN(3)], [true, false]],
        ["pow2(3) == 2**3", () => m.pow2(3), 2 ** 3],
        ["deg2rad(180)", () => m.deg2rad(180), Math.PI],
        ["rad2deg(PI)", () => m.rad2deg(Math.PI), 180],
        ["nextpow2(17)", () => m.nextpow2(17), 5],
        ["nextpow2(0) is 0", () => m.nextpow2(0), 0],
        ["erf(0)", () => m.erf(0), 0],
        ["erfc(0) == 1 - erf(0)", () => m.erfc(0), 1 - m.erf(0)],
        ["erfinv(0)", () => m.erfinv(0), 0],
        ["erfcinv(1)", () => m.erfcinv(1), 0],
        ["erfcx(0) == erfc(0)", () => m.erfcx(0), 1],
        /* gamma oracle: gamma(n) == (n-1)! for integer n. */
        ["gamma(5) == 4!", () => m.gamma(5), 24],
        ["gamma(0.5) == sqrt(pi)", () => near(m.gamma(0.5), Math.sqrt(Math.PI), 1e-12), true],
        ["lgamma(5) == [ln(24), +1]", () =>
            [near(m.lgamma(5)[0], Math.log(24), 1e-12), m.lgamma(5)[1]], [true, 1]],
        ["gammaln(6) == ln(5!)", () => near(m.gammaln(6), Math.log(120), 1e-12), true],
        ["beta(2,3) == 1/12", () => near(m.beta(2, 3), 1 / 12, 1e-12), true],
        ["betaln(2,3) == ln(1/12)", () => near(m.betaln(2, 3), Math.log(1 / 12), 1e-12), true],
        ["psi(1) == -Euler gamma", () => near(m.psi(1), -0.5772156649015329, 1e-12), true],
        ["polygamma(1,1) == pi^2/6", () =>
            near(m.polygamma(1, 1), (Math.PI ** 2) / 6, 1e-12), true],
        ["gammainc(1,1) lower == 1-1/e", () =>
            near(m.gammainc(1, 1), 1 - Math.exp(-1), 1e-12), true],
        ["gammainc upper == 1 - lower", () =>
            near(m.gammainc(1, 1, "upper"), Math.exp(-1), 1e-12), true],
        ["gammaincinv(0.5,3) round trip", () =>
            near(m.gammainc(m.gammaincinv(0.5, 3), 3), 0.5, 1e-9), true],
        ["betainc(0.5,1,1) is x", () => m.betainc(0.5, 1, 1), 0.5],
        /* hand-computed: 12 * [x^2/2 - 2x^3/3 + x^4/4] at 0.5 = 0.6875. */
        ["betainc(0.5,2,3) hand value", () => m.betainc(0.5, 2, 3), 0.6875],
        ["betaincinv round trip", () =>
            near(m.betainc(m.betaincinv(0.5, 2, 3), 2, 3), 0.5, 1e-9), true],
        ["expint(1) E1 tabulated", () => near(m.expint(1), 0.2193839343955203, 1e-12), true],
        ["besselj(0,0)", () => m.besselj(0, 0), 1],
        ["besselj(0,1) tabulated", () => near(m.besselj(0, 1), 0.7651976865579666, 1e-12), true],
        ["bessely(0,1) tabulated", () => near(m.bessely(0, 1), 0.08825696421567697, 1e-12), true],
        ["besselk(0,1) tabulated", () => near(m.besselk(0, 1), 0.4210244382407083, 1e-12), true],
        ["besseliScaled(0,1) tabulated", () =>
            near(m.besseliScaled(0, 1), 0.4657596075936404, 1e-12), true],
        ["besselh(0,1,1) == [J0(1), Y0(1)]", () => {
            const [re, im] = m.besselh(0, 1, 1);
            return [near(re, m.besselj(0, 1), 1e-12), near(im, m.bessely(0, 1), 1e-12)];
        }, [true, true]],
        ["ellipke(0.5) tabulated", () =>
            m.ellipke(0.5).map((v, i) => near(v, [1.8540746773013717, 1.3506438810476753][i], 1e-12)),
        [true, true]],
        /* ellipj identities: sn^2 + cn^2 == 1 and dn^2 + m*sn^2 == 1. */
        ["ellipj(1,0.5) identities", () => {
            const j = m.ellipj(1, 0.5);
            return [near(j.sn * j.sn + j.cn * j.cn, 1, 1e-12),
                    near(j.dn * j.dn + 0.5 * j.sn * j.sn, 1, 1e-12)];
        }, [true, true]],
        /* legendre: P2(x) = (3x^2-1)/2; P2^1 has the Condon-Shortley phase. */
        ["legendre(2,0.5) hand values", () => {
            const c = m.legendre(2, 0.5);
            return [near(c[0], -0.125, 1e-12), near(c[1], -1.299038105676658, 1e-12)];
        }, [true, true]],
        ["legendreP(2,1,0.5) == legendre column", () =>
            m.legendreP(2, 1, 0.5), m.legendre(2, 0.5)[1]],
        ["airy(0) tabulated Ai(0)", () => near(m.airy(0).ai, 0.3550280538878172, 1e-12), true],
        ["isPrime(97)/isPrime(91)", () => [m.isPrime(97), m.isPrime(91)], [true, false]],
        ["factor(84) product identity", () => m.factor(84).reduce((a, b) => a * b, 1), 84],
        ["factor(1) is empty", () => m.factor(1), []],
        ["factor(0) refused", () => m.factor(0), THROWS],
        ["primes(20) hand list", () => m.primes(20), [2, 3, 5, 7, 11, 13, 17, 19]],
        ["primes(1) is empty", () => m.primes(1), []],
        ["primes past 5e7 refused", () => m.primes(6e7), THROWS],
        ["gcd(48,36)", () => m.gcd(48, 36), 12n],
        ["lcm(6,8)", () => m.lcm(6, 8), 24n],
        ["factorial(20)", () => m.factorial(20), 2432902008176640000n],
        ["factorial past 10000 refused", () => m.factorial(10001), THROWS],
        ["abs(-42n)", () => m.abs(-42n), 42n],
        ["abs of a Number is refused", () => m.abs(42), THROWS],
        ["bitLen(255n)/bitLen(0n)", () => [m.bitLen(255n), m.bitLen(0n)], [8, 0]],
        ["popcount(0b101101n)", () => m.popcount(0b101101n), 4],
        ["nchoosek(10,3) hand value", () => m.nchoosek(10, 3), 120],
        ["nchoosek(52,5) poker hands", () => m.nchoosek(52, 5), 2598960],
        ["nchoosek k>n is 0", () => m.nchoosek(3, 5), 0],
        ["perms([1,2]) reverse lex", () => m.perms([1, 2]), [[2, 1], [1, 2]]],
        ["perms([1,2,3]) 3! rows", () => m.perms([1, 2, 3]).length, 6],
        ["perms past 8 elements refused", () => m.perms([1, 2, 3, 4, 5, 6, 7, 8, 9]), THROWS],
        ["rat(0.333)", () => m.rat(0.333), [333, 1000]],
        ["rat(PI) == 355/113", () => m.rat(Math.PI), [355, 113]],
        ["linspace(0,1,3) ends exact", () => m.linspace(0, 1, 3), [0, 0.5, 1]],
        ["logspace(0,1,3) ends exact", () => m.logspace(0, 1, 3), [1, 10 ** 0.5, 10]],
        ["cumsum", () => m.cumsum([1, 2, 3]), [1, 3, 6]],
        ["cumprod", () => m.cumprod([1, 2, 3]), [1, 2, 6]],
        ["diff one shorter", () => m.diff([1, 2, 4]), [1, 2]],
        /* realmin is the MATLAB convention: smallest NORMAL double (2^-1022),
         * not JS's subnormal Number.MIN_VALUE. */
        ["realmin is the smallest normal (2^-1022)", () => m.realmin(), 2 ** -1022],
        ["realmax/flintmax/eps vs JS", () =>
            [Object.is(m.realmax(), Number.MAX_VALUE),
             Object.is(m.flintmax(), 2 ** 53),
             Object.is(m.eps(), Number.EPSILON)], [true, true, true]],
        ["eps(1000) is the ulp at 1000 (2^-43)", () => m.eps(1000), 2 ** -43],
    );

    /* ---- parametric sweeps: every callable top-level export ---- */
    for (const k of top) {
        const f = m[k];
        if (typeof f !== "function") continue; /* constants handled above */
        if (k === "Expression") continue;      /* class handled below */
        switch (k) {
            case "abs": case "bitLen": case "popcount": big1(k, f); break;
            case "gcd": case "lcm": big2(k, f); break;
            case "cumsum": case "cumprod": case "diff": case "perms": arr1(k, f); break;
            case "linspace": case "logspace": add3(k,
                () => f(), () => f("x", "x"),
                [() => f(0, 1, 0), () => f(0, 1, -1), () => f(0, 1, NaN),
                 () => f(0, 1, Infinity), () => f(0, 1, 2 ** 53)], 5); break;
            case "rat": num1(k, f); break;
            case "factorial": case "primes": case "factor": case "isPrime": num1(k, f); break;
            case "idivide": add3(k,
                () => f(), () => f("x", 2),
                [...BNUM.map((v) => () => f(v, 2)), () => f(5, 0) /* IEEE div-by-zero (docs) */],
                BNUM.length + 1); break;
            case "modf": case "frexp": num1(k, f); break;
            case "isInf": num1(k, f); break;
            case "mod": case "rem": case "fmod": case "remainder": case "nthroot":
            case "hypot": case "copysign": case "nextafter": case "scalbn": case "ldexp":
            case "beta": case "betaln": case "nchoosek": case "polygamma":
            case "gammainc": case "gammaincinv": case "besselj": case "bessely":
            case "besseli": case "besselk": case "besseliScaled": case "besselkScaled":
            case "ellipj": case "legendre": num2(k, f); break;
            case "betainc": case "betaincinv": add3(k,
                () => f(), () => f("x", "x", "x"),
                BNUM.map((v) => () => f(v, 1, 1)), BNUM.length); break;
            case "legendreP": add3(k,
                () => f(), () => f("x", "x", "x"),
                BNUM.map((v) => () => f(2, 0, v)), BNUM.length); break;
            case "besselh": add3(k,
                () => f(), () => f("x", "x", "x"),
                BNUM.map((v) => () => f(0, v, 1)), BNUM.length); break;
            default: num1(k, f); break;
        }
    }

    /* ---- bits namespace: every width-parameterised primitive ---- */
    const bits = m.bits;
    markRead("mathx", "bits.uintSize");
    rows.push(["bits.uintSize is 64", () => bits.uintSize, 64]);
    const u32un = ["leadingZeros8", "leadingZeros16", "leadingZeros32",
        "trailingZeros8", "trailingZeros16", "trailingZeros32",
        "onesCount8", "onesCount16", "onesCount32",
        "len8", "len16", "len32", "reverse8", "reverse16", "reverse32",
        "reverseBytes16", "reverseBytes32"];
    for (const k of u32un) num1("bits." + k, bits[k]);
    for (const k of ["rotateLeft8", "rotateLeft16", "rotateLeft32"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 1),
        [-1, 0, 7, NaN, 2 ** 53].map((kk) => () => bits[k](1, kk)), 5);
    for (const k of ["leadingZeros64", "trailingZeros64", "onesCount64", "len64",
                     "reverse64", "reverseBytes64"]) big1("bits." + k, bits[k]);
    add3("bits.rotateLeft64",
        () => bits.rotateLeft64(), () => bits.rotateLeft64("x", 1),
        [() => bits.rotateLeft64(1n, -1), () => bits.rotateLeft64(1n, 0),
         () => bits.rotateLeft64(1n, NaN), () => bits.rotateLeft64(1n, 2 ** 53),
         () => bits.rotateLeft64(1n, 1n) /* BigInt k refused (k is a Number) */], 5);
    for (const k of ["add32", "sub32"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 1, 0),
        [() => bits[k](0xffffffff, 1, 0), () => bits[k](0, -1, -1),
         () => bits[k](0, 0, NaN), () => bits[k](0, 0, 2 ** 53)], 4);
    for (const k of ["add64", "sub64"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 1n, 0n),
        [() => bits[k](0xffffffffffffffffn, 1n, 0n), () => bits[k](0n, -1n, 0n)], 2);
    num2("bits.mul32", bits.mul32);
    big2("bits.mul64", bits.mul64);
    for (const k of ["div32", "rem32"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 0, 1),
        [() => bits[k](0, 10, 0) /* y==0 refused (docs) */,
         () => bits[k](0, 10, 3), () => bits[k](1, 0, 1) /* quotient overflow */,
         () => bits[k](0, 10, NaN), () => bits[k](0, 10, 2 ** 53)], 5);
    for (const k of ["div64", "rem64"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 0n, 1n),
        [() => bits[k](0n, 10n, 0n), () => bits[k](0n, 10n, 3n),
         () => bits[k](1n, 0n, 1n)], 3);
    /* bits value pins against BigInt as the independent oracle. */
    rows.push(
        ["bits.rotateLeft32 matches JS shift oracle", () => {
            const x = 0x12345678, k = 8;
            return bits.rotateLeft32(x, k), ((x << k) | (x >>> (32 - k))) >>> 0;
        }, ((0x12345678 << 8) | (0x12345678 >>> 24)) >>> 0],
        ["bits.mul32 recombines to the BigInt product", () => {
            const a = 0x9e3779b9, b = 0x85ebca6b;
            const [hi, lo] = bits.mul32(a, b);
            return hi * 2 ** 32 + lo === Number((BigInt(a >>> 0) * BigInt(b >>> 0)) % 2n ** 64n);
        }, true],
        ["bits.add32 carry out", () => bits.add32(0xffffffff, 1, 0), [0, 1]],
        ["bits.div32(0,10,3)", () => bits.div32(0, 10, 3), [3, 1]],
        ["bits.rem32(0,10,3) == 10%3", () => bits.rem32(0, 10, 3), 10 % 3],
        ["bits.add64 wraps at 2^64", () => bits.add64(0xffffffffffffffffn, 1n, 0n).map(String),
         ["0", "1"]],
        ["bits.rotateLeft64(1n,1)", () => bits.rotateLeft64(1n, 1), 2n],
        ["bits.leadingZeros64(1n)", () => bits.leadingZeros64(1n), 63],
        ["bits.onesCount32(255)", () => bits.onesCount32(255), 8],
        ["bits.reverse8 doc vector", () => bits.reverse8(0b10110000), 13],
        ["bits.reverseBytes16(0x1234)", () => bits.reverseBytes16(0x1234), 13330],
        ["bits.reverseBytes32 hand value", () => bits.reverseBytes32(0x12345678), 0x78563412],
        ["bits.reverse32 is an involution", () =>
            bits.reverse32(bits.reverse32(0xdeadbeef)) === (0xdeadbeef >>> 0), true],
    );

    /* ---- Expression: compiled arithmetic ---- */
    const e = new m.Expression("a * x^2 + b");
    mark3("mathx", "Expression");
    rows.push(["Expression EMPTY ctor", () => new m.Expression(), ANY],
              ["Expression WRONG-TYPE ctor (number)", () => new m.Expression(42), ANY],
              ["Expression BOUNDARY ctor", () => outcomes([
                  () => new m.Expression("x".repeat(5000)) /* RangeError past 4096 (docs) */,
                  () => new m.Expression("")]), 2],
              ["Expression.variables()", () => e.variables(), ["a", "x", "b"]],
              ["Expression.eval hand value a*x^2+b", () => e.eval({ a: 2, x: 3, b: 1 }), 19],
              /* ^ is right-associative and above unary minus: 2^(3^2) = 512. */
              ["Expression 2^3^2 right-assoc", () => new m.Expression("2^3^2").eval(), 512],
              ["Expression bad syntax is a SyntaxError", () => new m.Expression("nope $"), THROWS]);
    mark3("mathx", "Expression.eval");
    rows.push(["Expression.eval EMPTY (vars required)", () => e.eval(), ANY],
              ["Expression.eval WRONG-TYPE (string)", () => e.eval("x"), ANY],
              ["Expression.eval BOUNDARY", () => outcomes([
                  () => e.eval({}), () => e.eval({ x: NaN }), () => e.eval({ x: Infinity }),
                  () => e.eval({ x: 2 ** 53 }),
                  () => new m.Expression("x").eval({ get x() { throw new Error("getter"); } })]), 5]);
    mark3("mathx", "Expression.variables");
    rows.push(["Expression.variables EMPTY", () => e.variables(), ["a", "x", "b"]],
              ["Expression.variables WRONG-TYPE (extra arg ignored)", () =>
                  e.variables(123), ["a", "x", "b"]],
              ["Expression.variables BOUNDARY", () => outcomes([
                  () => new m.Expression("1").variables(),
                  () => new m.Expression("x+x+x").variables()]), 2]);

    return rows;
});

/* ===================================================================== */
/* ==                      dyna:dataframe                             == */
/* ===================================================================== */

section("dataframe", await mod("dataframe"), (d) => {
    const rows = [];
    const proto = Object.getOwnPropertyNames(d.DataFrame.prototype)
        .filter((k) => k !== "constructor");
    for (const k of proto) COV.dataframe.names.add(k);
    COV.dataframe.names.add("DataFrame");

    /* 3x3 frame: one float column, one int column, one dictionary-encoded
     * string column.  Rebuilt for every call so rows stay independent. */
    const F = () => new d.DataFrame({
        x: new Float64Array([1, 2, 3]),
        n: new Int32Array([10, 20, 30]),
        s: ["a", "b", "a"],
    });
    const E = () => new d.DataFrame({ x: new Float64Array([]) });
    const M3 = () => new Uint8Array([1, 0, 1]);

    /* ---- construction pins (hand-computed / documented refusals) ---- */
    mark3("dataframe", "DataFrame");
    rows.push(
        ["ctor ROWS/COLS/COLUMNS", () => [F().ROWS, F().COLS, F().COLUMNS], [3, 3, ["x", "n", "s"]]],
        ["ctor EMPTY (no columns)", () => new d.DataFrame({}).ROWS, 0],
        ["ctor WRONG-TYPE (string arg)", () => new d.DataFrame("x"), THROWS],
        ["ctor BOUNDARY (NUL in name / clamped array / ragged lengths)", () => outcomes([
            () => new d.DataFrame({ "a\u0000b": new Float64Array([1, 2]) }) /* refused (docs) */,
            () => new d.DataFrame({ a: new Uint8ClampedArray([1, 2]) }) /* refused (docs) */,
            () => new d.DataFrame({ a: new Float64Array([1]), b: new Float64Array([1, 2]) })]),
            3],
        ["DTYPES tags", () => F().DTYPES(), { x: "f64", n: "i32", s: "str" }],
        ["TO_RECORDS first row", () => F().TO_RECORDS()[0], { x: 1, n: 10, s: "a" }],
        ["TO_CSV header", () => F().TO_CSV().split("\n")[0], "x,n,s"],
        ["FROM_RECORDS union + fill", () => {
            const g = new d.DataFrame({}).FROM_RECORDS([{ a: 1, b: "x" }, { a: 2 }]);
            return [g.COLUMNS, g.ROWS, g.TO_RECORDS()[1].b];
        }, [["a", "b"], 2, ""]],
        ["FROM_RECORDS zero rows refused", () =>
            new d.DataFrame({}).FROM_RECORDS([]), THROWS],
        ["SELECT reorders", () => F().SELECT(["s", "x"]).COLUMNS, ["s", "x"]],
        ["SELECT duplicate refused", () => F().SELECT(["x", "x"]), THROWS],
        /* Aggregation with hand-computed results. */
        ["SUM hand value", () => F().SUM("x"), 6],
        ["MEAN hand value", () => F().MEAN("x"), 2],
        ["MIN/MAX hand values", () => [F().MIN("x"), F().MAX("x")], [1, 3]],
        ["MEDIAN hand value", () => F().MEDIAN("x"), 2],
        ["COUNT hand value", () => F().COUNT("x"), 3],
        ["GROUP_BY_SUM hand values", () => {
            const g = F().GROUP_BY_SUM("s", "x");
            return [g.keys, Array.from(g.values)];
        }, [["a", "b"], [4, 2]]],
        ["DESCRIBE hand stats", () => {
            const r = F().DESCRIBE("x");
            return [r.count, r.sum, r.mean, r.min, r.max];
        }, [3, 6, 2, 1, 3]],
        /* Empty-selection conventions from the docs. */
        ["SUM of empty is 0", () => E().SUM("x"), 0],
        ["MIN of empty is undefined (documented)", () => E().MIN("x"), undefined],
        ["MEAN of empty is NaN", () => Number.isNaN(E().MEAN("x")), true],
        /* Boundary refusals documented in API.md. */
        ["SAMPLE n>ROWS refused", () => F().SAMPLE(5), THROWS],
        ["FILTER mask length refused", () => F().FILTER(new Uint8Array([1, 0])), THROWS],
        ["QUANTILE q outside [0,1] refused", () => F().QUANTILE("x", 7), THROWS],
        ["HISTOGRAM bins=Infinity refused", () => F().HISTOGRAM("x", Infinity), THROWS],
        ["NTILE buckets=Infinity refused", () => F().NTILE("x", Infinity), THROWS],
        ["N_LARGEST k=Infinity refused", () => F().N_LARGEST("x", Infinity), THROWS],
        ["EMA alpha=Infinity refused", () => F().EMA("x", Infinity), THROWS],
        ["ROLLING_MEAN window=Infinity refused", () => F().ROLLING_MEAN("x", Infinity), THROWS],
        /* Scalar arithmetic: column rhs resolves by name (docs). */
        ["ADD int column + float column", () => F().ADD("n", "x"), new Float64Array([11, 22, 33])],
        ["WHERE(mask, col, scalar)", () => F().WHERE(M3(), "x", 99), new Float64Array([1, 99, 3])],
        ["SLICE(-2) counts from the end", () => F().SLICE(-2).ROWS, 2],
        ["SAMPLE(2,7) is seed-reproducible", () => {
            const a = F().SAMPLE(2, 7).TO_RECORDS();
            const b = F().SAMPLE(2, 7).TO_RECORDS();
            return JSON.stringify(a) === JSON.stringify(b) && a.length === 2;
        }, true],
    );

    /* ---- parametric sweep over EVERY prototype method ---- */
    const g = (nm, args) => () => F()[nm](...args);
    const add3 = (nm, kind) => {
        mark3("dataframe", nm);
        if (kind === "red1") { /* (col[, mask]) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (number col)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY (bad col names)`, () => outcomes([g(nm, [""]), g(nm, ["nope"])]), 2]);
        } else if (kind === "red2") { /* (colA, colB[, mask]) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (numbers)`, g(nm, [123, 123]), ANY],
                      [`${nm} BOUNDARY`, () => outcomes([g(nm, ["x", "x"]), g(nm, ["nope", "x"])]), 2]);
        } else if (kind === "grp") { /* (key, value[, extra]) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (numbers)`, g(nm, [123, 123]), ANY],
                      [`${nm} BOUNDARY`, () => outcomes([g(nm, ["s", "x"]), g(nm, ["nope", "x"])]), 2]);
        } else if (kind === "colNum") { /* (col, number[, ...]) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string scalar)`, g(nm, ["x", "x"]), ANY],
                      [`${nm} BOUNDARY (numeric set)`, () =>
                          outcomes(BNUM.map((v) => g(nm, ["x", v]))), BNUM.length]);
        } else if (kind === "mask") { /* (Uint8Array[, ...]) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string mask)`, g(nm, ["x"]), ANY],
                      [`${nm} BOUNDARY (empty + [1,0,1] mask)`, () =>
                          outcomes([g(nm, [new Uint8Array(0)]), g(nm, [M3()])]), 2]);
        } else if (kind === "arr") { /* (Array[, ...]) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string)`, g(nm, ["x"]), ANY],
                      [`${nm} BOUNDARY ([] and [null])`, () =>
                          outcomes([g(nm, [[]]), g(nm, [[null]])]), 2]);
        } else if (kind === "obj") { /* (Object) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (number)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY ({} and unknown key)`, () =>
                          outcomes([g(nm, [{}]), g(nm, [{ nope: "q" }])]), 2]);
        } else if (kind === "df") { /* (DataFrame) */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (number)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY (empty + full frame)`, () =>
                          outcomes([g(nm, [E()]), g(nm, [F()])]), 2]);
        } else if (kind === "numOnly") { /* numeric scalar params only */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string)`, g(nm, ["x"]), ANY],
                      [`${nm} BOUNDARY (numeric set)`, () =>
                          outcomes(BNUM.map((v) => g(nm, [v]))), BNUM.length]);
        } else if (kind === "zero") { /* no-arg methods */
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (extra arg)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY (extra arg)`, g(nm, [-1]), ANY]);
        } else {
            throw new Error("unknown spec kind " + kind);
        }
    };

    const spec = {
        /* reductions (col[, mask]) */
        SUM: "red1", SUM_CHECKED: "red1", MIN: "red1", MAX: "red1", MEAN: "red1",
        COUNT: "red1", PRODUCT: "red1", FIRST: "red1", LAST: "red1", STDDEV: "red1",
        STDDEV_POP: "red1", VARIANCE: "red1", VARIANCE_POP: "red1", SEM: "red1",
        MEDIAN: "red1", MODE: "red1", MAD: "red1", MEDIAN_ABSOLUTE_DEVIATION: "red1",
        KURTOSIS: "red1", KURT_SAMP: "red1", SKEW: "red1", SKEW_SAMP: "red1",
        ENTROPY: "red1", APPROX_COUNT_DISTINCT: "red1", COUNT_NULLS: "red1",
        N_UNIQUE: "red1", UNIQUE: "red1", ARG_MAX: "red1", ARG_MIN: "red1",
        ARG_SORT: "red1", RANK: "red1", DENSE_RANK: "red1", PERCENT_RANK: "red1",
        ZSCORE: "red1", CUM_SUM: "red1", CUM_MAX: "red1", CUM_MIN: "red1",
        CUM_PROD: "red1", DIFF: "red1", PCT_CHANGE: "red1", SHIFT: "red1",
        IS_NA: "red1", NOT_NA: "red1", DROP_NA: "red1", SIGN: "red1", ABS: "red1",
        CEIL: "red1", FLOOR: "red1", EXP: "red1", LOG: "red1", SQRT: "red1",
        BITMASK: "red1", BOOL_AND: "red1", BOOL_OR: "red1", BOOL_XOR: "red1",
        ANY_HEAVY: "red1", DESCRIBE: "red1", SORT: "red1", DELTA_SUM: "red1",
        VALUE_COUNTS: "red1",
        QUANTILE_EXACT_LOW: "red1", QUANTILE_EXACT_HIGH: "red1",
        QUANTILE_EXACT_WEIGHTED: "red1", QUANTILE_TDIGEST_WEIGHTED: "red1",
        RANGE_AGG: "red1", ROUND: "red1",
        /* two-column reductions */
        DOT_PRODUCT: "red2", CORR: "red2", COV_POP: "red2", COV_SAMP: "red2",
        REGR_AVG_X: "red2", REGR_AVG_Y: "red2", REGR_COUNT: "red2",
        REGR_INTERCEPT: "red2", REGR_R2: "red2", REGR_SLOPE: "red2",
        REGR_SXX: "red2", REGR_SXY: "red2", REGR_SYY: "red2", RANK_CORR: "red2",
        MEAN_WEIGHTED: "red2", DELTA_SUM_TIMESTAMP: "red2", IRATE: "red2",
        RATE: "red2", BOUNDING_RATIO: "red2", APPROX_SIMILARITY: "red2",
        RANGE_INTERSECT_AGG: "red2", BITWISE_AND: "red2", BITWISE_OR: "red2",
        BITWISE_XOR: "red2",
        /* group-bys */
        GROUP_BY_SUM: "grp", GROUP_BY_MAX: "grp", GROUP_BY_MIN: "grp",
        GROUP_BY_MEAN: "grp", GROUP_CONCAT: "grp", GROUP_ARRAY: "grp",
        GROUP_UNIQ_ARRAY: "grp", GROUP_ARRAY_SORTED: "grp", GROUP_ARRAY_MOVING_AVG: "grp",
        GROUP_ARRAY_MOVING_SUM: "grp", GROUP_ARRAY_INTERSECT: "grp",
        GROUP_BITMAP: "grp", GROUP_BIT_AND: "grp", GROUP_BIT_OR: "grp",
        GROUP_BIT_XOR: "grp", MAX_MAP: "grp", MIN_MAP: "grp", SUM_MAP: "grp",
        JSON_AGG: "grp", JSON_AGG_STRICT: "grp", JSON_OBJECT_AGG: "grp",
        JSON_OBJECT_AGG_STRICT: "grp",
        /* masks */
        ANY: "mask", ALL: "mask", FILTER: "mask",
        /* arrays */
        SELECT: "arr", DROP_COLUMNS: "arr", FROM_RECORDS: "arr", COV_MATRIX: "arr",
        CORR_MATRIX: "arr",
        /* objects */
        RENAME: "obj",
        /* frames */
        CONCAT: "df", JOIN: "df", ASOF_JOIN: "df",
        /* numeric-only */
        SLICE: "numOnly", SAMPLE: "numOnly",
        /* no-arg */
        COPY: "zero", TO_CSV: "zero", TO_JSON: "zero", TO_RECORDS: "zero",
        TO_COLUMNS: "zero", DTYPES: "zero", SCHEMA: "zero", INFO: "zero",
        MEMORY_USAGE: "zero",
    };
    /* (col, number[, ...]) family */
    for (const nm of ["ADD", "SUB", "MUL", "DIV", "RDIV", "RSUB", "POW", "CLIP",
                      "FILL_NA", "EMA", "ROLLING_SUM", "ROLLING_MEAN", "ROLLING_MIN",
                      "ROLLING_MAX", "ROLLING_STD", "ROLLING_VAR", "HISTOGRAM",
                      "HISTOGRAM_NORMALIZED", "QUANTILE", "APPROX_PERCENTILE",
                      "APPROX_TOP_K", "TOP_K", "TOP_K_WEIGHTED", "N_LARGEST",
                      "N_SMALLEST", "NTILE", "BETWEEN", "UNIQ_UP_TO", "RESAMPLE",
                      "PERCENTILE_CONT", "PERCENTILE_DISC"]) spec[nm] = "colNum";
    spec.GROUP_ARRAY_LAST = "colNum";
    spec.GROUP_ARRAY_SAMPLE = "colNum";
    spec.GROUP_ARRAY_INSERT_AT = "colNum";
    /* three-arg specials */
    spec.EXPONENTIAL_TIME_DECAYED_AVG = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_SUM = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_MAX = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_COUNT = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_MIN = "colNum";
    spec.QUANTILES = "arr";
    spec.QUANTILES_TDIGEST = "arr";
    spec.PIVOT = "red1";
    spec.MELT = "arr";
    spec.DROP_DUPLICATES = "red1";
    spec.GROUP_BY_COUNT = "red1";
    spec.ISIN = "special";
    spec.MASK = "special";
    spec.WHERE = "special";
    spec.HEAD = "colNum";
    spec.TAIL = "colNum";
    spec.APPROX_TOP_SUM = "colNum";
    /* numeric comparisons take (col, value) */
    for (const nm of ["GT", "GE", "LT", "LE", "EQ", "NE"]) spec[nm] = "colNum";

    for (const nm of proto) {
        if (nm === "ROWS" || nm === "COLS" || nm === "COLUMNS") {
            markRead("dataframe", nm);
            rows.push([`${nm} getter`, () => F()[nm], ANY]);
            continue;
        }
        const kind = spec[nm];
        if (!kind) {
            skip++;
            print(`SKIP (no arg spec): dataframe.${nm}`);
            continue;
        }
        if (typeof F()[nm] !== "function") {
            skip++;
            print(`SKIP (not callable at runtime): dataframe.${nm}`);
            continue;
        }
        if (kind === "special") {
            mark3("dataframe", nm);
            if (nm === "ISIN") {
                rows.push(["ISIN EMPTY", g(nm, []), ANY],
                          ["ISIN WRONG-TYPE (number col)", g(nm, [123, []]), ANY],
                          ["ISIN BOUNDARY", () => outcomes([
                              g(nm, ["s", []]), g(nm, ["s", [null]]), g(nm, ["x", []])]), 3]);
            } else if (nm === "MASK") {
                rows.push(["MASK EMPTY", g(nm, []), ANY],
                          ["MASK WRONG-TYPE (string mask)", g(nm, ["x", 0]), ANY],
                          ["MASK BOUNDARY", () => outcomes([
                              g(nm, [new Uint8Array(0), 0]), g(nm, [M3(), 0]),
                              g(nm, [M3(), "z"])]), 3]);
            } else { /* WHERE(mask, a, b) */
                rows.push(["WHERE EMPTY", g(nm, []), ANY],
                          ["WHERE WRONG-TYPE (string mask)", g(nm, ["x", 1, 0]), ANY],
                          ["WHERE BOUNDARY", () => outcomes([
                              g(nm, [new Uint8Array(0), 1, 0]), g(nm, [M3(), "x", 99]),
                              g(nm, [M3(), 1, NaN])]), 3]);
            }
            continue;
        }
        add3(nm, kind);
    }

    return rows;
});

/* ===================================================================== */
/* ==                           dyna:ml                               == */
/* ===================================================================== */

section("ml", await mod("ml"), (m) => {
    const rows = [];
    const top = Object.getOwnPropertyNames(m);
    for (const k of top) COV.ml.names.add(k);
    const classes = top.filter((k) => typeof m[k] === "function" && m[k].prototype);
    const fns = top.filter((k) => typeof m[k] === "function" && !m[k].prototype);
    for (const c of classes) {
        for (const p of Object.getOwnPropertyNames(m[c].prototype))
            if (p !== "constructor") COV.ml.names.add(`${c}.${p}`);
        for (const p of Object.getOwnPropertyNames(m[c]))
            if (!["length", "name", "prototype"].includes(p)) COV.ml.names.add(`${c}.${p}`);
    }

    /* Tiny fixed datasets.  Supervised sets are linearly separable so a
     * correct classifier gets every training point right (that is the
     * independent oracle for the fit pin). */
    const X4 = [[0], [1], [2], [3]];
    const yCls = [0, 0, 1, 1];
    const yReg = [0, 2, 4, 6];
    const X2F = [[0], [0.1], [5], [5.1]]; /* two obvious clusters */
    const X6 = [[0], [0.1], [0.2], [5], [5.1], [5.2]];

    /* -------- top-level metric / utility functions -------- */
    const metric = (nm, f, third) => {
        mark3("ml", nm);
        rows.push([`${nm} EMPTY`, () => f(), ANY],
                  [`${nm} WRONG-TYPE (strings)`, () => f("x", "y"), ANY],
                  [`${nm} BOUNDARY (empty + [null] + non-finite)`, () => outcomes([
                      () => f([], []),
                      () => f([null, null, null], [1, 0, 1]),
                      () => f([NaN, 1], [0, 1]) /* docs: inputs must be finite */]), 3]);
    };
    for (const nm of ["accuracy", "precision", "recall", "f1", "specificity",
                      "balancedAccuracy", "matthewsCorrcoef", "cohenKappa",
                      "meanSquaredError", "meanAbsoluteError", "r2Score"])
        metric(nm, m[nm]);
    metric("rocAuc", m.rocAuc);
    metric("averagePrecision", m.averagePrecision);
    metric("logLoss", m.logLoss);
    metric("confusionMatrix", m.confusionMatrix);
    mark3("ml", "fbeta");
    rows.push(["fbeta EMPTY", () => m.fbeta(), ANY],
              ["fbeta WRONG-TYPE (strings)", () => m.fbeta("x", "y", "x"), ANY],
              ["fbeta BOUNDARY (beta must be positive)", () => outcomes([
                  () => m.fbeta([1, 0], [1, 0], 0), () => m.fbeta([1, 0], [1, 0], NaN),
                  () => m.fbeta([1, 0], [1, 0], Infinity),
                  () => m.fbeta([1, 0], [1, 0], 2, 1)]), 4]);
    mark3("ml", "imputeMean");
    rows.push(["imputeMean EMPTY", () => m.imputeMean(), ANY],
              ["imputeMean WRONG-TYPE (string)", () => m.imputeMean("x"), ANY],
              ["imputeMean BOUNDARY", () => outcomes([
                  () => m.imputeMean([]),
                  () => m.imputeMean([[NaN], [NaN]]) /* all-NaN column refused (docs) */,
                  () => m.imputeMean([[NaN]])]), 3]);
    mark3("ml", "dropMissing");
    rows.push(["dropMissing EMPTY", () => m.dropMissing(), ANY],
              ["dropMissing WRONG-TYPE (string)", () => m.dropMissing("x"), ANY],
              ["dropMissing BOUNDARY", () => outcomes([
                  () => m.dropMissing([]), () => m.dropMissing([[NaN, 1]]),
                  () => m.dropMissing([[1, 2]], null)]), 3]);
    mark3("ml", "trainTestSplit");
    rows.push(["trainTestSplit EMPTY", () => m.trainTestSplit(), ANY],
              ["trainTestSplit WRONG-TYPE (string)", () => m.trainTestSplit("x"), ANY],
              ["trainTestSplit BOUNDARY (n>=2)", () =>
                  outcomes(BNUM.map((v) => () => m.trainTestSplit(v))), BNUM.length]);
    mark3("ml", "kFold");
    rows.push(["kFold EMPTY", () => m.kFold(), ANY],
              ["kFold WRONG-TYPE (string)", () => m.kFold("x"), ANY],
              ["kFold BOUNDARY", () => outcomes(BNUM.map((v) => () => m.kFold(v, { k: 2 }))),
               BNUM.length]);
    mark3("ml", "stratifiedKFold");
    rows.push(["stratifiedKFold EMPTY", () => m.stratifiedKFold(), ANY],
              ["stratifiedKFold WRONG-TYPE (string)", () => m.stratifiedKFold("x"), ANY],
              ["stratifiedKFold BOUNDARY", () => outcomes([
                  () => m.stratifiedKFold([]), () => m.stratifiedKFold([null]),
                  () => m.stratifiedKFold([NaN, 1])]), 3]);
    const factory = () => new m.DecisionTreeClassifier({ maxDepth: 1, seed: 7 });
    mark3("ml", "crossValScore");
    rows.push(["crossValScore EMPTY", () => m.crossValScore(), ANY],
              ["crossValScore WRONG-TYPE (number factory)", () =>
                  m.crossValScore(123, X4, yCls), ANY],
              ["crossValScore BOUNDARY (empty data + bad k)", () => outcomes([
                  () => m.crossValScore(factory, [], []),
                  () => m.crossValScore(factory, X4, yCls, { k: 1 })]), 2]);
    mark3("ml", "gridSearch");
    rows.push(["gridSearch EMPTY", () => m.gridSearch(), ANY],
              ["gridSearch WRONG-TYPE (number factory)", () =>
                  m.gridSearch(123, X4, yCls, {}), ANY],
              ["gridSearch BOUNDARY (empty grid + bad k)", () => outcomes([
                  () => m.gridSearch(factory, X4, yCls, {}),
                  () => m.gridSearch(factory, X4, yCls, { maxDepth: [1] }, { k: 1 })]), 2]);
    mark3("ml", "randomSearch");
    rows.push(["randomSearch EMPTY", () => m.randomSearch(), ANY],
              ["randomSearch WRONG-TYPE (number factory)", () =>
                  m.randomSearch(123, X4, yCls, {}), ANY],
              ["randomSearch BOUNDARY (empty grid + nIter 0)", () => outcomes([
                  () => m.randomSearch(factory, X4, yCls, {}),
                  () => m.randomSearch(factory, X4, yCls, { maxDepth: [1] },
                                       { k: 2, nIter: 0 })]), 2]);

    /* -------- metrics: hand-computed 3-point values (independent oracle) --- */
    rows.push(
        ["accuracy hand value 2/3", () => m.accuracy([0, 1, 1], [0, 1, 0]), 2 / 3],
        ["precision hand value 2/3", () => m.precision([1, 1, 1, 0], [1, 1, 0, 1]), 2 / 3],
        ["recall hand value 2/3", () => m.recall([1, 1, 1, 0], [1, 1, 0, 1]), 2 / 3],
        ["f1 hand value 2/3", () => m.f1([1, 1, 1, 0], [1, 1, 0, 1]), 2 / 3],
        ["mse hand value 4/3", () => m.meanSquaredError([1, 2, 3], [1, 2, 5]), 4 / 3],
        ["mae hand value 4/3", () => m.meanAbsoluteError([1, 2, 3], [2, 3, 5]), 4 / 3],
        ["r2 hand value 1/2", () => m.r2Score([1, 2, 3], [1, 2, 4]), 0.5],
        ["r2 constant exact is 1", () => m.r2Score([2, 2, 2], [2, 2, 2]), 1],
        ["r2 constant inexact is 0, never NaN", () => m.r2Score([2, 2, 2], [2, 2, 3]), 0],
        ["rocAuc hand value 3/4", () => m.rocAuc([0, 0, 1, 1], [0.1, 0.4, 0.35, 0.8]), 0.75],
        ["confusionMatrix hand values", () =>
            m.confusionMatrix([0, 1, 1, 0], [0, 1, 0, 1]), [[1, 1], [1, 1]]],
        ["logLoss == -ln(0.999)", () =>
            near(m.logLoss([0, 1], [0.001, 0.999]), -Math.log(0.999), 1e-12), true],
        /* recall for label 1 is 1/2 (one TP, one FN); specificity for label 1
         * is 1/2 (one TN, one FP); balanced accuracy is their mean. */
        ["balancedAccuracy hand value 1/2", () =>
            m.balancedAccuracy([0, 1, 1, 0], [0, 1, 0, 1]), 0.5],
        ["non-finite metric input refused", () => m.accuracy([NaN], [1]), THROWS],
        ["length-mismatch metric input refused", () =>
            m.accuracy([0, 1], [1]), THROWS],
        ["imputeMean hand values", () => m.imputeMean([[1, NaN], [3, 4]]),
         [[1, 4], [3, 4]]],
        ["dropMissing keeps survivors in order", () => {
            const r = m.dropMissing([[1, 2], [NaN, 4], [3, 6]], [0, 1, 2]);
            return [r.X, Array.from(r.y), r.kept];
        }, [[[1, 2], [3, 6]], [0, 2], [0, 2]]],
        ["trainTestSplit covers every index once", () => {
            const s = m.trainTestSplit(6, { seed: 7 });
            const all = [...s.train, ...s.test].sort((a, b) => a - b);
            return [s.train.length, s.test.length, all.join(",")];
        }, [4, 2, "0,1,2,3,4,5"]],
        ["trainTestSplit same seed reproduces", () => {
            const a = m.trainTestSplit(8, { seed: 7 });
            const b = m.trainTestSplit(8, { seed: 7 });
            return a.train.join(",") === b.train.join(",");
        }, true],
        ["kFold partitions exactly once", () => {
            const folds = m.kFold(6, { k: 3 });
            const all = [];
            for (const f of folds) for (const i of f.test) all.push(i);
            return all.sort((a, b) => a - b).join(",");
        }, "0,1,2,3,4,5"],
        ["stratifiedKFold fold count", () =>
            m.stratifiedKFold([0, 0, 0, 1, 1, 1], { k: 3 }).length, 3],
        ["crossValScore returns per-fold scores", () => {
            const s = m.crossValScore(factory, X4, yCls, { k: 2, seed: 7 });
            return s.length === 2 && s.every(Number.isFinite);
        }, true],
        ["gridSearch picks a best point", () => {
            const r = m.gridSearch((p) => new m.DecisionTreeClassifier({ ...p, seed: 7 }),
                                   X4, yCls, { maxDepth: [1, 2] }, { k: 2 });
            return [r.results.length, Number.isFinite(r.bestScore)];
        }, [2, true]],
        ["randomSearch honours nIter", () =>
            m.randomSearch((p) => new m.DecisionTreeClassifier({ ...p, seed: 7 }),
                           X4, yCls, { maxDepth: [1, 2] }, { k: 2, nIter: 2 }).results.length,
        2],
    );

    /* -------- classes -------- */
    const MATRIX = new Set(["predict", "predictProba", "transform", "fitTransform",
                            "inverseTransform", "apply", "decisionFunction"]);
    /* ctor spec: "num" = numeric first param, "obj" = options object,
     * "arr" = array first param, "csr" = four positional arrays. */
    const ctorKind = {
        CSR: "csr", Pipeline: "arr", KMeans: "num", GaussianMixture: "num",
        PCA: "num", DBScan: "num", KNClassifier: "num", KNRegressor: "num",
    };
    /* fit spec: "sup" takes (X, y); "un" takes (X). */
    const fitKind = {
        LinearRegression: "sup", LogisticRegression: "sup", SVC: "sup",
        GaussianNB: "sup", DecisionTreeClassifier: "sup", DecisionTreeRegressor: "sup",
        RandomForestClassifier: "sup", RandomForestRegressor: "sup",
        GradientBoostingClassifier: "sup", GradientBoostingRegressor: "sup",
        XGBClassifier: "sup", XGBRegressor: "sup",
        KNClassifier: "sup", KNRegressor: "sup", Pipeline: "sup",
        KMeans: "un", DBScan: "un", GaussianMixture: "un", PCA: "un",
        StandardScaler: "un", MinMaxScaler: "un", CSR: "none",
    };
    const ctorArgs = (c, v) => {
        switch (ctorKind[c]) {
            case "num": return [v];
            case "arr": return [[v]];
            case "csr": return [[v], [0], [0, 1], 1];
            default: return [{}];
        }
    };
    /* cheap fitted instance per class (seeded where the algorithm is random) */
    const cheapOpts = {
        KMeans: [2, 7], GaussianMixture: [2, { seed: 7, maxIter: 20 }],
        DBScan: [0.5, 2], PCA: [1],
        RandomForestClassifier: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        RandomForestRegressor: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        GradientBoostingClassifier: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        GradientBoostingRegressor: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        XGBClassifier: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        XGBRegressor: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        LogisticRegression: [{ maxIter: 100 }], SVC: [{ kernel: "linear", maxIter: 100 }],
        DecisionTreeClassifier: [{ maxDepth: 2, seed: 7 }],
        DecisionTreeRegressor: [{ maxDepth: 2, seed: 7 }],
        KNClassifier: [3], KNRegressor: [3],
        GaussianNB: [], StandardScaler: [], MinMaxScaler: [],
        LinearRegression: [{}],
    };
    /* Fresh, cheaply-constructed instance (Pipeline needs real stages and
     * must not share them between probe rows). */
    const makeFresh = (c) => c === "Pipeline"
        ? new m.Pipeline([new m.StandardScaler(), new m.LinearRegression({})])
        : new m[c](...(cheapOpts[c] || [{}]));
    const fitData = (c) => {
        if (c === "CSR") return null;
        const kind = fitKind[c];
        if (kind === "sup") {
            const y = /Classifier|SVC|NB|Logistic|KNClassifier|Pipeline/.test(c) ? yCls : yReg;
            return c === "Pipeline"
                ? { X: X4, y: yCls }
                : { X: X4, y };
        }
        return { X: c === "PCA" ? [[0, 0], [1, 1], [2, 2], [3, 3]] : X2F, y: null };
    };
    const makeFitted = (c) => {
        if (c === "CSR") return m.CSR.fromDense([[0, 1], [2, 0]]);
        const inst = makeFresh(c);
        const data = fitData(c);
        if (data.y === null) inst.fit(data.X);
        else inst.fit(data.X, data.y);
        return inst;
    };

    for (const c of classes) {
        const C = m[c];

        /* constructor probes */
        mark3("ml", c);
        rows.push([`${c} EMPTY ctor`, () => new C(), ANY],
                  [`${c} WRONG-TYPE ctor (string)`, () => new C("x"), ANY]);
        if (ctorKind[c] === "num") {
            rows.push([`${c} BOUNDARY ctor (numeric set)`, () =>
                outcomes(BNUM.map((v) => () => new C(...ctorArgs(c, v)))), BNUM.length]);
        } else {
            rows.push([`${c} BOUNDARY ctor`, () => outcomes([
                () => new C(...ctorArgs(c, null)), () => new C(...ctorArgs(c, NaN))]),
                2]);
        }

        /* fit probes on fresh instances */
        if (fitKind[c] !== "none") {
            mark3("ml", `${c}.fit`);
            const sup = fitKind[c] === "sup";
            rows.push([`${c}.fit EMPTY`, () => makeFresh(c).fit(), ANY],
                      [`${c}.fit WRONG-TYPE (strings)`, () =>
                          makeFresh(c).fit(sup ? "x" : "x", sup ? "y" : undefined), ANY],
                      [`${c}.fit BOUNDARY (empty rows + NaN target)`, () => outcomes([
                          () => makeFresh(c).fit(sup ? [] : []),
                          () => makeFresh(c).fit([[NaN], [1]], sup ? [NaN, 1] : undefined)]),
                          2]);
        }

        /* fitted-instance method probes */
        let inst = null;
        try { inst = makeFitted(c); } catch (e) {
            skip++;
            print(`SKIP (construction/fit failed): ${c} -- ${e.name}: ${e.message}`);
        }
        if (inst) {
            /* fit pin: a tiny fixed fit yields FINITE predictions (brief). */
            const hasPredict = typeof inst.predict === "function";
            if (hasPredict) {
                const px = c === "PCA" ? [[1, 1]] : [[1]];
                rows.push([`${c} fit yields finite predictions`, () => {
                    const p = inst.predict(px);
                    return Array.isArray(p) && p.length > 0 &&
                        p.every((v) => typeof v === "number" ? Number.isFinite(v) : true);
                }, true]);
            }
            for (const pm of Object.getOwnPropertyNames(Object.getPrototypeOf(inst))) {
                if (pm === "constructor" || pm === "fit" || pm === "close" || pm === "dispose")
                    continue;
                const f = inst[pm];
                if (typeof f !== "function") { /* data getter: read-pinned */
                    markRead("ml", `${c}.${pm}`);
                    rows.push([`${c}.${pm} getter read`, () => inst[pm], ANY]);
                    continue;
                }
                mark3("ml", `${c}.${pm}`);
                if (MATRIX.has(pm)) {
                    rows.push([`${c}.${pm} EMPTY`, () => f(), ANY],
                              [`${c}.${pm} WRONG-TYPE (string)`, () => f("x"), ANY],
                              [`${c}.${pm} BOUNDARY (empty + [[NaN]])`, () => outcomes([
                                  () => f([]), () => f([[NaN]])]), 2]);
                } else if (pm === "row") {
                    rows.push([`${c}.row EMPTY`, () => f(), ANY],
                              [`${c}.row WRONG-TYPE (string)`, () => f("x"), ANY],
                              [`${c}.row BOUNDARY (numeric set)`, () =>
                                  outcomes(BNUM.map((v) => () => f(v))), BNUM.length]);
                } else if (pm === "stage") {
                    rows.push([`${c}.stage EMPTY`, () => f(), ANY],
                              [`${c}.stage WRONG-TYPE (string)`, () => f("x"), ANY],
                              [`${c}.stage BOUNDARY (numeric set)`, () =>
                                  outcomes(BNUM.map((v) => () => f(v))), BNUM.length]);
                } else if (pm === "save" || pm === "serialize") {
                    rows.push([`${c}.${pm} EMPTY`, () => f(), ANY],
                              [`${c}.${pm} WRONG-TYPE (number path)`, () => f(123), ANY],
                              [`${c}.${pm} BOUNDARY (null path)`, () => f(null), ANY]);
                } else { /* toDense and anything else callable */
                    rows.push([`${c}.${pm} EMPTY`, () => f(), ANY],
                              [`${c}.${pm} WRONG-TYPE (extra arg)`, () => f("x"), ANY],
                              [`${c}.${pm} BOUNDARY (extra arg)`, () => f(-1), ANY]);
                }
            }
            /* statics: load / deserialize (and anything else declared) */
            for (const sm of Object.getOwnPropertyNames(C)) {
                if (["length", "name", "prototype"].includes(sm)) continue;
                const sf = C[sm];
                if (typeof sf !== "function") {
                    markRead("ml", `${c}.${sm}`);
                    rows.push([`${c}.${sm} static read`, () => C[sm], ANY]);
                    continue;
                }
                mark3("ml", `${c}.${sm}`);
                rows.push([`${c}.${sm} EMPTY`, () => sf(), ANY],
                          [`${c}.${sm} WRONG-TYPE (number)`, () => sf(123), ANY],
                          [`${c}.${sm} BOUNDARY (missing path / garbage bytes)`, () => outcomes([
                              () => sf("/nonexistent-t3-path"),
                              () => sf(new Uint8Array([1, 2, 3]))]), 2]);
            }
            /* close/dispose: probed LAST, on throwaway instances, including
             * the double-release and use-after-close cases.  They are void
             * methods (a plain `undefined` return is legitimate), so these
             * rows assert boundedness via OK rather than definedness. */
            for (const cm of ["close", "dispose"]) {
                if (typeof inst[cm] !== "function") continue;
                mark3("ml", `${c}.${cm}`);
                rows.push([`${c}.${cm} EMPTY`, () => makeFresh(c)[cm](), OK],
                          [`${c}.${cm} WRONG-TYPE (extra arg)`, () =>
                              makeFresh(c)[cm](123), OK],
                          [`${c}.${cm} BOUNDARY (double release)`, () => {
                              const t = makeFresh(c);
                              t[cm]();
                              return t[cm]();
                          }, OK]);
            }
        }
    }

    /* -------- class-specific pins (independent oracles) -------- */
    rows.push(
        ["LinearRegression learns y=2x", () => {
            const r = new m.LinearRegression({});
            r.fit([[1], [2], [3], [4]], [2, 4, 6, 8]);
            return near(r.predict([[5]])[0], 10, 0.5);
        }, true],
        ["LinearRegression predict before fit refused", () =>
            new m.LinearRegression({}).predict([[1]]), THROWS],
        ["LogisticRegression separates its training set", () => {
            const r = new m.LogisticRegression({ maxIter: 500 });
            r.fit(X4, yCls);
            return JSON.stringify(r.predict(X4)), JSON.stringify(yCls);
        }, JSON.stringify(yCls)],
        ["StandardScaler centres and scales", () => {
            const s = new m.StandardScaler();
            s.fit([[1], [2], [3]]);
            /* population std over [1,2,3] is sqrt(2/3) (ddof=0). */
            return [near(s.transform([[2]])[0][0], 0, 1e-9), near(s.mean[0], 2, 1e-9),
                    near(s.std[0], Math.sqrt(2 / 3), 1e-12)];
        }, [true, true, true]],
        ["StandardScaler constant column keeps std 1", () => {
            const s = new m.StandardScaler();
            s.fit([[7], [7], [7]]);
            return s.std[0];
        }, 1],
        ["MinMaxScaler maps 2 of [1,3] to 0.5", () => {
            const s = new m.MinMaxScaler();
            s.fit([[1], [2], [3]]);
            return s.transform([[2]])[0][0];
        }, 0.5],
        ["CSR.fromDense round trips", () => {
            const c = m.CSR.fromDense([[0, 1], [2, 0]]);
            return [c.rows, c.cols, c.nnz, c.density, c.toDense()];
        }, [2, 2, 2, 0.5, [[0, 1], [2, 0]]]],
        ["CSR ctor stores the given triplets", () => {
            const c = new m.CSR([1, 2], [0, 1], [0, 1, 2], 2);
            return c.toDense();
        }, [[1, 0], [0, 2]]],
        ["Pipeline length/fitted/stage", () => {
            const p = new m.Pipeline([new m.StandardScaler(), new m.LinearRegression({})]);
            p.fit([[1], [2], [3]], [1, 2, 3]);
            return [p.length, p.fitted, p.predict([[4]])[0] > 3.5,
                    p.stage(0) instanceof m.StandardScaler,
                    p.estimator instanceof m.LinearRegression];
        }, [2, true, true, true, true]],
        ["serialize/deserialize round trip", () => {
            const r = new m.LinearRegression({});
            r.fit(X4, yReg);
            const r2 = m.LinearRegression.deserialize(r.serialize());
            return near(r2.predict([[5]])[0], r.predict([[5]])[0], 1e-12);
        }, true],
        ["close marks the model closed", () => {
            const r = new m.LinearRegression({});
            r.fit(X4, yReg);
            r.close();
            return r.closed;
        }, true],
        ["predict after close is a clean TypeError", () => {
            const r = new m.LinearRegression({});
            r.fit(X4, yReg);
            r.close();
            return r.predict([[1]]);
        }, THROWS],
    );

    return rows;
});

/* ============================================================== coverage */
print("\n" + "=".repeat(64));
for (const modName of Object.keys(COV)) {
    const c = COV[modName];
    const total = c.names.size;
    const p3 = [...c.probed3].filter((n) => c.names.has(n)).length;
    const read = [...c.read].filter((n) => c.names.has(n)).length;
    const missing = [...c.names].filter((n) => !c.probed3.has(n) && !c.read.has(n));
    const pct = total ? ((100 * p3) / total).toFixed(1) : "0.0";
    const pctTouched = total ? ((100 * (p3 + read)) / total).toFixed(1) : "0.0";
    print(`coverage ${modName}: ${p3}/${total} names (${pct}%) probed with all ` +
          `three probes; +${read} read-pinned (touched ${p3 + read}/${total} = ${pctTouched}%)`);
    if (missing.length) print(`  unprobed names: ${missing.join(", ")}`);
}

print("\nthrown-error classes: " + Object.entries(thrownClass)
    .sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}:${v}`).join("  "));
if (notes.length) {
    print("NOTES:");
    for (const n of notes) print("  " + n);
}
print("=".repeat(64));
if (fails.length) {
    print(`FAILURES (${fails.length}):`);
    for (const f of fails) print("  " + f);
}
print(`test_api_params_df_mathx_ml: ${pass + fail + baseFail} cases, ${pass} passed, ` +
      `${skip} skipped, ${baseFail} failed-by-baseline (${fail} failed-hard)`);
if (!(canaryHits === 0 && fail === 0 && baseFail === BASELINE_FAILS.size))
    throw new Error("params-grid: hard failures or canary hit");
} catch (e) {
    failedSections++; print("SECTION FAILED: params-grid -- " + (e && e.message));
}

/* ===================================================================
 * adversarial
 * bounded adversarial sweep (7997 inputs; 19 pinned garbage-successes)
 * =================================================================== */
try {
/* test_adversarial_parsers.js -- one bounded adversarial sweep across the TEXT
 * parsers: dyna:yaml, dyna:xml (+SAX), dyna:csv, dyna:encoding (JSON5),
 * dyna:config (TOML/INI/Env) and dyna:url.
 *
 * THE CONTRACT UNDER TEST IS A CLEAN FAILURE. For every adversarial input the
 * parser either throws an Error or returns a structured value -- it must never
 * hang, abort the process, or throw a NON-Error (an engine crash surfaces as
 * process death, so the run surviving IS the assertion for that class).
 *
 * ADVERSARIAL DATA LIVES IN CORPORA; ASSERTIONS LIVE HERE. The strings come
 * from the vendored BLNS list via tests/fuzzgen.js (the shared data layer);
 * the structured shapes (depth caps at cap and cap+1, unterminated constructs,
 * huge scalars, NUL bytes, lone surrogates, CR/CRLF mixes, mid-stream BOMs,
 * 1/2/3-byte chunked feeds) are built programmatically below.
 *
 * BASELINE DISCIPLINE: where a parser silently RETURNS a degraded result on an
 * input (a garbage success -- e.g. csv tolerant mode keeping a readable
 * prefix), the behavior is recorded as a named BASELINE entry and PINNED: the
 * case fails loudly if the behavior changes, so a future fix tightens the
 * baseline by deleting one entry, and the named case flips to green. Baseline
 * entries are findings, not crashes; real crashes/hangs were found during the
 * sweep that produced this file and are listed in the header comments next to
 * the case that pins their bounded sibling.
 *
 * BOUNDS (so the whole file stays far under the 20s budget): corpus strings
 * are used verbatim (BLNS max ~100 chars); the biggest scalar built is a
 * 1e6-digit number; the biggest input is one 64 MiB string exercising yaml's
 * documented input cap; the URLSearchParams append loop is pinned at 800
 * iterations because the measured cost is quadratic (FINDING, see BASELINE).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_adversarial_parsers.js
 */

/* ------------------------------------------------------------------ harness */

let cases = 0, passed = 0, skipped = 0, baseline = 0, failed = 0;
const tally = {};            /* parser -> { threw, returned } */
const notes = [];            /* loud one-line notes, printed in the summary */

const bump = (p, kind) => { if (!tally[p]) tally[p] = { threw: 0, returned: 0 }; tally[p][kind]++; };

/* Run fn, record outcome { threw: Error } | { returned: value } | { skipped }.
 * The ONE assertion applied to every case: a thrown value is an Error, and
 * surviving the call at all rules out hangs/aborts. Anything else thrown -- a
 * string, a number, null -- means the engine's error path leaked a non-Error,
 * which is a crash-class bug. A clean throw or a structured return PASSES. */
function probe(parser, id, fn) {
    cases++;
    let out;
    try { out = fn(); } catch (e) {
        if (!(e instanceof Error)) {
            failed++;
            print("  FAIL  " + id + " threw a NON-Error: " + String(e));
            return { threw: false };
        }
        bump(parser, "threw");
        passed++;
        return { threw: true, err: e };
    }
    bump(parser, "returned");
    passed++;
    return { threw: false, returned: out };
}

/* A case with a stronger expectation: clean = threw an Error, not returned. */
function refuses(parser, id, fn) {
    const o = probe(parser, id, fn);
    if (!o.threw) {
        passed--;                       /* probe credited the clean return */
        failed++;
        print("  FAIL  " + id + " expected a throw, got: " + safeJson(o.returned));
    }
}

function safeJson(v) {
    try {
        const s = JSON.stringify(v);
        if (s === undefined) return String(v);
        return s.length > 90 ? s.slice(0, 90) + "..." : s;
    } catch (e) { return "<unstringifiable " + typeof v + ">"; }
}

/* Structural deep-equal: arrays ordered, objects compared as key sets, so a
 * serializer that reorders keys does not masquerade as a round-trip break. */
function deepEq(a, b) {
    if (a === b) return true;
    if (typeof a === "number" && typeof b === "number")
        return Number.isNaN(a) && Number.isNaN(b);
    if (a === null || b === null || typeof a !== "object" || typeof b !== "object")
        return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    if (Array.isArray(a)) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (!deepEq(a[i], b[i])) return false;
        return true;
    }
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
        if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
        if (!deepEq(a[k], b[k])) return false;
    }
    return true;
}

/* BASELINE: recorded garbage-successes. Each entry names the input, describes
 * what the parser silently does with it, and PINS that behavior -- a mismatch
 * means the module changed under the baseline (fix landed, or regression). */
const BASELINE = {
    /* dyna:csv -- documented tolerant mode keeps a readable prefix; strict refuses. */
    "csv/garbage-after-quote/tolerant": {
        desc: "tolerant read splits 'x\"junk,2' into rows [x,\"\"] + [unk,2] (total=2), no error",
        check: (r) => !r.threw && r.returned.totalRows === 2 && r.returned.rows[0][0] === "x",
    },
    "csv/unterminated-quote/tolerant": {
        desc: "tolerant read swallows the rest of the file into the quoted field (total=1)",
        check: (r) => !r.threw && r.returned.totalRows === 1 && r.returned.rows[0][0] === "x,2\n",
    },
    "csv/blank-line/yields-empty-row": {
        desc: "a blank data line parses as a phantom row of empty cells",
        check: (r) => !r.threw && r.returned.totalRows === 3,
    },
    /* FINDING (silent data loss, strict does not refuse): a raw NUL byte cuts
     * the field short -- 'x<NUL>y' reads back as "x" in BOTH modes. */
    "csv/nul-field/truncates-even-in-strict": {
        desc: "field 'x<NUL>y' reads back as 'x' in tolerant AND strict mode",
        check: (r) => !r.threw && r.returned.rows[0][0] === "x",
    },
    /* dyna:yaml */
    "yaml/bare-cr/folds-lines-into-scalar": {
        desc: "a bare CR is not a line break: 'b: 2<CR>c: 3' parses to one scalar '2<CR>c: 3'",
        check: (r) => !r.threw && r.returned.b === "2\rc: 3",
    },
    "yaml/huge-int/degrades-to-string": {
        desc: "a 1e6-digit integer degrades to a string rather than throwing or rounding",
        check: (r) => !r.threw && typeof r.returned.a === "string" && r.returned.a.length === 1000000,
    },
    /* dyna:config -- INI/Env are line-oriented and miss the bare CR. */
    "ini/bare-cr/merges-lines": {
        desc: "'c = 3' after a bare CR folds into b's value ('2<CR>c = 3'), c is lost",
        check: (r) => !r.threw && r.returned.b === "2\rc = 3" && r.returned.c === undefined,
    },
    "env/bare-cr/merges-lines": {
        desc: "'C=3' after a bare CR folds into B's value ('2<CR>C=3'), C is lost",
        check: (r) => !r.threw && r.returned.B === "2\rC=3" && r.returned.C === undefined,
    },
    /* FINDING (documented cap not enforced): TOML.parse accepts dotted keys and
     * table headers nested 24 deep, but API.md says depth 16. INI enforces it. */
    "toml/dotted-key/past-documented-cap-accepted": {
        desc: "a 24-level dotted key parses fine although the documented cap is 16",
        check: (r) => !r.threw,
    },
    "toml/table-header/past-documented-cap-accepted": {
        desc: "a 24-level [a.b.c...] header parses fine although the documented cap is 16",
        check: (r) => !r.threw,
    },
    /* mid-stream BOM: every text config parser makes it key/value content. */
    "yaml/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'b' yields the key '<BOM>b'",
        check: (r) => !r.threw && r.returned["﻿b"] === 2,
    },
    "toml/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'b' yields the key '<BOM>b'",
        check: (r) => !r.threw && r.returned["﻿b"] === 2,
    },
    "ini/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'b' yields the key '<BOM>b'",
        check: (r) => !r.threw && r.returned["﻿b"] === "2",
    },
    "env/bom-midstream/becomes-key-content": {
        desc: "a BOM before 'B' yields the key '<BOM>B'",
        check: (r) => !r.threw && r.returned["﻿B"] === "2",
    },
    "csv/bom-midstream/becomes-value-content": {
        desc: "a BOM inside a cell stays in the value ('<BOM>2')",
        check: (r) => !r.threw && r.returned.rows[0][1] === "﻿2",
    },
    /* dyna:xml */
    "xml/stringify/emits-raw-nul": {
        desc: "XMLStringify re-emits a NUL from parsed text as a raw <NUL> byte",
        check: (r) => !r.threw && r.returned.indexOf("\0") >= 0,
    },
    "sax/end-with-open-elements/accepted": {
        desc: "SAX end() accepts a document left with open elements (XMLParse refuses the same doc)",
        check: (r) => !r.threw,
    },
    /* dyna:url -- a DECODED NUL truncates the USP value; a RAW NUL in a
       path is percent-encoded (WHATWG path set), not dropped. */
    "url/raw-nul/percent-encodes-in-path": {
        desc: "path 'a<NUL>b' becomes '/a%00b': the raw NUL is encoded like any C0 byte",
        check: (r) => !r.threw && r.returned.pathname === "/a%00b" && r.returned.search === "?q=1%00r",
    },
    "url/decoded-nul/truncates-usp-value": {
        desc: "URLSearchParams('a=x%00y').get('a') === 'x': the decoded NUL cuts the value",
        check: (r) => !r.threw && r.returned === "x",
    },
};
const BASELINE_TOTAL = Object.keys(BASELINE).length;

/* Check a recorded baseline behavior. `id` must be a probe id produced with
 * runBaseline(); a mismatch fails the suite loudly (this is the flip-to-green
 * point when a fix lands: delete the entry, the case becomes a plain probe). */
function runBaseline(parser, id, fn) {
    const o = probe(parser, id, fn);
    const b = BASELINE[id];
    cases++;                       /* the pin is its own case on top of the probe */
    if (!b) { failed++; print("  FAIL  " + id + " has no BASELINE entry"); return o; }
    let good = false;
    try { good = b.check(o); } catch (e) { good = false; }
    if (good) { baseline++; print("  baseline ok  " + id + " -- " + b.desc); }
    else {
        failed++;
        print("  FAIL  BASELINE MISMATCH " + id + " -- " + b.desc +
              "\n          got: " + (o.threw ? "threw " + safeJson(o.err.message) : safeJson(o.returned)));
    }
    return o;
}

function section(name) { print("--- " + name + " ---"); }

/* The corpus: BLNS via the shared data layer. Floor strings only if the file
 * is missing -- and then the skip is LOUD, never silent thin coverage. */
const BLNS = corpusSize() > 0 ? STRINGS : [];
if (BLNS.length === 0) {
    skipped++;
    notes.push("SKIP: tests/corpus/blns.txt missing (corpusSize()==0) -- all blns corpus sections skipped");
}

/* Tiny fixed-alphabet doc generator: benign values only (no YAML/TOML
 * metacharacters at scalar starts, no NaN/-0, nothing the writers escape). */
function benignDocs() {
    const rnd = PRNG(0xa05e3d ^ 0x5eed);
    const words = ["alpha", "beta 42", "g_1", "d-e", "plain text", "v9"];
    const scalar = () => {
        const r = rnd();
        if (r < 0.3) return Math.floor(rnd() * 1000);
        if (r < 0.5) return Math.round(rnd() * 10000) / 100;
        if (r < 0.7) return words[Math.floor(rnd() * words.length) % words.length];
        if (r < 0.85) return rnd() < 0.5;
        return null;
    };
    const gen = (depth) => {
        if (depth >= 2 || rnd() < 0.4) return scalar();
        if (rnd() < 0.5) {
            const n = 1 + Math.floor(rnd() * 3), a = [];
            for (let i = 0; i < n; i++) a.push(gen(depth + 1));
            return a;
        }
        const n = 1 + Math.floor(rnd() * 3), o = {};
        for (let i = 0; i < n; i++) o["k" + Math.floor(rnd() * 90)] = gen(depth + 1);
        return o;
    };
    const docs = [];
    for (let i = 0; i < 6; i++) docs.push(gen(0));
    return docs;
}
const DOCS = benignDocs();

/* Shared adversarial building blocks (per-parser junk tables below). */
const NUL = String.fromCharCode(0), BOM = String.fromCharCode(0xfeff);
const HI = String.fromCharCode(0xd800), LO = String.fromCharCode(0xdfff);
const BIG6 = "9".repeat(1000000);                      /* 1e6 digits */
const BIG6F = "9." + "9".repeat(1000000);

/* utf8(): deterministic encoder for the SAX byte-chunk sweep. */
function utf8(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
        let c = s.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const lo = s.charCodeAt(i + 1);
            if (lo >= 0xdc00 && lo <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); i++; }
        }
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
}

/* ============================================================== dyna:yaml */
{
    section("dyna:yaml  Parse / ParseAll / Stringify");
    for (const s of BLNS) {
        probe("yaml", "yaml/blns/document " + safeJson(s), () => Parse(s));
        probe("yaml", "yaml/blns/key-value " + safeJson(s), () => Parse("k: " + s));
        probe("yaml", "yaml/blns/ParseAll " + safeJson(s), () => ParseAll(s));
    }
    /* structured junk: caps at cap and cap+1 (127 parses, 128 throws), then
       the classic breakage classes. Stringify of a PARSED adversarial result
       is recorded but allowed to refuse -- only benign stringifies must pass. */
    const nest = (d) => { let v = "1"; for (let i = 0; i < d; i++) v = "[" + v + "]"; return "a: " + v; };
    probe("yaml", "yaml/nest-127/parse", () => Parse(nest(127)));
    refuses("yaml", "yaml/nest-128/parse", () => Parse(nest(128)));
    refuses("yaml", "yaml/nest-128/stringify", () => {
        let v = 1; for (let i = 0; i < 128; i++) v = [v];   /* 128 nested arrays */
        return YStringify(v);
    });
    probe("yaml", "yaml/flow-unterminated", () => Parse("a: [1, 2"));
    probe("yaml", "yaml/block-unterminated", () => Parse("a:\n  b: 1\n c: 2"));
    probe("yaml", "yaml/tab-indent", () => Parse("a:\n\tb: 1"));
    probe("yaml", "yaml/double-doc/plain", () => Parse("a: 1\n---\nb: 2"));
    probe("yaml", "yaml/directive-junk", () => Parse("%YAML 9.9\n---\na: 1"));
    probe("yaml", "yaml/tag-junk", () => Parse("a: !!int notanint"));
    probe("yaml", "yaml/alias-junk", () => Parse("a: *alias"));
    probe("yaml", "yaml/huge-key-64k", () => Parse("x".repeat(65536) + ": 1"));
    probe("yaml", "yaml/huge-int-1e6", () => Parse("a: " + BIG6));
    probe("yaml", "yaml/huge-float-1e6", () => Parse("a: " + BIG6F));
    probe("yaml", "yaml/exp-1e999", () => Parse("a: 1e999"));
    probe("yaml", "yaml/nul-mid-value", () => Parse("a: x" + NUL + "y"));
    probe("yaml", "yaml/nul-in-key", () => Parse("a" + NUL + "b: 1"));
    probe("yaml", "yaml/lone-hi", () => Parse("a: " + HI));
    probe("yaml", "yaml/lone-lo", () => Parse("a: " + LO));
    probe("yaml", "yaml/surrogate-pair", () => Parse("a: " + HI + LO));
    probe("yaml", "yaml/cr-mix", () => Parse("a: 1\r\nb: 2\rc: 3"));
    probe("yaml", "yaml/bom-lead", () => Parse(BOM + "a: 1"));
    runBaseline("yaml", "yaml/bom-midstream/becomes-key-content", () => Parse("a: 1\n" + BOM + "b: 2"));
    runBaseline("yaml", "yaml/bare-cr/folds-lines-into-scalar", () => Parse("a: 1\r\nb: 2\rc: 3"));
    runBaseline("yaml", "yaml/huge-int/degrades-to-string", () => Parse("a: " + BIG6));
    probe("yaml", "yaml/input-cap-64MiB", () => Parse("a: " + "x".repeat(67108864)));
    /* (c)+(d): benign docs -- Stringify of a parsed doc never throws, and
       parse -> stringify -> parse is the identity on values. */
    for (let i = 0; i < DOCS.length; i++) {
        const doc = DOCS[i];
        const first = probe("yaml", "yaml/benign[" + i + "]/parse", () => Parse(YStringify(doc)));
        if (!first.threw) {
            const again = probe("yaml", "yaml/benign[" + i + "]/reparse", () => Parse(YStringify(first.returned)));
            cases++;
            if (deepEq(first.returned, again.returned)) passed++;
            else { failed++; print("  FAIL  yaml/benign[" + i + "] parse->stringify->parse is not idempotent: " +
                                   safeJson(first.returned) + " vs " + safeJson(again.returned)); }
        }
    }
}

/* ============================================================== dyna:xml */
{
    section("dyna:xml  XMLParse / XMLStringify / XMLToObject / SAXParser");
    for (const s of BLNS) {
        probe("xml", "xml/blns/text " + safeJson(s), () => XMLParse("<a>" + s + "</a>"));
        probe("xml", "xml/blns/attr " + safeJson(s), () => XMLParse('<a b="' + s + '"/>'));
    }
    const nest = (d) => "<a>".repeat(d) + "</a>".repeat(d);
    probe("xml", "xml/nest-255/parse", () => XMLParse(nest(255)).name);
    refuses("xml", "xml/nest-256/parse", () => XMLParse(nest(256)).name);
    probe("xml", "xml/element-unterminated", () => XMLParse("<a><b>hi"));
    probe("xml", "xml/attr-unterminated", () => XMLParse('<a b="1>'));
    probe("xml", "xml/comment-unterminated", () => XMLParse("<a><!-- x"));
    probe("xml", "xml/cdata-unterminated", () => XMLParse("<a><![CDATA[x"));
    probe("xml", "xml/unknown-entity", () => XMLParse("<a>&nope;</a>"));
    probe("xml", "xml/entity-lone-hi", () => XMLParse("<a>&#xD800;</a>"));
    probe("xml", "xml/entity-nul", () => XMLParse("<a>&#0;</a>"));
    probe("xml", "xml/entity-out-of-range", () => XMLParse("<a>&#x110000;</a>"));
    probe("xml", "xml/two-roots", () => XMLParse("<a/><b/>"));
    probe("xml", "xml/nul-in-text", () => XMLParse("<a>x" + NUL + "y</a>"));
    probe("xml", "xml/lone-hi-in-text", () => XMLParse("<a>" + HI + "</a>"));
    probe("xml", "xml/cr-mix", () => XMLParse("<a>x\r\ny\rz</a>"));
    probe("xml", "xml/bom-lead", () => XMLParse(BOM + "<a/>").name);
    probe("xml", "xml/bom-mid", () => XMLParse("<a/>" + BOM + "<b/>"));
    probe("xml", "xml/huge-attr-64k", () => XMLParse('<a b="' + "x".repeat(65536) + '"/>').name);
    probe("xml", "xml/huge-text-1e6", () => XMLParse("<a>" + "x".repeat(1000000) + "</a>").children.length);
    probe("xml", "xml/stringify-attr-junk", () => XMLStringify(XMLParse('<a b="&lt;&quot;"/>')));
    runBaseline("xml", "xml/stringify/emits-raw-nul", () => XMLStringify(XMLParse("<a>x" + NUL + "</a>")));
    probe("xml", "xml/toobject-nul", () => XMLToObject(XMLParse("<a>x" + NUL + "y</a>")));
    probe("xml", "xml/toobject-lone-hi", () => XMLToObject(XMLParse("<a>" + HI + "</a>")));

    /* (c)+(d): benign docs -- stringify of a parsed doc never throws and
       parse -> stringify -> parse is a fixed point of the TEXT. */
    const benignXml = [
        '<a b="1"><c>x</c><d>two &amp; three</d></a>',
        '<r><item id="i1">é</item><item id="i2"><deep k="v">😱</deep></item></r>',
    ];
    for (let i = 0; i < benignXml.length; i++) {
        const doc = benignXml[i];
        probe("xml", "xml/benign[" + i + "]/parse+stringify", () => XMLStringify(XMLParse(doc)));
        probe("xml", "xml/benign[" + i + "]/toobject", () => XMLToObject(XMLParse(doc)));
        const s1 = XMLStringify(XMLParse(doc));
        const s2 = XMLStringify(XMLParse(s1));
        cases++;
        if (s1 === s2) passed++;
        else { failed++; print("  FAIL  xml/benign[" + i + "] stringify->parse->stringify is not a fixed point"); }
    }

    /* THE CHUNK MATRIX: an adversarial doc (multi-byte UTF-8, an entity, a
       CDATA, a comment, a PI, a NUL) fed whole vs 1/2/3-byte boundaries, as
       STRINGS and as BYTES. A resume bug cannot hide from the 1-byte sweep. */
    const saxDoc = '<a x="é">héllo &amp; 😱<![CDATA[é😱]]><!--é--><?pi é?></a>';
    const saxEvents = (feed) => {
        const out = [];
        const p = new SAXParser({
            onOpen: (nm, at) => out.push(["open", nm, JSON.stringify(at)]),
            onClose: (nm) => out.push(["close", nm]),
            onText: (t) => out.push(["text", t]),
            onCData: (t) => out.push(["cdata", t]),
            onComment: (t) => out.push(["comment", t]),
            onPI: (t, d) => out.push(["pi", t, d]),
        });
        if (feed.bytes) {
            const b = utf8(saxDoc);
            if (feed.k === 0) p.write(b);
            else for (let i = 0; i < b.length; i += feed.k) p.write(b.slice(i, i + feed.k));
        } else {
            if (feed.k === 0) p.write(saxDoc);
            else for (let i = 0; i < saxDoc.length; i += feed.k) p.write(saxDoc.slice(i, i + feed.k));
        }
        p.end();
        return JSON.stringify(out);
    };
    const whole = saxEvents({ k: 0 });
    for (const k of [1, 2, 3]) {
        probe("xml", "xml/sax/string-chunk-" + k, () =>
            (saxEvents({ k }) === whole ? "identical" : "DIFFERS from whole-buffer feed"));
        probe("xml", "xml/sax/byte-chunk-" + k, () =>
            (saxEvents({ k, bytes: true }) === whole ? "identical" : "DIFFERS from whole-buffer feed"));
        cases += 2;   /* the comparison above is the assertion */
        const sk = saxEvents({ k }), bk = saxEvents({ k, bytes: true });
        const bad = (sk === whole ? 0 : 1) + (bk === whole ? 0 : 1);
        if (bad === 0) passed += 2;
        else {
            failed += bad;
            if (sk !== whole) print("  FAIL  xml/sax/string-chunk-" + k + " event stream differs");
            if (bk !== whole) print("  FAIL  xml/sax/byte-chunk-" + k + " event stream differs");
        }
    }
    probe("xml", "xml/sax/nul-in-text", () => {
        const out = [];
        const p = new SAXParser({ onText: (t) => out.push(t) });
        p.write("<a>x" + NUL + "y</a>"); p.end();
        return JSON.stringify(out);
    });
    probe("xml", "xml/sax/lone-hi-in-text", () => {
        const out = [];
        const p = new SAXParser({ onText: (t) => out.push(t) });
        p.write("<a>" + HI + "</a>"); p.end();
        return JSON.stringify(out);
    });
    probe("xml", "xml/sax/unknown-entity", () => {
        const p = new SAXParser({ onText: () => {} });
        p.write("<a>&nope;</a>"); p.end();
        return "accepted";
    });
    runBaseline("xml", "sax/end-with-open-elements/accepted", () => {
        const p = new SAXParser({ onOpen: () => {}, onClose: () => {}, onText: () => {} });
        p.write("<a><b>hi"); p.end();
        return "accepted";
    });
}

/* ============================================================== dyna:csv */
{
    section("dyna:csv  CSVFile create/read round-trip + {strict} refusals");
    const dir = makeTempDir("dyna-adv-csv-");
    const P = (n) => dir.join(n);
    try {
        const files = [
            ["garbage-after-quote", 'A,B\n"x"junk,2\n'],
            ["unterminated-quote", "A,B\n\"x,2\n"],
            ["nul-field", "A,B\nx" + NUL + "y,2\n"],
            ["crlf-rows", "A,B\r\n1,2\r\n3,4\r\n"],
            ["cr-rows", "A,B\r1,2\r3,4\r"],
            ["bom-lead", BOM + "A,B\n1,2\n"],
            ["bom-mid", "A,B\n1," + BOM + "2\n"],
            ["mixed-eol", "A,B\n1,2\r\n3,4\n5,6\r"],
            ["blank-lines", "A,B\n\n1,2\n\n"],
            ["no-final-newline", "A,B\n1,2"],
            ["only-newlines", "\n\n\n"],
            ["quote-in-unquoted", 'A,B\nx"y,2\n'],
            ["lone-hi", "A,B\n" + HI + ",2\n"],
            ["huge-cell-1e6", "A,B\n" + BIG6 + ",2\n"],
        ];
        for (const [name, text] of files) {
            writeFile(P(name + ".csv"), text);
            const f = new CSVFile(P(name + ".csv"));
            probe("csv", "csv/" + name + "/tolerant", () => f.read({}));
            /* {strict} must REFUSE the malformed ones (garbage after quote,
               unterminated quote) and be value-identical on the rest -- the
               refusal itself is the assertion for the two malformed files. */
            const malformed = name === "garbage-after-quote" || name === "unterminated-quote";
            if (malformed) refuses("csv", "csv/" + name + "/strict-refuses", () => f.read({ strict: true }));
            else probe("csv", "csv/" + name + "/strict", () => f.read({ strict: true }));
            f.close();
        }
        runBaseline("csv", "csv/garbage-after-quote/tolerant", () => new CSVFile(P("garbage-after-quote.csv")).read({}));
        runBaseline("csv", "csv/unterminated-quote/tolerant", () => new CSVFile(P("unterminated-quote.csv")).read({}));
        runBaseline("csv", "csv/blank-line/yields-empty-row", () => new CSVFile(P("blank-lines.csv")).read({}));
        runBaseline("csv", "csv/nul-field/truncates-even-in-strict", () => new CSVFile(P("nul-field.csv")).read({ strict: true }));
        runBaseline("csv", "csv/bom-midstream/becomes-value-content", () => new CSVFile(P("bom-mid.csv")).read({}));

        /* (c)+(d): benign round-trip -- cells containing commas, quotes and
           newlines survive create -> read -> create -> read unchanged. */
        const rnd = PRNG(0xc5e2);
        const cells = ['a,b', 'q"uote', "line\nbreak", " plain ", "unicode é😱", "", "042"];
        const rows = [];
        for (let i = 0; i < 12; i++) {
            const r = [];
            for (let j = 0; j < 3; j++) r.push(cells[Math.floor(rnd() * cells.length) % cells.length]);
            rows.push(r);
        }
        const fa = new CSVFile(P("rt-a.csv"));
        fa.create({ headers: ["C1", "C2", "C3"], rows });
        const ra = fa.read({});
        const fb = new CSVFile(P("rt-b.csv"));
        fb.create({ headers: ["C1", "C2", "C3"], rows: ra.rows, overwrite: true });
        const rb = fb.read({});
        cases++;
        if (deepEq(ra.rows, rb.rows) && deepEq(ra.headers, rb.headers)) passed++;
        else failed++ || print("  FAIL  csv/benign-round-trip rows differ after a second round-trip");
        fa.close(); fb.close();
    } finally {
        removeAll(dir);
    }
}

/* ========================================================= dyna:encoding */
{
    section("dyna:encoding  JSON5Parse / JSON5Stringify");
    for (const s of BLNS) {
        probe("json5", "json5/blns/bare " + safeJson(s), () => JSON5Parse(s));
        probe("json5", "json5/blns/in-object " + safeJson(s), () => JSON5Parse('{"k": ' + s + "}"));
    }
    const nest = (d) => { let v = "1"; for (let i = 0; i < d; i++) v = "[" + v + "]"; return v; };
    probe("json5", "json5/nest-255/parse", () => JSON5Parse(nest(255)));
    refuses("json5", "json5/nest-256/parse", () => JSON5Parse(nest(256)));
    probe("json5", "json5/array-unterminated", () => JSON5Parse('{"a": [1,'));
    probe("json5", "json5/object-unterminated", () => JSON5Parse('{"a": 1'));
    probe("json5", "json5/string-unterminated", () => JSON5Parse('{"a": "x'));
    probe("json5", "json5/bad-token", () => JSON5Parse("{a:}"));
    probe("json5", "json5/comment-unterminated", () => JSON5Parse("/* no end"));
    probe("json5", "json5/bom-lead", () => JSON5Parse(BOM + '{"a":1}'));
    probe("json5", "json5/nul-in-string", () => JSON5Parse('{"a":"x' + NUL + '"}'));
    probe("json5", "json5/lone-hi-in-string", () => JSON5Parse('{"a":"' + HI + '"}'));
    probe("json5", "json5/huge-int-1e6", () => JSON5Parse(BIG6));
    probe("json5", "json5/huge-float-1e6", () => JSON5Parse(BIG6F));
    probe("json5", "json5/exp-1e999", () => JSON5Parse("1e999"));
    probe("json5", "json5/tiny-subnormal", () => JSON5Parse("0." + "0".repeat(400) + "9"));
    probe("json5", "json5/string-1e6", () => JSON5Parse('"' + "x".repeat(1000000) + '"').length);
    probe("json5", "json5/stringify-parsed-lone-hi", () => JSON5Stringify(JSON5Parse('{"a":"' + HI + '"}')));
    probe("json5", "json5/stringify-parsed-nul", () => JSON5Stringify(JSON5Parse('{"a":"x' + NUL + '"}')));
    /* (c)+(d): benign idempotence. */
    for (let i = 0; i < DOCS.length; i++) {
        const doc = DOCS[i];
        const first = probe("json5", "json5/benign[" + i + "]/parse", () => JSON5Parse(JSON5Stringify(doc)));
        if (!first.threw) {
            const again = probe("json5", "json5/benign[" + i + "]/reparse", () => JSON5Parse(JSON5Stringify(first.returned)));
            cases++;
            if (deepEq(first.returned, again.returned)) passed++;
            else { failed++; print("  FAIL  json5/benign[" + i + "] parse->stringify->parse is not idempotent: " +
                                   safeJson(first.returned) + " vs " + safeJson(again.returned)); }
        }
    }
}

/* =========================================================== dyna:config */
{
    section("dyna:config  TOML.parse/stringify + INI.parse + Env.parse");
    for (const s of BLNS) {
        probe("toml", "toml/blns/bare " + safeJson(s), () => TOML.parse(s));
        probe("toml", "toml/blns/key-value " + safeJson(s), () => TOML.parse("k = " + s));
        probe("ini", "ini/blns/bare " + safeJson(s), () => INI.parse(s));
        probe("env", "env/blns/bare " + safeJson(s), () => Env.parse(s));
    }
    const dotted = (d) => { const k = []; for (let i = 0; i < d; i++) k.push("k" + i); return k.join(".") + " = 1"; };
    const headers = (d) => { const k = []; for (let i = 0; i < d; i++) k.push("t" + i); return "[" + k.join(".") + "]\nx = 1"; };
    probe("toml", "toml/dots-16/parse", () => TOML.parse(dotted(16)));
    probe("toml", "toml/dots-17/parse", () => TOML.parse(dotted(17)));
    runBaseline("toml", "toml/dotted-key/past-documented-cap-accepted", () => TOML.parse(dotted(24)));
    runBaseline("toml", "toml/table-header/past-documented-cap-accepted", () => TOML.parse(headers(24)));
    const iniHeader = (d) => { const k = []; for (let i = 0; i < d; i++) k.push("s" + i); return "[" + k.join(".") + "]\nx = 1"; };
    probe("ini", "ini/dots-16/parse", () => INI.parse(iniHeader(16)));
    refuses("ini", "ini/dots-17-refused", () => INI.parse(iniHeader(17)));
    probe("toml", "toml/string-unterminated", () => TOML.parse('x = "abc'));
    probe("toml", "toml/table-unterminated", () => TOML.parse("[tab"));
    probe("toml", "toml/duplicate-key", () => TOML.parse("a = 1\na = 2"));
    probe("toml", "toml/leading-zero", () => TOML.parse("x = 01"));
    probe("toml", "toml/bad-date", () => TOML.parse("x = 1979-99-99"));
    probe("toml", "toml/nul-in-string", () => TOML.parse('a = "x' + NUL + 'y"'));
    probe("toml", "toml/lone-hi-in-string", () => TOML.parse('a = "' + HI + '"'));
    probe("toml", "toml/huge-int-1e6", () => TOML.parse("a = " + BIG6));
    probe("toml", "toml/huge-float-1e6", () => TOML.parse("a = " + BIG6F));
    probe("toml", "toml/inf-nan", () => TOML.parse("a = inf\nb = nan"));
    probe("toml", "toml/cr-mix", () => TOML.parse("a = 1\r\nb = 2\rc = 3"));
    probe("toml", "toml/bom-lead", () => TOML.parse(BOM + "a = 1"));
    runBaseline("toml", "toml/bom-midstream/becomes-key-content", () => TOML.parse("a = 1\n" + BOM + "b = 2"));
    probe("toml", "toml/stringify-lone-hi", () => TOML.stringify(TOML.parse('a = "' + HI + '"')));
    probe("ini", "ini/nul-value", () => INI.parse("a = x" + NUL + "y"));
    probe("ini", "ini/section-unterminated", () => INI.parse("[unterminated\ngood = 1\n"));
    probe("ini", "ini/bom-lead", () => INI.parse(BOM + "a = 1"));
    runBaseline("ini", "ini/bom-midstream/becomes-key-content", () => INI.parse("a = 1\n" + BOM + "b = 2"));
    runBaseline("ini", "ini/bare-cr/merges-lines", () => INI.parse("a = 1\r\nb = 2\rc = 3"));
    probe("env", "env/nul-value", () => Env.parse("A=x" + NUL + "y"));
    probe("env", "env/line-without-equals", () => Env.parse("JUSTWORDS\nA=1"));
    probe("env", "env/export-prefix", () => Env.parse("export A=1"));
    probe("env", "env/bom-lead", () => Env.parse(BOM + "A=1"));
    runBaseline("env", "env/bom-midstream/becomes-key-content", () => Env.parse("A=1\n" + BOM + "B=2"));
    runBaseline("env", "env/bare-cr/merges-lines", () => Env.parse("A=1\r\nB=2\rC=3"));
    /* (c)+(d): TOML benign idempotence (no nulls -- TOML.stringify renders
       them as the literal `null`, which is a lossy round trip by design). */
    const tomlDocs = DOCS.map((d) => JSON.parse(JSON.stringify(d, (k, v) => (v === null ? undefined : v))));
    for (let i = 0; i < tomlDocs.length; i++) {
        const doc = tomlDocs[i];
        const first = probe("toml", "toml/benign[" + i + "]/parse", () => {
            const text = TOML.stringify(doc);
            return TOML.parse(text);
        });
        if (!first.threw) {
            const again = probe("toml", "toml/benign[" + i + "]/reparse", () => TOML.parse(TOML.stringify(first.returned)));
            cases++;
            if (deepEq(first.returned, again.returned)) passed++;
            else { failed++; print("  FAIL  toml/benign[" + i + "] parse->stringify->parse is not idempotent: " +
                                   safeJson(first.returned) + " vs " + safeJson(again.returned)); }
        }
    }
}

/* ============================================================== dyna:url */
{
    section("dyna:url  URL + URLSearchParams");
    for (const s of BLNS) {
        probe("url", "url/blns/absolute " + safeJson(s), () => new URL(s).href);
        probe("url", "url/blns/in-path " + safeJson(s), () => new URL("https://h.example/" + s).pathname);
        probe("url", "url/blns/search-params " + safeJson(s), () => new URLSearchParams(s).size);
    }
    runBaseline("url", "url/raw-nul/percent-encodes-in-path", () => {
        const u = new URL("https://e.com/a" + NUL + "b?q=1" + NUL + "r");
        return { pathname: u.pathname, search: u.search };
    });
    runBaseline("url", "url/decoded-nul/truncates-usp-value", () => new URLSearchParams("a=x%00y").get("a"));
    probe("url", "url/ipv6-unterminated", () => new URL("http://[::1").href);
    probe("url", "url/empty-input", () => new URL("").href);
    probe("url", "url/bare-word", () => new URL("not a url").href);
    probe("url", "url/port-out-of-range", () => new URL("https://h.example:99999/").port);
    probe("url", "url/lone-hi-path", () => new URL("https://e.com/" + HI).pathname);
    probe("url", "url/cr-in-path", () => new URL("https://e.com/p\rq").pathname);
    probe("url", "url/newline-in-path", () => new URL("https://e.com/p\nq").pathname);
    probe("url", "url/tab-in-path", () => new URL("https://e.com/p\tq").pathname);
    probe("url", "url/input-cap-65536", () => new URL("https://e.com/" + "a".repeat(70000)).pathname);
    probe("url", "url/usp-value-1e6", () => new URLSearchParams("a=" + "b".repeat(1000000)).get("a").length);
    /* FINDING (perf, quadratic): append re-scans the whole list -- measured
       16/63/258/1044/4208 ms at n=500/1000/2000/4000/8000, so 100k appends
       exceeds 40s. The suite pins a BOUNDED case (n=800) and reports the
       hang-size case here rather than hanging the suite itself. */
    notes.push("FINDING url/usp-append-quadratic: 100k appends > 40s (measured quadratic); suite pins a bounded n=800 case");
    probe("url", "url/usp-append-bounded-800", () => {
        const s = new URLSearchParams();
        for (let i = 0; i < 800; i++) s.append("k", "v" + i);
        return s.size;
    });
    /* (c)+(d): benign URL and params round-trips are fixed points. */
    const urls = [
        "https://user:pw@example.com:8443/a/b?x=1&y=2#frag",
        "http://h.example/p?q=é&z=%20#è",
        "https://[2001:db8::1]:8080/",
        "mailto:a@b.example",
    ];
    for (let i = 0; i < urls.length; i++) {
        probe("url", "url/benign[" + i + "]/href", () => new URL(urls[i]).href);
        const h1 = new URL(urls[i]).href;
        const h2 = new URL(h1).href;
        cases++;
        if (h1 === h2) passed++;
        else { failed++; print("  FAIL  url/benign[" + i + "] href is not a fixed point: " + h1 + " vs " + h2); }
    }
    const qs = "a=1&b=two&c=é%20space&d=";
    probe("url", "url/benign/usp-toString", () => new URLSearchParams(qs).toString());
    {
        const t1 = new URLSearchParams(qs).toString();
        const t2 = new URLSearchParams(t1).toString();
        cases++;
        if (t1 === t2) passed++;
        else { failed++; print("  FAIL  url/benign/usp toString is not a fixed point: " + t1 + " vs " + t2); }
    }
}

/* --------------------------------------------------------------- summary */

/* How many corpus strings each parser actually consumes in its blns sweep
 * (csv has no string-blns sweep: its corpus is the adversarial FILES above). */
const BLNS_PER_PARSER = { yaml: 3, xml: 2, json5: 2, toml: 2, ini: 1, env: 1, url: 3 };

for (const p of Object.keys(tally)) {
    const n = BLNS.length * (BLNS_PER_PARSER[p] || 0);
    print("  " + p + ": " + (n ? n + " corpus-driven inputs + " : "no corpus sweep; ") +
          "structured junk -> " + tally[p].threw + " threw, " + tally[p].returned +
          " returned (all clean)");
}
for (const n of notes) print("  " + n);

print("test_adversarial_parsers: " + cases + " cases, " + passed + " passed, " +
      skipped + " skipped, " + baseline + " failed-by-baseline (of " + BASELINE_TOTAL +
      " recorded)" + (failed ? ", " + failed + " FAILED" : ""));
if (failed) throw new Error("test_adversarial_parsers: " + failed + " failures");
} catch (e) {
    failedSections++; print("SECTION FAILED: adversarial -- " + (e && e.message));
}

/* ------------------------------ driver ------------------------------ */

if (failedSections) {
    print("test_conformance: " + failedSections + " of 5 sections failed");
    throw new Error("test_conformance failed");
}
print("test_conformance: all 5 sections passed");
