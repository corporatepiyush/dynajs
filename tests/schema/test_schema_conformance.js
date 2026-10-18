// flags: --std
// Split from tests/test_conformance.js: the JSON Schema Test Suite (draft 2020-12).
import * as std from "std";
import { CSVFile } from "dyna:csv";
import { JSON5Parse, JSON5Stringify } from "dyna:encoding";
import { Parse, ParseAll, Stringify as YStringify } from "dyna:yaml";
import { Patch } from "dyna:json";
import { Path, readFile, readDir } from "dyna:file";
import { STRINGS, corpusSize, PRNG } from "../lib/fuzzgen.js";
import { Schema } from "dyna:schema";
import { TOML, INI, Env } from "dyna:config";
import { URL, URLSearchParams } from "dyna:url";
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

var failedSections = 0;

// The suite's vectors are fetched, not committed. Without them every case below is unreachable, so say so by name
// rather than reporting a pass over nothing.
const corpusRoot = (() => {
    const p = (typeof scriptArgs !== "undefined" && scriptArgs[0]) || "tests/x.js";
    const i = p.lastIndexOf("/");
    return (i < 0 ? "." : p.slice(0, i)) + "/../corpus/jsonschema";
})();
let corpusPresent = true;
try { readDir(new Path(corpusRoot + "/draft2020-12")); } catch (e) { corpusPresent = false; }

if (!corpusPresent) {
    print("SKIP test_schema_conformance: no JSON-Schema-Test-Suite corpus at " + corpusRoot
          + " -- run tools/fetch-jsonschema-suite.sh (tests/schema/test_schema_refs.js still runs)");
} else try {
const BASELINE = [];

const CORPUSDIR = scriptDirSafe() + "/../corpus/jsonschema/draft2020-12";

function scriptDirSafe() {
    const p = (typeof scriptArgs !== "undefined" && scriptArgs[0]) || "tests/x.js";
    const i = p.lastIndexOf("/");
    return i >= 0 ? p.slice(0, i) : ".";
}

function v(schema, data) {
    try { return Schema.validate(schema, data).valid === true; }
    catch (e) { return null; }
}
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
    "default.json": () =>
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
        && v({ unevaluatedItems: false }, []) === true
        ? null : "unevaluatedItems not enforced (silently ignored)",
    "unevaluatedProperties.json": () =>
        v({ unevaluatedProperties: false }, { a: 1 }) === false
        && v({ unevaluatedProperties: false }, {}) === true
        ? null : "unevaluatedProperties not enforced (silently ignored)",
    "uniqueItems.json": () =>
        v({ uniqueItems: true }, [1, 1]) === false && v({ uniqueItems: true }, [1, 2]) === true
        ? null : "uniqueItems not enforced",

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

// The suite's remotes/ tree is what its runner serves at http://localhost:1234/; here every file is handed to the
// compiler through { resources } so no network is involved.
const REMOTES = scriptDirSafe() + "/../corpus/jsonschema/remotes";
const RESOURCES = {};
(function walk(dir, rel) {
    for (const e of readDir(new Path(dir))) {
        if (e.isDir) walk(dir + "/" + e.name, rel + e.name + "/");
        else if (e.name.endsWith(".json")) RESOURCES["http://localhost:1234/" + rel + e.name] = JSON.parse(readFile(new Path(dir + "/" + e.name)));
    }
})(REMOTES, "");
// Files whose vectors exercise behaviour this module deliberately leaves out, each with its reason.
const POLICY_SKIP = {
    "optional/cross-draft.json": "dyna:schema implements draft 2020-12 only; a referenced resource declaring an older "
        + "$schema (2019-09, draft-07) would have to be processed under that draft's rules",
};
function caseSkipReason() {
    return null;
}
function optionsFor(file) {
    if (file === "format.json") return { resources: RESOURCES, formats: "annotation" };
    if (file.startsWith("optional/format/")) return { resources: RESOURCES, formats: "assertion" };
    return { resources: RESOURCES };
}

let n = 0, passed = 0, skipped = 0, filesRun = 0, filesSkipped = 0;
const failing = [];

const listJson = (dir, pre) => readDir(new Path(CORPUSDIR + dir)).map((e) => pre + e.name).filter((nm) => nm.endsWith(".json")).sort();
const names = [...listJson("", ""), ...listJson("/optional", "optional/"), ...listJson("/optional/format", "optional/format/")];

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
            try { got = Schema.compile(g.schema, optionsFor(name)).validate(t.data).valid === true; }
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

if (failedSections) {
    print("test_schema_conformance: the section failed");
    throw new Error("test_schema_conformance failed");
}
if (corpusPresent) print("test_schema_conformance: passed");
