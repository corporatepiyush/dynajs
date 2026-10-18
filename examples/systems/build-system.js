// flags: --std
// Build system — a Bazel-flavoured incremental builder: targets declared in YAML, dependency scanning of imports,
// content-addressed action keys, a local action cache plus a shared remote cache over HTTP, parallel execution on
// worker threads in DAG order, early cutoff, cycle detection, and a critical-path report.
//
// WHAT IT IS
//   BUILD        targets with a rule (transpile, bundle, test, archive), sources, deps and outputs, in YAML
//   scanning     transpile actions read `import … from "./x.js"` lines, so a target's real inputs include the
//                files it imports; an import not declared in deps is a hermeticity error, reported before building
//   action keys  SHA-256 over the rule, its options, and the content hashes of every input (sources and the
//                OUTPUT hashes of dependencies) — so the key changes exactly when something that matters changes
//   caching      outputs are stored by key in a local content-addressed store; on a miss the remote cache (an HTTP
//                service shared by "CI" and "laptops") is asked before running the action; results are uploaded
//   early cutoff a dependency that rebuilds to byte-identical output (a comment-only edit) does not invalidate
//                its dependants, because their keys use output hashes, not "was rebuilt" flags
//   execution    ready actions run on a pool of os.Worker threads, in topological order, with -j parallelism;
//                a failing test stops dependants but lets independent branches finish (keep-going)
//   reporting    per-target status (built / local hit / remote hit / up to date / failed), the critical path
//                through the DAG by measured durations, and the graph in DOT format
//   digests      file content hashes live in an LRU keyed by path and validated by (size, mtime) — Bazel's digest
//                cache — so a no-op build stats every source but hashes none of them; source ownership for the
//                hermeticity scan is one Map lookup per import, built once per load
//
// MODULES  os (Worker) · dyna:yaml · dyna:hash (SHA256) · dyna:http (App) · dyna:structures (Graph, LRU)
//          · dyna:compress (gzip, ZipPack) · dyna:file · dyna:async (sleep) · dyna:cli (Table)
//
// RUN      dynajs --std examples/systems/build-system.js     clean build, no-op build, edits, a failing test, a fresh clone using the remote cache

import * as os from "os";
import * as YAML from "dyna:yaml";
import { SHA256Hex } from "dyna:hash";
import { App } from "dyna:http";
import { Graph, LRU } from "dyna:structures";
import { makeTempDir, makeDir, writeFile, readFile, readBytes, exists, stat, removeAll } from "dyna:file";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("build");
const enc = new TextEncoder(),
    dec = new TextDecoder();

// =============================================================================
// Action implementations (run inside worker threads)
// =============================================================================
const workerPath = work.join("action-worker.js");
writeFile(
    workerPath,
    `
import * as os from "os";
import { gzip, ZipPack } from "dyna:compress";
const parent = os.Worker.parent;
const minify = (src) => src.split("\\n").map((l) => l.replace(/\\/\\/.*$/, "").trim()).filter(Boolean).join("\\n");
const actions = {
    transpile: ({ sources, opts }) => {
        const out = {};
        for (const [name, text] of Object.entries(sources)) {
            let js = text.replace(/:\\s*(number|string|boolean|void)\\b/g, "").replace(/^export type .*$/gm, "");
            if (opts.minify) js = minify(js);
            out[name.replace(/\\.ts$/, ".js")] = js;
        }
        const end = Date.now() + (opts.costMs || 30); while (Date.now() < end) {}
        return out;
    },
    bundle: ({ deps, opts }) => {
        const modules = Object.entries(deps).flatMap(([, files]) => Object.entries(files)).sort(([a], [b]) => (a < b ? -1 : 1));
        const body = modules.map(([n, js]) => "// module " + n + "\\n" + js.replace(/^import .*$/gm, "").replace(/^export /gm, "")).join("\\n");
        return { [opts.out]: "(function(){\\n" + body + "\\n})();\\n" };
    },
    test: ({ deps, opts }) => {
        const all = Object.values(deps).flatMap((f) => Object.values(f)).join("\\n");
        const end = Date.now() + (opts.costMs || 60); while (Date.now() < end) {}
        for (const must of opts.expect || []) if (!all.includes(must)) throw new Error("assertion failed: expected bundle to contain " + JSON.stringify(must));
        return { [opts.out]: "ok " + (opts.expect || []).length + " assertions\\n" };
    },
    archive: ({ deps, opts }) => {
        const entries = Object.values(deps).flatMap((f) => Object.entries(f)).map(([name, text]) => ({ name, data: new TextEncoder().encode(text), mtime: 0 }));
        const zip = ZipPack(entries.sort((a, b) => (a.name < b.name ? -1 : 1)));
        return { [opts.out]: Array.from(gzip(zip)).map((b) => String.fromCharCode(b)).join("") };
    },
};
parent.onmessage = (e) => {
    const m = e.data;
    if (m.type === "stop") { parent.onmessage = null; return; }
    const t = Date.now();
    try { parent.postMessage({ id: m.id, ok: true, outputs: actions[m.rule](m), ms: Date.now() - t }); }
    catch (err) { parent.postMessage({ id: m.id, ok: false, error: String(err.message || err), ms: Date.now() - t }); }
};
`,
);

// =============================================================================
// Remote cache service (shared between machines)
// =============================================================================
const remoteStore = new Map(),
    remoteStats = { gets: 0, hits: 0, puts: 0 };
const remote = new App({ port: 0 });
remote.get("/ac/:key", (req) => {
    remoteStats.gets++;
    const v = remoteStore.get(req.params.key);
    if (!v) return { status: 404, body: "" };
    remoteStats.hits++;
    return { body: v, contentType: "application/json" };
});
remote.put("/ac/:key", (req) => {
    remoteStats.puts++;
    remoteStore.set(req.params.key, req.body);
    return { status: 201, body: "" };
});
remote.start();
const REMOTE = `http://127.0.0.1:${remote.port}`;

// =============================================================================
// The workspace
// =============================================================================
function writeWorkspace(root) {
    const f = (p, t) => {
        const path = root.join(p);
        makeDir(path.dirname, { recursive: true });
        writeFile(path, t);
    };
    f(
        "BUILD.yaml",
        `
targets:
  //lib/money:
    rule: transpile
    srcs: [lib/money.ts]
    opts: { minify: true, costMs: 40 }
  //lib/tax:
    rule: transpile
    srcs: [lib/tax.ts]
    deps: [//lib/money]
    opts: { minify: true, costMs: 40 }
  //lib/catalog:
    rule: transpile
    srcs: [lib/catalog.ts]
    opts: { minify: true, costMs: 80 }
  //app/checkout:
    rule: transpile
    srcs: [app/checkout.ts]
    deps: [//lib/tax, //lib/money, //lib/catalog]
    opts: { minify: true, costMs: 60 }
  //app:bundle:
    rule: bundle
    deps: [//app/checkout, //lib/tax, //lib/money, //lib/catalog]
    opts: { out: app.bundle.js }
  //app:test:
    rule: test
    deps: [//app:bundle]
    opts: { out: test.log, costMs: 120, expect: ["function total", "function vat", "function price"] }
  //lib:test:
    rule: test
    deps: [//lib/money, //lib/tax]
    opts: { out: libtest.log, costMs: 50, expect: ["function vat"] }
  //dist:release:
    rule: archive
    deps: [//app:bundle, //app:test]
    opts: { out: release.zip.gz }
`,
    );
    f(
        "lib/money.ts",
        "// Money helpers\nexport function round2(x: number): number {\n  return Math.round(x * 100) / 100;\n}\n",
    );
    f(
        "lib/tax.ts",
        'import { round2 } from "./money.js";\n// VAT by country\nexport function vat(net: number, country: string): number {\n  return round2(net * ({ DE: 0.19, FR: 0.2 }[country] ?? 0));\n}\n',
    );
    f(
        "lib/catalog.ts",
        "export type Sku = string;\nexport function price(sku: string): number {\n  return { A: 10, B: 25 }[sku] ?? 0;\n}\n",
    );
    f(
        "app/checkout.ts",
        'import { vat } from "../lib/tax.js";\nimport { price } from "../lib/catalog.js";\nimport { round2 } from "../lib/money.js";\nexport function total(skus: string, country: string): number {\n  const net = skus.split(",").reduce((s, k) => s + price(k), 0);\n  return round2(net + vat(net, country));\n}\n',
    );
}

// =============================================================================
// The builder
// =============================================================================
class Builder {
    constructor(root, { jobs = 3, useRemote = true } = {}) {
        this.root = root;
        this.jobs = jobs;
        this.useRemote = useRemote;
        this.cas = root.join(".cache");
        makeDir(this.cas, { recursive: true });
        this.outputs = new Map();
        this.digests = new LRU(10_000);
        this.hashed = 0;
    }
    digest(path) {
        // content hash, recomputed only when size or mtime moved
        const st = stat(this.root.join(path)),
            hit = this.digests.get(path);
        if (hit && hit.size === st.size && hit.mtimeMs === st.mtimeMs) return hit.hash;
        this.hashed++;
        const hash = SHA256Hex(readBytes(this.root.join(path)));
        this.digests.put(path, { size: st.size, mtimeMs: st.mtimeMs, hash });
        return hash;
    }
    load() {
        const spec = YAML.Parse(readFile(this.root.join("BUILD.yaml"))).targets;
        const names = Object.keys(spec),
            g = new Graph({ directed: true }),
            id = new Map(names.map((n) => [n, g.addNode()]));
        for (const [n, t] of Object.entries(spec))
            for (const d of t.deps || []) {
                if (!id.has(d)) throw new Error(`${n}: unknown dependency ${d}`);
                g.addEdge(id.get(d), id.get(n));
            }
        const order = g.topologicalSort();
        if (!order || order.length !== names.length) throw new Error("dependency cycle in BUILD.yaml");
        this.ownerOf = new Map(Object.entries(spec).flatMap(([n, t]) => (t.srcs || []).map((src) => [src, n])));
        return { spec, order: order.map((i) => names[i]) };
    }
    scan(name, t) {
        // imports must be covered by declared deps
        const problems = [];
        for (const src of t.srcs || []) {
            for (const m of readFile(this.root.join(src)).matchAll(/^import .* from "(.+)";$/gm)) {
                const target = this.root
                    .join(src)
                    .dirname.join(m[1])
                    .toString()
                    .replace(this.root.toString() + "/", "")
                    .replace(/\.js$/, ".ts");
                const norm = target
                    .split("/")
                    .reduce((acc, p) => (p === ".." ? acc.slice(0, -1) : p === "." ? acc : [...acc, p]), [])
                    .join("/");
                const owner = this.ownerOf.get(norm);
                if (owner && owner !== name && !(t.deps || []).includes(owner))
                    problems.push(`${name}: ${src} imports ${norm} from ${owner}, which is not a declared dependency`);
            }
        }
        return problems;
    }
    actionKey(name, t) {
        const inputs = Object.fromEntries((t.srcs || []).map((s) => [s, this.digest(s)]));
        const deps = Object.fromEntries((t.deps || []).map((d) => [d, this.outputs.get(d).hash]));
        return SHA256Hex(JSON.stringify({ rule: t.rule, opts: t.opts || {}, inputs, deps, v: 1 }));
    }
    async build() {
        const { spec, order } = this.load();
        this.spec = spec;
        const problems = order.flatMap((n) => this.scan(n, spec[n]));
        if (problems.length) return { problems };
        const pool = Array.from({ length: this.jobs }, () => ({
            w: new os.Worker(workerPath.toString()),
            busy: false,
        }));
        const waiting = new Map();
        for (const slot of pool)
            slot.w.onmessage = (e) => {
                const cb = waiting.get(e.data.id);
                waiting.delete(e.data.id);
                cb(e.data);
            };
        const status = new Map(),
            timing = new Map(),
            done = new Map();
        let seq = 0;
        const run = async (name) => {
            const t = spec[name];
            for (const d of t.deps || [])
                if (!(await done.get(d))) {
                    status.set(name, "skipped (dependency failed)");
                    return false;
                }
            const key = this.actionKey(name, t),
                local = this.cas.join(key);
            if (exists(local)) {
                this.outputs.set(name, JSON.parse(readFile(local)));
                status.set(name, "local hit");
                timing.set(name, 0);
                return true;
            }
            if (this.useRemote) {
                const r = await fetch(`${REMOTE}/ac/${key}`);
                if (r.status === 200) {
                    const body = await r.text();
                    writeFile(local, body);
                    this.outputs.set(name, JSON.parse(body));
                    status.set(name, "remote hit");
                    timing.set(name, 0);
                    return true;
                } else await r.text();
            }
            while (!pool.some((s) => !s.busy)) await sleep(2);
            const slot = pool.find((s) => !s.busy);
            slot.busy = true;
            const deps = Object.fromEntries((t.deps || []).map((d) => [d, this.outputs.get(d).files]));
            const sources = Object.fromEntries((t.srcs || []).map((s) => [s, readFile(this.root.join(s))]));
            const id = ++seq;
            const res = await new Promise((resolve) => {
                waiting.set(id, resolve);
                slot.w.postMessage({ id, rule: t.rule, opts: t.opts || {}, sources, deps });
            });
            slot.busy = false;
            timing.set(name, res.ms);
            if (!res.ok) {
                status.set(name, `FAILED: ${res.error}`);
                return false;
            }
            const record = { files: res.outputs, hash: SHA256Hex(JSON.stringify(res.outputs)) };
            const prev = this.outputs.get(name);
            this.outputs.set(name, record);
            const body = JSON.stringify(record);
            writeFile(local, body);
            if (this.useRemote) await (await fetch(`${REMOTE}/ac/${key}`, { method: "PUT", body })).text();
            status.set(name, prev && prev.hash === record.hash ? "built (same output)" : "built");
            return true;
        };
        const started = Date.now();
        for (const n of order) done.set(n, run(n));
        await Promise.all(done.values());
        for (const s of pool) {
            s.w.onmessage = null;
            s.w.postMessage({ type: "stop" });
        }
        const finish = new Map(); // critical path by measured durations
        for (const n of order)
            finish.set(n, (timing.get(n) || 0) + Math.max(0, ...(spec[n].deps || []).map((d) => finish.get(d) || 0)));
        const end = [...finish].sort((a, b) => b[1] - a[1])[0];
        const path = [];
        let cur = end?.[0];
        while (cur) {
            path.unshift(cur);
            cur = (spec[cur].deps || []).sort((a, b) => (finish.get(b) || 0) - (finish.get(a) || 0))[0];
        }
        return {
            status,
            timing,
            wallMs: Date.now() - started,
            critical: { path, ms: end?.[1] ?? 0 },
            dot: `digraph build {\n${order.flatMap((n) => (spec[n].deps || []).map((d) => `  "${d}" -> "${n}";`)).join("\n")}\n}`,
        };
    }
}

// =============================================================================
// Scenario
// =============================================================================
const results = {};
const ws = work.join("ws");
writeWorkspace(ws);
const b = new Builder(ws, { jobs: 3 });
const runs = [];
const r1 = await b.build();
runs.push(["clean", r1]);
results["a clean build builds every target"] = [...r1.status.values()].every((s) => s === "built");
const serial = [...r1.timing.values()].reduce((a, x) => a + x, 0);
results[`parallel execution: ${r1.wallMs} ms wall for ${serial} ms of actions`] = r1.wallMs < serial * 0.95;
const hashedBefore = b.hashed;
const r2 = await b.build();
runs.push(["no-op", r2]);
results["a second build is all cache hits"] = [...r2.status.values()].every((s) => s === "local hit");
results["the no-op build re-hashed no source file (digest cache)"] = b.hashed === hashedBefore;
// A comment-only edit in money.ts: money rebuilds to the same (minified) output; nothing downstream reruns.
writeFile(
    ws.join("lib/money.ts"),
    readFile(ws.join("lib/money.ts")).replace("// Money helpers", "// Money helpers (rounding to cents)"),
);
const r3 = await b.build();
runs.push(["comment edit", r3]);
results["early cutoff: an identical output does not rebuild dependants"] =
    r3.status.get("//lib/money") === "built (same output)" &&
    r3.status.get("//lib/tax") === "local hit" &&
    r3.status.get("//dist:release") === "local hit";
// A real change in catalog.ts: catalog, checkout, bundle, app test and release rebuild; lib/tax and lib:test do not.
writeFile(ws.join("lib/catalog.ts"), readFile(ws.join("lib/catalog.ts")).replace("B: 25", "B: 25, C: 40"));
const r4 = await b.build();
runs.push(["catalog change", r4]);
const rebuilt = [...r4.status]
    .filter(([, s]) => s.startsWith("built"))
    .map(([n]) => n)
    .sort();
results[`only the affected targets rebuilt: ${rebuilt.join(", ")}`] =
    rebuilt.join() === "//app/checkout,//app:bundle,//app:test,//dist:release,//lib/catalog";
// A change that breaks the app test: release is skipped, the unrelated lib test still runs.
writeFile(
    ws.join("lib/catalog.ts"),
    readFile(ws.join("lib/catalog.ts")).replace("export function price", "export function unitPrice"),
);
writeFile(
    ws.join("lib/tax.ts"),
    readFile(ws.join("lib/tax.ts"))
        .replace("// VAT by country", "// VAT by country (EU)")
        .replace("round2(net *", "round2(1 * net *"),
);
const r5 = await b.build();
runs.push(["broken test", r5]);
results["a failing test skips its dependants while independent targets finish"] =
    /FAILED: assertion failed/.test(r5.status.get("//app:test")) &&
    r5.status.get("//dist:release").startsWith("skipped") &&
    r5.status.get("//lib:test").startsWith("built");
writeFile(
    ws.join("lib/catalog.ts"),
    readFile(ws.join("lib/catalog.ts")).replace("export function unitPrice", "export function price"),
);
const r6 = await b.build();
runs.push(["fixed", r6]);
results["the fix restores a green build"] = /^(local hit|built)/.test(r6.status.get("//app:test"));
// Hermeticity: checkout imports catalog without declaring it.
const spec = readFile(ws.join("BUILD.yaml"));
writeFile(
    ws.join("BUILD.yaml"),
    spec.replace("deps: [//lib/tax, //lib/money, //lib/catalog]", "deps: [//lib/tax, //lib/money]"),
);
const r7 = await b.build();
results["an undeclared import is reported before anything builds"] =
    r7.problems?.length === 1 && /imports lib\/catalog\.ts from \/\/lib\/catalog/.test(r7.problems[0]);
writeFile(
    ws.join("BUILD.yaml"),
    spec.replace(
        "deps: [//lib/money]\n    opts: { minify: true, costMs: 40 }\n  //lib/catalog",
        "deps: [//lib/money, //app/checkout]\n    opts: { minify: true, costMs: 40 }\n  //lib/catalog",
    ),
);
let cycle = null;
try {
    await b.build();
} catch (e) {
    cycle = e.message;
}
results["a dependency cycle is refused"] = /cycle/.test(cycle || "");
writeFile(ws.join("BUILD.yaml"), spec);
// A fresh clone on another machine: everything comes from the remote cache.
const clone = work.join("clone");
writeWorkspace(clone);
writeFile(clone.join("lib/money.ts"), readFile(ws.join("lib/money.ts")));
writeFile(clone.join("lib/catalog.ts"), readFile(ws.join("lib/catalog.ts")));
writeFile(clone.join("lib/tax.ts"), readFile(ws.join("lib/tax.ts")));
const r8 = await new Builder(clone, { jobs: 3 }).build();
runs.push(["fresh clone", r8]);
results[`a fresh clone downloads all ${r8.status.size} targets from the remote cache`] = [...r8.status.values()].every(
    (s) => s === "remote hit",
);
const bundle = b.outputs.get("//app:bundle").files["app.bundle.js"];
results["the bundle is transpiled and linked"] =
    bundle.includes("function total") && !bundle.includes(": number") && !/^import /m.test(bundle);

print("build-system");
print(
    table([
        ["target", ...runs.map(([n]) => n)],
        ...[...r1.status.keys()].map((t) => [
            t,
            ...runs.map(([, r]) =>
                (r.status.get(t) || "").replace("FAILED: assertion failed: expected bundle to contain", "FAILED:"),
            ),
        ]),
    ]),
);
print(
    `  critical   ${r1.critical.path.join(" -> ")} (${r1.critical.ms} ms of ${serial} ms total work; clean wall ${r1.wallMs} ms)`,
);
print(`  remote     ${remoteStats.puts} uploads, ${remoteStats.hits}/${remoteStats.gets} hits`);
print(`  graph      ${r1.dot.split("\n").length - 2} edges in DOT`);
remote.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
