// flags: --std
// Split from tests/test_conformance.js: the JSON Patch test suite.
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

try {
const BASELINE = [];

const FILES = ["tests.json", "spec_tests.json"];

function scriptDir() {
    const p = (typeof scriptArgs !== "undefined" && scriptArgs[0]) || "tests/x.js";
    const i = p.lastIndexOf("/");
    return i >= 0 ? p.slice(0, i) : ".";
}
const CORPUS = scriptDir() + "/../corpus/jsonpatch";

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

if (failedSections) {
    print("test_json_conformance_patch: the section failed");
    throw new Error("test_json_conformance_patch failed");
}
print("test_json_conformance_patch: passed");
