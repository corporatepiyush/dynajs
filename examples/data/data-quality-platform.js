// Data quality platform — profile a dataset, infer a contract, validate new batches, detect drift, publish a report.
//
// WHAT IT IS
//   The checks a data team runs before a new batch reaches the warehouse:
//
//   detect     the file's character encoding (a Latin-1 export from an old system is converted to UTF-8)
//   profile    per column: inferred type, null rate, distinct count, min/max, quantiles, histogram, outliers
//              (|z| > 4), most frequent values, string length range
//   contract   the profile of a trusted baseline becomes a JSON Schema (types, ranges widened by a margin,
//              enums for low-cardinality columns, required non-null columns) saved next to the data
//   validate   each new batch is checked row by row against the contract; failures are grouped by rule
//   drift      numeric columns compared to the baseline with the two-sample Kolmogorov-Smirnov statistic,
//              categorical columns with the population stability index (PSI) over native Multisets
//   replay     baseline ids go into a RoaringBitmap; a batch that re-sends rows the warehouse already has is
//              caught with one native intersection count instead of a JS Set probe per row
//   report     an HTML page (templated, escaped) and a machine-readable JSON verdict: pass / warn / fail
//
// MODULES  dyna:csv · dyna:encoding (DetectEncoding) · dyna:bytes (decode) · dyna:dataframe · dyna:schema
//          dyna:structures (Multiset, RoaringBitmap) · dyna:simd (i32Min/i32Max) · dyna:mathx · dyna:html (Template)
//          dyna:random · dyna:file · dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/data/data-quality-platform.js --baseline good.csv --batch new.csv --out report.html
//          dynajs examples/data/data-quality-platform.js       a baseline plus a clean batch and a broken one

import { parse as parseCSV, stringify as toCSV } from "dyna:csv";
import { DetectEncoding } from "dyna:encoding";
import { decode as decodeCharset, encode as encodeCharset } from "dyna:bytes";
import { DataFrame } from "dyna:dataframe";
import { Schema } from "dyna:schema";
import { Multiset, RoaringBitmap } from "dyna:structures";
import { i32Min, i32Max } from "dyna:simd";
import * as mathx from "dyna:mathx";
import { Template } from "dyna:html";
import { Random } from "dyna:random";
import { Path, makeTempDir, readBytes, writeFile, removeAll } from "dyna:file";
import { Command } from "dyna:cli";

const { options } = new Command("data-quality")
    .option("--baseline <csv>", "trusted baseline", { type: "string" })
    .option("--batch <csv>", "new batch to check", { type: "string", variadic: true })
    .option("--out <html>", "report path", { type: "string" })
    .parse(scriptArgs.slice(1));
const selfTest = !options.baseline;
const work = makeTempDir("dq");
const sample = selfTest ? generateFiles(work) : null;
const baselinePath = selfTest ? sample.baseline : new Path(options.baseline);
const batchPaths = selfTest ? sample.batches : (options.batch || []).map((p) => new Path(p));

// =============================================================================
// Load with encoding detection
// =============================================================================
function load(path) {
    const bytes = readBytes(path);
    const encoding = DetectEncoding(bytes, {
        fallback: "windows-1252",
        allowList: ["utf-8", "windows-1252", "utf-16le"],
    });
    const text = encoding === "utf-8" ? new TextDecoder().decode(bytes) : decodeCharset(bytes, encoding);
    const { headers, rows } = parseCSV(text, { strict: true });
    return { path, encoding, headers, rows };
}

// =============================================================================
// Profile
// =============================================================================
const isNull = (v) => v === "" || /^(null|na|n\/a|none)$/i.test(v);
function profile(data) {
    const cols = {};
    data.headers.forEach((name, c) => {
        const raw = data.rows.map((r) => r[c]);
        const present = raw.filter((v) => !isNull(v));
        const numeric = present.length > 0 && present.every((v) => /^-?\d+(\.\d+)?$/.test(v));
        const integer = numeric && present.every((v) => /^-?\d+$/.test(v));
        const p = {
            name,
            rows: raw.length,
            nulls: raw.length - present.length,
            nullRate: (raw.length - present.length) / raw.length,
        };
        if (numeric) {
            const values = Float64Array.from(raw, (v) => (isNull(v) ? NaN : Number(v)));
            const df = new DataFrame({ v: values });
            const ok = df.NOT_NA("v");
            const z = df.ZSCORE("v");
            const qs = df.QUANTILES("v", [0.01, 0.25, 0.5, 0.75, 0.99], ok);
            Object.assign(p, {
                type: integer ? "integer" : "number",
                min: df.MIN("v", ok),
                max: df.MAX("v", ok),
                mean: df.MEAN("v", ok),
                std: df.STDDEV("v", ok),
                p01: qs[0],
                q1: qs[1],
                median: qs[2],
                q3: qs[3],
                p99: qs[4],
                distinct: df.N_UNIQUE("v", ok),
                outliers: [...z].count((x) => Math.abs(x) > 4),
                histogram: [...df.HISTOGRAM("v", 10, ok).counts],
                values: [...values].filter((x) => !Number.isNaN(x)),
            });
        } else {
            const counts = new Multiset();
            for (const v of present) counts.add(v);
            const lens = Int32Array.from(present, (v) => v.length);
            Object.assign(p, {
                type: "string",
                distinct: counts.size,
                top: counts
                    .entrySet()
                    .sortWith([(a, b) => b[1] - a[1], (a, b) => a[0].compareBytes(b[0])])
                    .take(5),
                minLen: lens.length ? i32Min(lens) : 0,
                maxLen: lens.length ? i32Max(lens) : 0,
                frequencies: counts,
            });
        }
        cols[name] = p;
    });
    return cols;
}

// =============================================================================
// Contract inference and validation
// =============================================================================
function inferContract(prof) {
    const properties = {},
        required = [];
    for (const p of Object.values(prof)) {
        let s;
        if (p.type === "integer" && p.distinct === p.rows - p.nulls) {
            s = { type: "integer", minimum: Math.min(0, p.min) }; // a key column: unique, not range-bound
        } else if (p.type === "integer" || p.type === "number") {
            const span = p.max - p.min || Math.abs(p.max) || 1;
            s = { type: p.type, minimum: p.min - span * 0.1, maximum: p.max + span * 0.1 };
            if (p.min >= 0) s.minimum = Math.max(0, s.minimum);
        } else if (p.distinct <= 12) s = { enum: p.frequencies.elementSet().sort() };
        else s = { type: "string", minLength: Math.max(0, p.minLen - 2), maxLength: p.maxLen * 2 };
        if (p.nullRate === 0) required.push(p.name);
        else s = { anyOf: [s, { type: "null" }] };
        properties[p.name] = s;
    }
    return {
        $schema: "https://json-schema.org/draft/2020-12/schema",
        type: "object",
        required,
        properties,
        additionalProperties: false,
    };
}
function toRecord(headers, row, prof) {
    const o = {};
    headers.forEach((h, i) => {
        const v = row[i];
        o[h] = isNull(v)
            ? null
            : (prof[h]?.type === "integer" || prof[h]?.type === "number") && /^-?\d+(\.\d+)?$/.test(v)
              ? Number(v)
              : v;
    });
    return o;
}
function validate(data, contract, baseProfile) {
    const compiled = Schema.compile(contract);
    const failures = new Map();
    let bad = 0;
    if (data.headers.join() !== Object.keys(contract.properties).join())
        failures.set("columns", { count: 1, example: `got [${data.headers}]` });
    data.rows.forEach((row, i) => {
        const r = compiled.validate(toRecord(data.headers, row, baseProfile));
        if (r.valid) return;
        bad++;
        for (const e of r.errors) {
            const key = `${e.path || "/"} ${e.keyword}`;
            const f = failures.get(key) || { count: 0, example: `row ${i + 2}: ${e.message}` };
            f.count++;
            failures.set(key, f);
        }
    });
    return {
        badRows: bad,
        failures: [...failures].map(([rule, f]) => ({ rule, ...f })).sort((a, b) => b.count - a.count),
    };
}

// =============================================================================
// Drift: KS for numbers, PSI for categories
// =============================================================================
function ks(a, b) {
    const x = Float64Array.from(a).sort(),
        y = Float64Array.from(b).sort();
    let i = 0,
        j = 0,
        d = 0;
    while (i < x.length && j < y.length) {
        const v = Math.min(x[i], y[j]);
        while (i < x.length && x[i] <= v) i++;
        while (j < y.length && y[j] <= v) j++;
        d = Math.max(d, Math.abs(i / x.length - j / y.length));
    }
    const n = (x.length * y.length) / (x.length + y.length);
    const lambda = (Math.sqrt(n) + 0.12 + 0.11 / Math.sqrt(n)) * d; // asymptotic Kolmogorov p-value
    let p = 0;
    for (let k = 1; k <= 100; k++) p += 2 * (k % 2 ? 1 : -1) * Math.exp(-2 * k * k * lambda * lambda);
    return { d, p: mathx.isNaN(p) ? 1 : Math.min(1, Math.max(0, p)) };
}
function psi(base, cur) {
    const keys = base.elementSet().union(cur.elementSet());
    const nb = base.totalSize,
        nc = cur.totalSize;
    let s = 0;
    for (const k of keys) {
        const pb = Math.max(base.count(k) / nb, 1e-4),
            pc = Math.max(cur.count(k) / nc, 1e-4);
        s += (pc - pb) * Math.log(pc / pb);
    }
    return s;
}
function drift(baseProf, batchProf) {
    const out = [];
    for (const [name, b] of Object.entries(baseProf)) {
        const c = batchProf[name];
        if (!c || b.distinct === b.rows - b.nulls) continue; // a key column (every value unique) has no distribution to drift
        if (b.values && c.values) {
            const r = ks(b.values, c.values);
            out.push({ column: name, test: "KS", stat: r.d, p: r.p, drifted: r.p < 0.01 });
        } else if (b.frequencies && c.frequencies) {
            const v = psi(b.frequencies, c.frequencies);
            out.push({ column: name, test: "PSI", stat: v, drifted: v > 0.25 });
        }
    }
    return out;
}

// =============================================================================
// Run + report
// =============================================================================
const baseline = load(baselinePath);
const baseProfile = profile(baseline);
const idColumn = baseline.headers.indexOf("id");
const idsOf = (data) =>
    new RoaringBitmap(data.rows.map((r) => Number(r[idColumn])).filter((x) => Number.isInteger(x) && x >= 0));
const baselineIds = idColumn >= 0 ? idsOf(baseline) : new RoaringBitmap();
const contract = inferContract(baseProfile);
writeFile(work.join("contract.schema.json"), JSON.stringify(contract, null, 2));
const report = new Template(`<!doctype html><meta charset="utf-8"><title>Data quality: {{name}}</title>
<h1>{{name}} — <span class="{{verdict}}">{{verdict}}</span></h1>
<p>{{rows}} rows · encoding {{encoding}} · {{badRows}} rows violate the contract · {{replayed}} ids already in the baseline</p>
<h2>Contract failures</h2><ul>{{#failures}}<li><code>{{rule}}</code> ×{{count}} — {{example}}</li>{{/failures}}{{^failures}}<li>none</li>{{/failures}}</ul>
<h2>Drift</h2><table>{{#drift}}<tr><td>{{column}}</td><td>{{test}}</td><td>{{stat}}</td><td>{{#drifted}}DRIFT{{/drifted}}</td></tr>{{/drift}}</table>
<h2>Profile</h2><table>{{#columns}}<tr><td>{{name}}</td><td>{{type}}</td><td>{{nullPct}}% null</td><td>{{distinct}} distinct</td><td>{{range}}</td></tr>{{/columns}}</table>`);

const verdicts = [];
for (const path of batchPaths) {
    const batch = load(path);
    const prof = profile(batch);
    const v = validate(batch, contract, baseProfile);
    const d = drift(baseProfile, prof);
    const badShare = v.badRows / batch.rows.length;
    const replayed = idColumn >= 0 ? idsOf(batch).andCardinality(baselineIds) : 0;
    const verdict =
        badShare > 0.02 || v.failures.some((f) => f.rule === "columns")
            ? "fail"
            : badShare > 0 || replayed > 0 || d.some((x) => x.drifted)
              ? "warn"
              : "pass";
    const html = report.render({
        name: path.basename,
        verdict,
        rows: batch.rows.length.format(),
        encoding: batch.encoding,
        badRows: v.badRows,
        replayed,
        failures: v.failures.slice(0, 10),
        drift: d.map((x) => ({ ...x, stat: x.stat.toFixed(3) })),
        columns: Object.values(prof).map((p) => ({
            name: p.name,
            type: p.type,
            nullPct: (p.nullRate * 100).toFixed(1),
            distinct: p.distinct,
            range: p.type === "string" ? `len ${p.minLen}..${p.maxLen}` : `${p.min} .. ${p.max} (median ${p.median})`,
        })),
    });
    writeFile(
        options.out && batchPaths.length === 1
            ? new Path(options.out)
            : work.join(path.basenameWithout(".csv") + ".html"),
        html,
    );
    verdicts.push({
        file: path.basename,
        verdict,
        encoding: batch.encoding,
        badRows: v.badRows,
        replayed,
        failures: v.failures,
        drift: d.filter((x) => x.drifted).map((x) => x.column),
    });
    print(
        `${path.basename}: ${verdict.toUpperCase()} — ${batch.rows.length.format()} rows (${batch.encoding}), ${v.badRows} violate the contract, ${replayed} replayed ids, drift in [${verdicts.at(-1).drift}]`,
    );
    for (const f of v.failures.slice(0, 4)) print(`    ${f.rule} ×${f.count}  e.g. ${f.example}`);
}
print(
    `baseline ${baseline.rows.length.format()} rows (${baselineIds.size.format()} ids in ${baselineIds.bytes.bytes()} of bitmap); contract has ${contract.required.length} required columns, ${Object.values(contract.properties).filter((s) => s.enum || s.anyOf?.[0]?.enum).length} enums`,
);

if (selfTest) {
    const results = {};
    const [clean, legacy, broken] = verdicts;
    results[`a batch from the same process passes (${JSON.stringify(clean)})`] = clean.verdict === "pass";
    results[`a Latin-1 export is decoded and accepted (${legacy.encoding}, ${legacy.verdict})`] =
        legacy.encoding === "windows-1252" && legacy.verdict !== "fail";
    results["the broken batch fails on its bad columns"] =
        broken.verdict === "fail" &&
        broken.failures.some((f) => /country/.test(f.rule)) &&
        broken.failures.some((f) => /amount/.test(f.rule));
    results["the shifted amount distribution is flagged as drift"] = broken.drift.includes("amount");
    results[`replayed baseline rows are counted (${verdicts.map((x) => x.replayed)})`] =
        clean.replayed === 0 && legacy.replayed === 0 && broken.replayed === 15;
    results[`baseline outliers found (${baseProfile.amount.outliers})`] = baseProfile.amount.outliers === 3;
    removeAll(work);
    check(results);
}

// ---- payment exports: a baseline, a clean batch, a Latin-1 legacy batch, a broken batch ----
function generateFiles(dir) {
    const rng = new Random(99);
    const countries = ["DE", "FR", "ES", "IT", "NL"],
        methods = ["card", "sepa", "paypal"],
        cities = ["Köln", "Zürich", "São Paulo", "Malmö", "Kraków"];
    const make = (n, opts = {}) => {
        const rows = [["id", "amount", "currency", "country", "method", "city", "items"]];
        const idBase = opts.idBase ?? 10000;
        for (let i = 0; i < n; i++) {
            let amount = Math.max(1, rng.normal(opts.shift ? 120 : 60, 18));
            if (opts.outliers && i < 3) amount = 5000 + i;
            rows.push([
                String(opts.replay && i < opts.replay ? 10000 + i * 7 : idBase + i),
                amount.toFixed(2),
                "EUR",
                opts.badCountry && i % 20 === 0 ? "XX" : rng.choice(countries),
                rng.choice(methods),
                rng.choice(cities),
                String(1 + rng.poisson(2)),
            ]);
        }
        if (opts.negative) rows[5][1] = "-40.00";
        return toCSV(rows);
    };
    const baseline = dir.join("payments-baseline.csv"),
        clean = dir.join("payments-2026-03-04.csv"),
        legacy = dir.join("payments-legacy.csv"),
        broken = dir.join("payments-2026-03-05.csv");
    writeFile(baseline, make(4000, { outliers: true }));
    writeFile(clean, make(800, { idBase: 20000 }));
    writeFile(legacy, encodeCharset(make(500, { idBase: 30000 }), "windows-1252"));
    writeFile(broken, make(600, { idBase: 40000, replay: 15, shift: true, badCountry: true, negative: true }));
    return { baseline, batches: [clean, legacy, broken] };
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
