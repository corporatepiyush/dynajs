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
//   depends    functional dependencies between text columns are discovered in the baseline (city -> country:
//              every city lies in one country) and enforced on each batch; constant and key columns are skipped,
//              since everything trivially determines a constant and a key determines everything
//   validate   each new batch is checked row by row against the contract; failures are grouped by rule
//   duplicates ids repeated inside one batch (an exporter that retried a page) from bitmap cardinality
//   drift      numeric columns compared to the baseline with the two-sample Kolmogorov-Smirnov statistic,
//              categorical columns with the population stability index (PSI) over native Multisets
//   replay     baseline ids go into a RoaringBitmap; a batch that re-sends rows the warehouse already has is
//              caught with one native intersection count instead of a JS Set probe per row
//   report     an HTML page (templated, escaped) and a machine-readable JSON verdict: pass / warn / fail
//
// MODULES  dyna:mathx (stats.ks2Samp, stats.psiCounts) · dyna:csv · dyna:encoding (DetectEncoding) · dyna:bytes (decode) · dyna:dataframe · dyna:schema
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
// X -> Y holds when no X value appears with two Y values; mapping[x] is the one Y it determines
function discoverDependencies(data, prof) {
    const text = data.headers.filter((h) => {
        const p = prof[h];
        return p.type === "string" && p.distinct > 1 && p.distinct < p.rows - p.nulls;
    });
    const out = [];
    for (const x of text)
        for (const y of text) {
            if (x === y) continue;
            const xi = data.headers.indexOf(x),
                yi = data.headers.indexOf(y),
                mapping = {};
            let holds = true;
            for (const r of data.rows) {
                if (isNull(r[xi]) || isNull(r[yi])) continue;
                if ((mapping[r[xi]] ??= r[yi]) !== r[yi]) {
                    holds = false;
                    break;
                }
            }
            // a dependency needs evidence: more rows than distinct X values several times over
            if (holds && prof[x].distinct * 5 < data.rows.length) out.push({ x, y, xi, yi, mapping });
        }
    return out;
}
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
function validate(data, contract, baseProfile, deps) {
    const compiled = Schema.compile(contract);
    const failures = new Map();
    let bad = 0;
    if (data.headers.join() !== Object.keys(contract.properties).join())
        failures.set("columns", { count: 1, example: `got [${data.headers}]` });
    let fdRows = 0;
    data.rows.forEach((row, i) => {
        const r = compiled.validate(toRecord(data.headers, row, baseProfile));
        const errors = r.valid ? [] : [...r.errors];
        for (const d of deps) {
            const x = row[data.headers.indexOf(d.x)],
                y = row[data.headers.indexOf(d.y)],
                want = d.mapping[x];
            if (want !== undefined && !isNull(y) && y !== want)
                errors.push({
                    path: `/${d.x} -> /${d.y}`,
                    keyword: "dependency",
                    message: `${x} is in ${want}, not ${y}`,
                });
        }
        if (errors.some((e) => e.keyword === "dependency")) fdRows++;
        if (!errors.length) return;
        bad++;
        for (const e of errors) {
            const key = `${e.path || "/"} ${e.keyword}`;
            const f = failures.get(key) || { count: 0, example: `row ${i + 2}: ${e.message}` };
            f.count++;
            failures.set(key, f);
        }
    });
    return {
        badRows: bad,
        fdRows,
        failures: [...failures].map(([rule, f]) => ({ rule, ...f })).sort((a, b) => b.count - a.count),
    };
}

// =============================================================================
// Drift: KS for numbers, PSI for categories
// =============================================================================
// two-sample Kolmogorov-Smirnov (exact p up to 10,000 values per side) and the population stability index are native
function ks(a, b) {
    const r = mathx.stats.ks2Samp(a, b);
    return { d: r.statistic, p: r.pvalue };
}
function psi(base, cur) {
    const keys = [...base.elementSet().union(cur.elementSet())];
    return mathx.stats.psiCounts(
        keys.map((k) => base.count(k)),
        keys.map((k) => cur.count(k)),
    );
}
// the PSI definition, kept as an independent cross-check of psiCounts
function psiDefinition(base, cur) {
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
const deps = discoverDependencies(baseline, baseProfile);
writeFile(work.join("contract.schema.json"), JSON.stringify(contract, null, 2));
const report = new Template(`<!doctype html><meta charset="utf-8"><title>Data quality: {{name}}</title>
<h1>{{name}} — <span class="{{verdict}}">{{verdict}}</span></h1>
<p>{{rows}} rows · encoding {{encoding}} · {{badRows}} rows violate the contract · {{replayed}} ids already in the baseline</p>
<h2>Contract failures</h2><ul>{{#failures}}<li><code>{{rule}}</code> ×{{count}} — {{example}}</li>{{/failures}}{{^failures}}<li>none</li>{{/failures}}</ul>
<h2>Drift</h2><table>{{#drift}}<tr><td>{{column}}</td><td>{{test}}</td><td>{{stat}}</td><td>{{#drifted}}DRIFT{{/drifted}}</td></tr>{{/drift}}</table>
<h2>Profile</h2><table>{{#columns}}<tr><td>{{name}}</td><td>{{type}}</td><td>{{nullPct}}% null</td><td>{{distinct}} distinct</td><td>{{range}}</td></tr>{{/columns}}</table>`);

const verdicts = [],
    psiChecks = [];
for (const path of batchPaths) {
    const batch = load(path);
    const prof = profile(batch);
    const v = validate(batch, contract, baseProfile, deps);
    const batchIds = idColumn >= 0 ? idsOf(batch) : new RoaringBitmap();
    const duplicates = idColumn >= 0 ? batch.rows.length - batchIds.size : 0;
    const d = drift(baseProfile, prof);
    for (const [name, b] of Object.entries(baseProfile))
        if (b.frequencies && prof[name]?.frequencies)
            psiChecks.push(
                Math.abs(
                    psi(b.frequencies, prof[name].frequencies) - psiDefinition(b.frequencies, prof[name].frequencies),
                ),
            );
    const badShare = v.badRows / batch.rows.length;
    const replayed = idColumn >= 0 ? batchIds.andCardinality(baselineIds) : 0;
    const verdict =
        badShare > 0.02 || v.failures.some((f) => f.rule === "columns")
            ? "fail"
            : badShare > 0 || replayed > 0 || duplicates > 0 || d.some((x) => x.drifted)
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
        fdRows: v.fdRows,
        duplicates,
        replayed,
        failures: v.failures,
        drift: d.filter((x) => x.drifted).map((x) => x.column),
    });
    print(
        `${path.basename}: ${verdict.toUpperCase()} — ${batch.rows.length.format()} rows (${batch.encoding}), ${v.badRows} violate the contract, ${replayed} replayed ids, ${duplicates} repeated, drift in [${verdicts.at(-1).drift}]`,
    );
    for (const f of v.failures.slice(0, 4)) print(`    ${f.rule} ×${f.count}  e.g. ${f.example}`);
}
print(
    `baseline ${baseline.rows.length.format()} rows (${baselineIds.size.format()} ids in ${baselineIds.bytes.bytes()} of bitmap); contract has ${contract.required.length} required columns, ${Object.values(contract.properties).filter((s) => s.enum || s.anyOf?.[0]?.enum).length} enums, dependencies [${deps.map((d) => `${d.x} -> ${d.y}`)}]`,
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
    results[`exactly one dependency is discovered: city -> country (${deps.map((d) => `${d.x} -> ${d.y}`)})`] =
        deps.length === 1 && deps[0].x === "city" && deps[0].y === "country";
    results[`dependency violations equal the planted rows (${verdicts.map((x) => x.fdRows)} vs ${sample.fdRows})`] =
        clean.fdRows === 0 && legacy.fdRows === 0 && broken.fdRows === sample.fdRows;
    results[`ids repeated inside a batch are counted (${verdicts.map((x) => x.duplicates)})`] =
        clean.duplicates === 0 && legacy.duplicates === 0 && broken.duplicates === 5;
    {
        // the profile's quantiles against the definition: linear interpolation between order statistics of a sort
        const xs = [...baseProfile.amount.values].sort((a, b) => a - b);
        const at = (q) => {
            const h = q * (xs.length - 1),
                lo = Math.floor(h);
            return xs[lo] + (h - lo) * ((xs[lo + 1] ?? xs[lo]) - xs[lo]);
        };
        results["profile quartiles equal the order-statistic definition"] = [
            ["q1", 0.25],
            ["median", 0.5],
            ["q3", 0.75],
            ["p99", 0.99],
        ].every(([k, q]) => Math.abs(baseProfile.amount[k] - at(q)) < 1e-9);
        // KS D by definition: the largest gap between the two empirical CDFs, evaluated at every observed value
        const brokenAmounts = load(batchPaths[2])
            .rows.map((r) => Number(r[1]))
            .filter((v) => !Number.isNaN(v));
        const cdf = (sorted, v) => {
            let n = 0;
            for (const x of sorted) if (x <= v) n++;
            return n / sorted.length;
        };
        const ys = [...brokenAmounts].sort((a, b) => a - b);
        let dRef = 0;
        for (const v of [...xs, ...ys]) dRef = Math.max(dRef, Math.abs(cdf(xs, v) - cdf(ys, v)));
        results[`native PSI equals the definition on ${psiChecks.length} categorical columns`] =
            psiChecks.length > 0 && psiChecks.every((e) => e < 1e-12);
        results[`KS distance equals the definition (${dRef.toFixed(4)})`] =
            Math.abs(ks(baseProfile.amount.values, brokenAmounts).d - dRef) < 1e-12;
    }
    removeAll(work);
    check(results);
}

// ---- payment exports: a baseline, a clean batch, a Latin-1 legacy batch, a broken batch ----
function generateFiles(dir) {
    const rng = new Random(99);
    const countries = ["DE", "FR", "ES", "IT", "NL"],
        methods = ["card", "sepa", "paypal"],
        cityCountry = {
            Köln: "DE",
            München: "DE",
            Besançon: "FR",
            Orléans: "FR",
            Málaga: "ES",
            Forlì: "IT",
            Utrecht: "NL",
            Groningen: "NL",
        },
        cities = Object.keys(cityCountry);
    let fdRows = 0;
    const make = (n, opts = {}) => {
        const rows = [["id", "amount", "currency", "country", "method", "city", "items"]];
        const idBase = opts.idBase ?? 10000;
        for (let i = 0; i < n; i++) {
            let amount = Math.max(1, rng.normal(opts.shift ? 120 : 60, 18));
            if (opts.outliers && i < 3) amount = 5000 + i;
            const city = rng.choice(cities);
            let country = cityCountry[city];
            if (opts.badCountry && i % 20 === 0) country = "XX";
            else if (opts.wrongCountry && i % 25 === 3) country = countries.find((c) => c !== country);
            if (country !== cityCountry[city]) fdRows++;
            rows.push([
                String(opts.replay && i < opts.replay ? 10000 + i * 7 : idBase + i),
                amount.toFixed(2),
                "EUR",
                country,
                rng.choice(methods),
                city,
                String(1 + rng.poisson(2)),
            ]);
        }
        if (opts.negative) rows[5][1] = "-40.00";
        if (opts.retried) {
            // an exporter re-sent a page; its rows bring their dependency violations with them
            const again = rows.slice(101, 101 + opts.retried);
            fdRows += again.filter((r) => cityCountry[r[5]] !== r[3]).length;
            rows.push(...again);
        }
        return toCSV(rows);
    };
    const baseline = dir.join("payments-baseline.csv"),
        clean = dir.join("payments-2026-03-04.csv"),
        legacy = dir.join("payments-legacy.csv"),
        broken = dir.join("payments-2026-03-05.csv");
    writeFile(baseline, make(4000, { outliers: true }));
    writeFile(clean, make(800, { idBase: 20000 }));
    writeFile(legacy, encodeCharset(make(500, { idBase: 30000 }), "windows-1252"));
    writeFile(
        broken,
        make(600, {
            idBase: 40000,
            replay: 15,
            shift: true,
            badCountry: true,
            wrongCountry: true,
            negative: true,
            retried: 5,
        }),
    );
    return { baseline, batches: [clean, legacy, broken], fdRows };
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
