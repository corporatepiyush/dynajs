// flags: --std
// Split from tests/test_conformance.js: the WPT urltestdata corpus.
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

const CORPUS_PATH = "tests/corpus/wpt/urltestdata.json";

const FIELDS = ["href", "origin", "protocol", "username", "password",
                "host", "hostname", "port", "pathname", "search", "hash"];

const raw = std.loadFile(CORPUS_PATH);
if (raw === null || raw === undefined)
    throw new Error("cannot load corpus " + CORPUS_PATH + " (run from the repo root)");
const entries = JSON.parse(raw);

let cases = 0, passed = 0, skippedComments = 0;
const failures = [];
let originChecks = 0, originMismatches = 0;
const originNullDivergences = [];

function caseId(i) { return "case#" + i; }

function echo(s, n) {
    s = String(s);
    if (s.length > n) s = s.slice(0, n) + "...";
    let out = "";
    for (const ch of s) {
        const c = ch.codePointAt(0);
        out += (c >= 0x20 && c < 0x7f) ? ch : (c > 0xffff ? "\\u" + c.toString(16) : "\\u" + ("0000" + c.toString(16)).slice(-4));
    }
    return out;
}

for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    if (typeof e === "string") { skippedComments++; continue; }

    cases++;
    const id = caseId(i);
    const why = [];

    if (e.failure) {
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

if (failedSections) {
    print("test_url_conformance_wpt: the section failed");
    throw new Error("test_url_conformance_wpt failed");
}
print("test_url_conformance_wpt: passed");
