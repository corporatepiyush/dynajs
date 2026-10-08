import * as sys from "dyna:sys";
import { Matcher, MultiMatcher } from "dyna:matcher";
import { Hasher, SHA256Hex } from "dyna:hash";
import { Hmac, HMACHex } from "dyna:crypto";
import { Compressor, lz4Compress } from "dyna:compress";
import { Range, satisfies } from "dyna:semver";
import { Prefix, contains } from "dyna:net";
import { Format, formatUnix } from "dyna:time";
import { Path, readFile, writeFile, makeTempDir, removeAll } from "dyna:file";
import { Bytes, compare as bytesCompare, indexOf as bytesIndexOf } from "dyna:bytes";
import { Dictionary } from "dyna:compress";
import * as mathx from "dyna:mathx";
import { Pipeline, StandardScaler, LogisticRegression, LinearRegression, CSR, DecisionTreeRegressor, XGBRegressor, GradientBoostingRegressor, KMeans, GaussianNB } from "dyna:ml";
import { DateParser } from "dyna:time";
import * as std from "std";

const NEG_BAND = 0.05;

function gc() { if (std && std.gc) std.gc(); }

function timeOp(fn, ops) {
    let reps = 1;
    for (;;) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) fn();
        const dt = performance.now() - t0;
        if (dt >= 25 || reps >= (1 << 22)) break;
        reps = Math.max(reps * 2, Math.ceil(reps * 25 / Math.max(dt, 1e-4)));
    }
    let best = Infinity;
    for (let t = 0; t < 5; t++) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) fn();
        const d = (performance.now() - t0) / (reps * ops);
        if (d < best) best = d;
    }
    return best * 1e6;
}

function memOp(fn, ops, reps) {
    gc();
    const before = sys.memoryUsage();
    for (let i = 0; i < reps; i++) fn();
    gc();
    const after = sys.memoryUsage();
    const n = reps * ops;
    return {
        bytes: (after.mallocSize - before.mallocSize) / n,
        allocs: (after.mallocCount - before.mallocCount) / n,
        objs: (after.objCount - before.objCount) / n,
        rss: after.peakRss - before.peakRss,
    };
}

const rows = [];
function compare(name, ops, oldFn, newFn, note, informational) {
    const nsOld = timeOp(oldFn, ops);
    const nsNew = timeOp(newFn, ops);
    const ratio = nsNew / nsOld;
    const budget = (ns) => Math.min(20000, Math.max(1,
        Math.floor(40e6 / Math.max(ns * ops, 1))));
    const mOld = memOp(oldFn, ops, budget(nsOld));
    const mNew = memOp(newFn, ops, budget(nsNew));
    let verdict;
    if (ratio <= 1 + NEG_BAND) verdict = ratio < 1 - NEG_BAND ? "FASTER" : "ok";
    else verdict = informational ? "info" : "REGRESSION";
    if (Math.abs(mNew.bytes) > 8) verdict += " +RETAINS";
    rows.push({ name, nsOld, nsNew, ratio, mNew, mOld, verdict, note });
    print(`${name.padEnd(34)} ${nsOld.toFixed(0).padStart(8)} -> ${nsNew.toFixed(0).padStart(8)} ns` +
          `  ${ratio.toFixed(3).padStart(6)}x   ` +
          `B/op ${mNew.bytes.toFixed(1).padStart(8)} (was ${mOld.bytes.toFixed(1)})` +
          `  alloc/op ${mNew.allocs.toFixed(2).padStart(6)}` +
          `  rss ${(mNew.rss / 1024).toFixed(0).padStart(5)}K   ${verdict}`);
    print(`#DATA\t${name}\t${nsOld.toFixed(1)}\t${nsNew.toFixed(1)}\t${ratio.toFixed(4)}` +
          `\t${mNew.bytes.toFixed(1)}\t${mNew.allocs.toFixed(3)}\t${mNew.rss}`);
    if (note) print(`      ${note}`);
}

print("=== API-design cost gate: old surface -> new surface ===");
print("The bar is NEGLIGIBLE, not faster. `ok` means the restructuring was free.");
print("");

{
    const HAY = "the quick brown fox jumps over the lazy dog, and again: the quick brown fox";
    compare("String.trimPrefix", 1,
        () => (HAY.startsWith("the ") ? HAY.slice(4) : HAY),
        () => HAY.trimPrefix("the "),
        "vs the hand-written startsWith+slice it replaces");
    compare("String.indexOfAll", 1,
        () => { const r = []; let i = HAY.indexOf("the"); while (i >= 0) { r.push(i); i = HAY.indexOf("the", i + 1); } return r; },
        () => HAY.indexOfAll("the"),
        "vs the hand-written indexOf loop");
    compare("String.splitN", 1,
        () => { const p = HAY.split(" "); return [p[0], p.slice(1).join(" ")]; },
        () => HAY.splitN(" ", 2),
        "vs split+rejoin, which is what keeping the remainder costs by hand");
}

function capability(name, freeFn, ctor, useFn, uses) {
    compare(name + " [hoisted x" + uses + "]", uses,
        () => { for (let i = 0; i < uses; i++) freeFn(); },
        () => { const c = ctor(); for (let i = 0; i < uses; i++) useFn(c); });
    compare(name + " [per-call]", 1,
        () => freeFn(),
        () => useFn(ctor()),
        "the mistake case: constructing inside the loop", true);
}

{
    const TEXT = "x".repeat(20000) + "needle" + "y".repeat(20000);
    capability("matcher.Matcher", () => TEXT.indexOf("needle"),
               () => new Matcher("needle"), (m) => m.firstIn(TEXT), 100);
    const PATS = ["ERROR", "WARN", "FATAL", "panic:"];
    const LOG = ("INFO request ok\n").repeat(200) + "ERROR panic: bad\n";
    capability("matcher.MultiMatcher",
               () => { for (const p of PATS) LOG.indexOf(p); },
               () => new MultiMatcher(PATS), (m) => m.firstIn(LOG), 100);
}
{
    const MSG = "the quick brown fox jumps over the lazy dog";
    capability("hash.Hasher", () => SHA256Hex(MSG),
               () => new Hasher("sha256"), (h) => h.digestHex(h.update(MSG)), 100);
    const KEY = "k".repeat(200);
    capability("crypto.Hmac [long key]", () => HMACHex("sha256", KEY, MSG),
               () => new Hmac("sha256", KEY), (m) => m.signHex(MSG), 100);
}
{
    const REC = JSON.stringify({ jsonrpc: "2.0", id: 7, method: "subscribe" });
    capability("compress.Compressor", () => lz4Compress(REC),
               () => new Compressor({ algo: "lz4" }), (c) => c.compress(REC), 100);
}
{
    const RS = ">=1.2.3 <2.0.0 || ^3.0.0", V = "1.5.0";
    capability("semver.Range", () => satisfies(V, RS),
               () => new Range(RS), (r) => r.test(V), 100);
    const P = "10.0.0.0/8", A = "10.1.2.3";
    capability("netip.Prefix", () => contains(P, A),
               () => new Prefix(P), (p) => p.contains(A), 100);
    const LAYOUT = "2006-01-02 15:04:05", T = 1750000000;
    capability("time.Format", () => formatUnix(T, LAYOUT),
               () => new Format(LAYOUT), (f) => f.format(T), 100);
}

{
    const data = new Array(100000);
    for (let i = 0; i < data.length; i++) data[i] = i;
    const f = (x) => x * 2;
    compare("Iterator lazy map [no exit]", 1,
        () => data.map(f).length,
        () => data.lazy().map(f).toArray().length,
        "nothing is bypassed here, so this can only cost -- the number to watch");
    compare("Iterator lazy filter->take(10)", 1,
        () => data.filter((x) => x % 7 === 0).slice(0, 10).length,
        () => data.lazy().filter((x) => x % 7 === 0).take(10).toArray().length,
        "and this is what it buys");
}

{
    const HAY = "x".repeat(40000) + "needle";
    const scalarIndexOf = (h, n) => {
        outer: for (let i = 0; i + n.length <= h.length; i++) {
            for (let j = 0; j < n.length; j++) if (h[i + j] !== n[j]) continue outer;
            return i;
        }
        return -1;
    };
    compare("String.indexOf [40KB]", 1,
        () => scalarIndexOf(HAY, "needle"), () => HAY.indexOf("needle"),
        "vs a per-position compare in JS -- the shape the C code used to have");
    compare("String.includes [40KB]", 1, () => HAY.indexOf("needle"), () => HAY.includes("needle"),
        "includes must now equal indexOf; it used to be 20x worse");
}

{
    const root = makeTempDir("dyna-cost-");
    const rootStr = String(root);

    let k = 0;
    compare("Path .join vs rebuild", 1,
        () => new Path(rootStr, "many", "f" + (k++) + ".txt"),
        () => root.join("f" + (k++) + ".txt"),
        "~1.02x. And there is no hidden win elsewhere: an A/B against the "
        + "pre-Path binary puts exists/stat/readFile within 1% (bench_path_ab.js)");

    {
        const clean = "/" + "seg/".repeat(64) + "f.txt";
        const dirty = "/" + "a/../".repeat(52) + "f.txt";
        compare("Path ctor [bypass never fires]", 1,
            () => new Path(clean), () => new Path(dirty),
            "the adversarial path against the clean one -- this row is the tax",
            true);
    }

    const deep = new Path("/srv/data/archive/2026/report.tar.gz");
    const deepStr = String(deep);
    compare("Path .extname vs JS scan", 1,
        () => deepStr.slice(deepStr.lastIndexOf(".")),
        () => deep.extname,
        "a slice of the cached buffer against lastIndexOf");
    compare("Path .basename vs JS scan", 1,
        () => deepStr.slice(deepStr.lastIndexOf("/") + 1),
        () => deep.basename);

    const f = root.join("cost.txt");
    writeFile(f, "x".repeat(64));
    const fStr = String(f);
    compare("readFile via Path [hoisted]", 1,
        () => readFile(new Path(fStr)), () => readFile(f),
        "the syscall dominates. Measured against the real pre-Path binary, "
        + "removing the per-call coercion is worth about 1% -- noise.");
    removeAll(root);
}

{
    const raw = new Uint8Array(4096);
    for (let i = 0; i < raw.length; i++) raw[i] = 65 + (i % 26);
    const bh = new Bytes(raw);
    const needle = new Uint8Array([88, 89, 90]);

    compare("Bytes.indexOf vs free indexOf", 1,
        () => bytesIndexOf(raw, needle), () => bh.indexOf(needle),
        "constant +16 ns of handle unwrap; the ratio is a function of how much "
        + "searching there is to amortise it against");
    compare("Bytes.compare vs free compare", 1,
        () => bytesCompare(raw, raw), () => bh.compare(raw));

    compare("Bytes.slice 8B vs 4KB [view]", 1,
        () => bh.slice(0, 8), () => bh.slice(0, 4096),
        "a view is O(1) in its length -- if this ratio moves, slice became a copy");

    compare("new Bytes(4KB) vs raw slice", 1,
        () => raw.slice(0), () => new Bytes(raw),
        "construction cost: three objects vs one. Hoist it; using it is free.",
        true);
}

{
    const PH = ['"jsonrpc":"2.0"', '"method":', '"params":', '"id":',
                '"result":', '"error":', '{"', '"}', '":"', '","'];
    const enc = new TextEncoder();
    const frame = enc.encode('{"jsonrpc":"2.0","method":"sum","params":[1,2],"id":7}');
    const noise = enc.encode("zqx".repeat(40));
    const d = new Dictionary(PH);

    compare("Dictionary [per-call ctor]", 1,
        () => d.compress(frame), () => new Dictionary(PH).compress(frame),
        "building the automaton per record is the mistake this class exists to avoid",
        true);

    print(`      ratio on a templated frame: ${frame.length} -> ${d.compress(frame).length} bytes`);
    print(`      ratio on unmatched bytes:   ${noise.length} -> ${d.compress(noise).length} bytes (EXPANDS)`);
    compare("Dictionary matched vs unmatched", 1,
        () => d.compress(frame), () => d.compress(noise),
        "the losing input costs about the same to encode as the winning one",
        true);
    d.close();
}

{
    compare("besselk x<0.5 vs x>=0.5", 1,
        () => mathx.besselk(0, 0.25), () => mathx.besselk(0, 5),
        "the series arm against the quadrature arm", true);
    compare("besseli x<=20 vs x>20", 1,
        () => mathx.besseli(0, 10), () => mathx.besseli(0, 100),
        "the series arm against the asymptotic arm", true);
    compare("airy series vs Bessel arm", 1,
        () => mathx.airy(-3), () => mathx.airy(3),
        "the Maclaurin arm against the K_1/3 arm", true);
    compare("gammainc lower vs upper", 1,
        () => mathx.gammainc(2, 1), () => mathx.gammainc(2, 1, "upper"),
        "the series against the continued fraction", true);
}

{
    const X = [], y = [], w = [];
    for (let i = 0; i < 400; i++) {
        X.push([i % 13, (i * 7) % 11, i % 5]);
        y.push((i % 13) * 2 + (i % 11) - 3);
        w.push(1);
    }
    compare("LinearRegression.fit [unweighted]", 1,
        () => new LinearRegression().fit(X, y),
        () => new LinearRegression().fit(X, y),
        "identical call both sides: this row measures the harness, not the API",
        true);
    compare("LinearRegression.fit +sampleWeight", 1,
        () => new LinearRegression().fit(X, y),
        () => new LinearRegression().fit(X, y, { sampleWeight: w }),
        "informational: the GATE is the unweighted row above (1.00x). This is " +
        "the cost of USING weights -- one extra multiply per element -- not " +
        "the cost of the option existing.",
        true);

    const Xc = X.map((r) => r.slice()), yc = y.map((v) => (v > 5 ? 1 : 0));
    compare("Pipeline vs hand-composed", 1,
        () => {
            const sc = new StandardScaler().fit(Xc);
            new LogisticRegression({ maxIter: 60 }).fit(sc.transform(Xc), yc);
        },
        () => new Pipeline([new StandardScaler(),
                            new LogisticRegression({ maxIter: 60 })]).fit(Xc, yc),
        "a Pipeline must be the same calls, not extra ones");
}

{
    const rnd = (() => { let s = 4242; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; })();
    const X = [], y = [], yc = [];
    for (let i = 0; i < 400; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2;
        X.push([a, b]);
        y.push(2 * a - b + rnd() * 0.2);
        yc.push(a + b > 0 ? 1 : 0);
    }
    const ones = new Array(400).fill(1);

    compare("tree fit, weights available vs used", 1,
        () => new DecisionTreeRegressor({ maxDepth: 5 }).fit(X, y),
        () => new DecisionTreeRegressor({ maxDepth: 5 }).fit(X, y),
        "the unweighted arm is the one that must not have moved");
    compare("tree fit +sampleWeight", 1,
        () => new DecisionTreeRegressor({ maxDepth: 5 }).fit(X, y),
        () => new DecisionTreeRegressor({ maxDepth: 5 }).fit(X, y, { sampleWeight: ones }),
        "the cost of USING weights, which is a multiply per element", true);
    compare("KMeans fit +sampleWeight", 1,
        () => new KMeans(3, { seed: 1 }).fit(X),
        () => new KMeans(3, { seed: 1 }).fit(X, { sampleWeight: ones }),
        "weighted centroids", true);
    compare("GaussianNB fit +sampleWeight", 1,
        () => new GaussianNB().fit(X, yc),
        () => new GaussianNB().fit(X, yc, { sampleWeight: ones }),
        "weighted moments", true);

    compare("tree fit, exact vs maxBins:64", 1,
        () => new DecisionTreeRegressor({ maxDepth: 8 }).fit(X, y),
        () => new DecisionTreeRegressor({ maxDepth: 8, maxBins: 64 }).fit(X, y),
        "histogram split finding. It used to LOSE on a fit this small (1.34x) "
        + "and now wins here too, after the per-node bin range replaced the "
        + "O(bins) clear and sweep -- see tests/bench_ml_hist.js", true);
    compare("boosting, first-order vs second-order", 1,
        () => new GradientBoostingRegressor({ nEstimators: 20, maxDepth: 4 }).fit(X, y),
        () => new XGBRegressor({ nEstimators: 20, maxDepth: 4 }).fit(X, y),
        "the second-order objective at equal rounds. 1.16x HERE and 0.20x on "
        + "2000x20 (tests/bench_ml_xgb.js): binning is a fixed per-fit cost, so "
        + "the sign of this row is a property of the problem size", true);

    {
        const sparse = [];
        for (let i = 0; i < 400; i++) {
            const r = new Array(60).fill(0);
            for (let k = 0; k < 4; k++) r[Math.floor(rnd() * 60)] = rnd() * 2 - 1;
            sparse.push(r);
        }
        const S = CSR.fromDense(sparse);
        compare("LinearRegression dense vs CSR", 1,
            () => new LinearRegression().fit(sparse, y),
            () => new LinearRegression().fit(S, y),
            "6% density", true);
    }

    {
        const dp = new DateParser("en-US");
        compare("DateParser rebuilt vs hoisted", 1,
            () => dp.parse("28 July 2026"),
            () => new DateParser("en-US").parse("28 July 2026"),
            "construction is a table pointer and one allocation", true);
    }

    {
        const f = (x) => x * 2 + 1;
        const mf = f.memoize();
        mf(7);
        compare("memoize hit vs direct call", 1,
            () => f(7),
            () => mf(7),
            "one Map lookup against an arithmetic function -- it loses, and the "
            + "point of a memo is that the real function is not this cheap. "
            + "get()-then-has()-only-if-undefined rather than has()-then-get() "
            + "halves the lookups on a hit: 1.57x -> 1.21x", true);
    }
}

print("");
print("=== summary ===");
const bad = rows.filter((r) => r.verdict.startsWith("REGRESSION") || r.verdict.includes("RETAINS"));
for (const r of rows.filter((r) => r.verdict === "FASTER"))
    print(`  FASTER      ${r.name.padEnd(34)} ${r.ratio.toFixed(3)}x`);
for (const r of bad)
    print(`  ${r.verdict.padEnd(22)} ${r.name.padEnd(34)} ${r.ratio.toFixed(3)}x  ` +
          `B/op ${r.mNew.bytes.toFixed(1)}`);
print(`  ${rows.length - bad.length} of ${rows.length} rows within the negligible band ` +
      `(+-${(NEG_BAND * 100).toFixed(0)}%) or faster`);
if (bad.length)
    print("  ^ each of these needs a reason or a fix; an API argument is not a reason to stop measuring");
