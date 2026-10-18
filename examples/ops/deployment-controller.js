// apps:solo -- its canary rollout is driven by the clock, so it runs alone
// Deployment controller — a reconciling control loop with rolling updates, readiness probes, canary analysis,
// automatic rollback, revision history and config-driven rollouts.
//
// WHAT IT IS
//   The core of a Kubernetes Deployment controller plus an Argo-Rollouts-style canary, managing real HTTP
//   server instances ("pods") in this process:
//
//   desired      a YAML manifest: image (a semver-tagged release), replicas, maxSurge / maxUnavailable, a probe,
//                a canary strategy (steps of traffic weight with an analysis between steps) and a config map
//   observed     the controller watches its pods: each one is an HTTP server answering /healthz and /api/price
//   reconcile    a level-triggered loop: compute the diff between desired and observed (replica count, image,
//                config hash) and take ONE safe step at a time, so a crash between steps is harmless
//   rollout      surge new pods, wait for readiness (consecutive successful probes), only then retire old ones,
//                never dropping below replicas - maxUnavailable ready pods
//   canary       a weighted router sends a share of live traffic to the new version; after each step the error
//                rate and latency of canary vs stable are compared (two-proportion z-test, p95 ratio); a failing
//                analysis aborts and rolls back to the last good revision automatically
//   history      revisions are recorded with the manifest hash; `rollback --to N` re-applies one
//   leadership   a lock file ensures only one controller acts (a second instance stands by)
//   diffs        every revision stores a line diff (Myers, dyna:matcher DiffLines) of its manifest against the
//                previous one, so `history` shows exactly what a rollout changed — the kubectl diff view
//   analysis     canary and stable latencies go into one TDigest each; p95 and the canary's share of requests
//                slower than the stable p99 (via cdf) come straight from the sketches
//
// MODULES  dyna:http (App) · dyna:yaml · dyna:semver · dyna:hash (SHA256) · dyna:file (FileLock) · dyna:mathx (erfc)
//          · dyna:structures (SortedMap, TDigest) · dyna:matcher (DiffLines) · dyna:async (sleep) · dyna:cli (Table)
//
// RUN      dynajs examples/ops/deployment-controller.js     deploys v1.4.0, a good v1.5.0, then a bad v1.6.0 that rolls back

import { App } from "dyna:http";
import * as YAML from "dyna:yaml";
import * as semver from "dyna:semver";
import { SHA256Hex } from "dyna:hash";
import { FileLock, makeTempDir, removeAll } from "dyna:file";
import { erfc } from "dyna:mathx";
import { SortedMap, TDigest } from "dyna:structures";
import { DiffLines } from "dyna:matcher";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("deploy");

// =============================================================================
// The "image registry": what each release does when it runs
// =============================================================================
const RELEASES = {
    "1.4.0": { startupMs: 60, errorRate: 0.002, latencyMs: 20, priceFactor: 1 },
    "1.5.0": { startupMs: 90, errorRate: 0.002, latencyMs: 18, priceFactor: 1 },
    "1.6.0": { startupMs: 80, errorRate: 0.06, latencyMs: 55, priceFactor: 1 }, // regressed
};

// =============================================================================
// Pods
// =============================================================================
let podSeq = 0;
class Pod {
    constructor(image, config, revision) {
        this.name = `api-${revision}-${(++podSeq).toString(36)}`;
        this.image = image;
        this.config = config;
        this.revision = revision;
        this.release = RELEASES[image];
        this.startedAt = Date.now();
        this.ready = false;
        this.okProbes = 0;
        this.terminating = false;
        let n = 0;
        this.app = new App({ port: 0 });
        this.app.get("/healthz", () =>
            Date.now() - this.startedAt < this.release.startupMs
                ? { status: 503, body: "starting" }
                : { status: 200, body: "ok" },
        );
        this.app.get("/api/price", async (req) => {
            n++;
            await sleep(this.release.latencyMs * (0.6 + ((n * 7919) % 100) / 125));
            if (((n * 2654435761) >>> 0) % 1000 < this.release.errorRate * 1000)
                return { status: 500, body: "internal error" };
            return {
                sku: req.query.sku,
                price: Number((9.99 * this.release.priceFactor * this.config.currencyRate).toFixed(2)),
                version: this.image,
                pod: this.name,
            };
        });
        this.app.start();
        this.port = this.app.port;
    }
    async probe() {
        try {
            const r = await fetch(`http://127.0.0.1:${this.port}/healthz`);
            await r.text();
            this.okProbes = r.status === 200 ? this.okProbes + 1 : 0;
        } catch {
            this.okProbes = 0;
        }
        this.ready = this.okProbes >= 2;
        return this.ready;
    }
    stop() {
        this.terminating = true;
        this.app.close();
    }
}

// =============================================================================
// Controller
// =============================================================================
const events = [];
const emit = (type, msg) => events.push({ t: Date.now(), type, msg });
class Controller {
    constructor(name) {
        this.name = name;
        this.pods = [];
        this.history = new SortedMap();
        this.revision = 0;
        this.desired = null;
        this.canary = null;
        this.lastGood = null;
        this.analyses = [];
    }
    apply(manifestText) {
        const m = YAML.Parse(manifestText);
        if (!semver.isValid(m.spec.image.split(":")[1])) throw new Error(`image tag must be semver: ${m.spec.image}`);
        const hash = SHA256Hex(JSON.stringify({ image: m.spec.image, config: m.spec.config })).slice(0, 10);
        if (this.desired?.hash === hash && this.desired.replicas === m.spec.replicas) return { unchanged: true };
        const scaleOnly = this.desired?.hash === hash;
        this.desired = { ...m.spec, version: m.spec.image.split(":")[1], hash, text: manifestText };
        if (!scaleOnly) {
            const prev = this.history.get(this.history.lastKey())?.text ?? "";
            const diff = DiffLines(prev, manifestText)
                .filter((h) => h.op !== 0)
                .flatMap((h) =>
                    h.text
                        .lines()
                        .filter((l) => !l.isBlank())
                        .map((l) => `${h.op > 0 ? "+" : "-"} ${l.trim()}`),
                );
            this.revision++;
            this.history.set(this.revision, {
                revision: this.revision,
                version: this.desired.version,
                hash,
                text: manifestText,
                status: "progressing",
                diff,
            });
            emit("RolloutStarted", `revision ${this.revision}: ${this.desired.image} (config ${hash})`);
            if (m.spec.strategy?.canary && this.pods.length)
                this.canary = { steps: m.spec.strategy.canary.steps, step: 0, weight: 0, analysis: [] };
        }
        return { revision: this.revision };
    }
    stable() {
        return this.pods.filter((p) => !p.terminating && p.revision !== this.revision);
    }
    current() {
        return this.pods.filter((p) => !p.terminating && p.revision === this.revision);
    }
    async reconcile() {
        const d = this.desired;
        await Promise.all(this.pods.map((p) => p.probe()));
        if (this.canary) return this.reconcileCanary();
        const cur = this.current(),
            old = this.stable();
        const ready = this.pods.filter((p) => p.ready && !p.terminating).length;
        const maxSurge = d.strategy?.rolling?.maxSurge ?? 1,
            maxUnavailable = d.strategy?.rolling?.maxUnavailable ?? 0;
        if (cur.length < d.replicas && this.pods.length < d.replicas + maxSurge) {
            this.spawn();
            return "surged";
        }
        const readyCur = cur.filter((p) => p.ready);
        if (
            old.length &&
            ready - 1 >= d.replicas - maxUnavailable &&
            (readyCur.length > 0 || this.pods.length >= d.replicas + maxSurge)
        ) {
            this.retire(old[0]);
            return "retired old";
        } // also makes room when surge is blocked
        if (!old.length && cur.length > d.replicas) {
            this.retire(cur[cur.length - 1]);
            return "scaled down";
        }
        if (!old.length && readyCur.length === d.replicas) {
            const h = this.history.get(this.revision);
            if (h.status !== "complete") {
                h.status = "complete";
                this.lastGood = this.revision;
                emit("RolloutComplete", `revision ${this.revision} ${d.version} on ${d.replicas} pods`);
            }
            return "steady";
        }
        return "waiting";
    }
    async reconcileCanary() {
        const c = this.canary,
            step = c.steps[c.step];
        const wantCanary = Math.max(1, Math.round((this.desired.replicas * (step?.setWeight ?? 100)) / 100));
        if (this.current().length < wantCanary) {
            this.spawn();
            return "canary pod";
        }
        if (!this.current().every((p) => p.ready)) return "canary warming";
        c.weight = step.setWeight;
        return "canary at " + c.weight + "%";
    }
    analyse(samples) {
        const c = this.canary,
            can = samples.filter((s) => s.canary),
            st = samples.filter((s) => !s.canary);
        const e1 = st.filter((s) => s.status >= 500).length,
            e2 = can.filter((s) => s.status >= 500).length;
        const p1 = e1 / st.length,
            p2 = e2 / can.length,
            p = (e1 + e2) / (st.length + can.length);
        const z = p > 0 ? (p2 - p1) / Math.sqrt(p * (1 - p) * (1 / st.length + 1 / can.length)) : 0;
        const pval = 0.5 * erfc(z / Math.SQRT2); // one-sided: canary worse
        const digest = (xs) => new TDigest(100).addMany(Float64Array.from(xs, (s) => s.ms));
        const dc = digest(can),
            ds = digest(st);
        const latencyRatio = dc.quantile(0.95) / ds.quantile(0.95);
        const slowShare = 1 - dc.cdf(ds.quantile(0.99)); // canary requests slower than stable's p99
        const pass = !(pval < 0.01 && p2 > p1) && latencyRatio < 1.5;
        const row = {
            version: this.desired.version,
            step: c.step,
            weight: c.weight,
            stable: st.length,
            canary: can.length,
            errStable: p1,
            errCanary: p2,
            p: pval,
            latencyRatio,
            slowShare,
            pass,
        };
        c.analysis.push(row);
        this.analyses.push(row);
        return pass;
    }
    promoteOrAbort(pass) {
        const c = this.canary;
        if (!pass) {
            this.history.get(this.revision).status = "aborted";
            emit("CanaryAborted", `analysis failed at ${c.weight}%: rolling back to revision ${this.lastGood}`);
            for (const p of this.current()) this.retire(p);
            this.canary = null;
            const good = this.history.get(this.lastGood);
            this.revision = this.lastGood;
            this.desired = { ...YAML.Parse(good.text).spec, version: good.version, hash: good.hash, text: good.text };
            return "rolled back";
        }
        c.step++;
        if (c.step >= c.steps.length) {
            emit("CanaryPromoted", `revision ${this.revision} promoted after ${c.analysis.length} analyses`);
            this.canary = null;
            return "promoted";
        }
        return "next step";
    }
    spawn() {
        const p = new Pod(this.desired.version, this.desired.config, this.revision);
        this.pods.push(p);
        emit("PodCreated", `${p.name} (${p.image})`);
    }
    retire(p) {
        p.stop();
        this.pods = this.pods.filter((x) => x !== p);
        emit("PodDeleted", `${p.name}`);
    }
    route() {
        // weighted routing for canaries
        const stable = this.stable().filter((p) => p.ready),
            canary = this.current().filter((p) => p.ready);
        if (this.canary && canary.length && stable.length)
            return Math.random() * 100 < this.canary.weight
                ? [canary[Math.floor(Math.random() * canary.length)], true]
                : [stable[Math.floor(Math.random() * stable.length)], false];
        const all = this.pods.filter((p) => p.ready && !p.terminating);
        return [all[Math.floor(Math.random() * all.length)], false];
    }
}

// =============================================================================
// A load generator that also records availability during rollouts
// =============================================================================
const traffic = { ok: 0, failed: 0, versions: new Map() };
async function load(ctrl, n, record) {
    const samples = [];
    await Promise.all(
        Array.from({ length: n }, async (_, i) => {
            const [pod, isCanary] = ctrl.route();
            if (!pod) {
                traffic.failed++;
                return;
            }
            const t = performance.now();
            try {
                const r = await fetch(`http://127.0.0.1:${pod.port}/api/price?sku=A${i}`);
                const body = await r.text();
                samples.push({ canary: isCanary, status: r.status, ms: performance.now() - t });
                if (r.status === 200) {
                    traffic.ok++;
                    const v = JSON.parse(body).version;
                    traffic.versions.set(v, (traffic.versions.get(v) || 0) + 1);
                } else traffic.failed++;
            } catch {
                traffic.failed++;
                samples.push({ canary: isCanary, status: 599, ms: performance.now() - t });
            }
        }),
    );
    if (record) record.push(...samples);
    return samples;
}
async function converge(ctrl, label, maxLoops = 200) {
    const minReady = [];
    for (let i = 0; i < maxLoops; i++) {
        const action = await ctrl.reconcile();
        minReady.push(ctrl.pods.filter((p) => p.ready && !p.terminating).length);
        if (ctrl.canary && action.startsWith("canary at")) {
            const samples = await load(ctrl, 300);
            const verdict = ctrl.promoteOrAbort(ctrl.analyse(samples));
            if (verdict === "rolled back" || verdict === "promoted") continue;
        } else await load(ctrl, 20);
        if (action === "steady") return { loops: i + 1, minReady: Math.min(...minReady.slice(1)) };
        await sleep(25);
    }
    throw new Error(`${label}: did not converge`);
}

// =============================================================================
// Scenario
// =============================================================================
const manifest = (version, extra = "", replicas = 4, rate = "1.0") => `
apiVersion: apps/v1
kind: Deployment
metadata: { name: price-api }
spec:
  image: registry.example/price-api:${version}
  replicas: ${replicas}
  strategy:
    rolling: { maxSurge: 1, maxUnavailable: 0 }
${extra}
  config: { currencyRate: ${rate} }
`;
const canaryStrategy = `    canary:
      steps: [{ setWeight: 25 }, { setWeight: 50 }, { setWeight: 100 }]`;
const results = {};

const lockPath = work.join("controller.lock");
const leader = new FileLock(lockPath, { retry: 0 });
let standbyBlocked = false;
const ctrl = leader.withLock(() => {
    try {
        new FileLock(lockPath, { retry: 0 }).withLock(() => {});
    } catch {
        standbyBlocked = true;
    }
    return new Controller("price-api");
});

ctrl.apply(manifest("1.4.0"));
const first = await converge(ctrl, "initial");
results["initial deployment: 4 ready pods of 1.4.0"] =
    ctrl.pods.length === 4 && ctrl.pods.every((p) => p.image === "1.4.0" && p.ready);

const before = { ...traffic };
ctrl.apply(manifest("1.5.0"));
const rolling = await converge(ctrl, "rolling to 1.5.0");
results[`rolling update kept >= 4 ready pods throughout (min ${rolling.minReady})`] =
    ctrl.pods.every((p) => p.image === "1.5.0") && rolling.minReady >= 4;
results[`no availability loss during the rollout (${traffic.failed - before.failed} failed requests)`] =
    traffic.failed - before.failed <= 2;

ctrl.apply(manifest("1.5.0", "", 6));
await converge(ctrl, "scale up");
results["a replica change scales without a new revision"] = ctrl.pods.length === 6 && ctrl.revision === 2;
ctrl.apply(manifest("1.5.0", "", 4, "1.08"));
await converge(ctrl, "config change");
const priced = await (await fetch(`http://127.0.0.1:${ctrl.pods[0].port}/api/price?sku=X`)).json();
results["a config change alone triggers a rollout"] =
    ctrl.revision === 3 && ctrl.pods.length === 4 && priced.price === 10.79;

ctrl.apply(manifest("1.6.0", canaryStrategy));
await converge(ctrl, "canary 1.6.0");
const lastAnalysis = events.filter((e) => e.type === "CanaryAborted");
results["the regressed canary is aborted and only 1.5.0 pods remain"] =
    lastAnalysis.length === 1 && ctrl.pods.every((p) => p.image === "1.5.0") && ctrl.pods.length === 4;
const revisions = ctrl.history.rangeQuery(0, Infinity).map(([, h]) => h);
results["history records the aborted revision"] = revisions.find((h) => h.version === "1.6.0").status === "aborted";
results["the config-only revision's diff touches nothing but the currency rate"] =
    revisions[2].diff.length === 2 && revisions[2].diff.all((l) => l.includes("currencyRate"));
results["the bad canary's slow tail shows in the latency sketch"] = ctrl.analyses.some(
    (a) => a.version === "1.6.0" && a.slowShare > 0.3,
);
results["the bad release only ever saw a small share of traffic"] =
    (traffic.versions.get("1.6.0") || 0) < traffic.ok * 0.1;

results["a second controller cannot take the leader lock"] = standbyBlocked;
let badTag = null;
try {
    ctrl.apply(manifest("latest"));
} catch (e) {
    badTag = e.message;
}
results["a non-semver image tag is refused"] = /semver/.test(badTag || "");

print("deployment-controller");
print(`  initial    converged in ${first.loops} loops`);
print(
    `  traffic    ${traffic.ok} ok, ${traffic.failed} failed; by version ${[...traffic.versions].map(([v, n]) => `${v}:${n}`).join(" ")}`,
);
const canaryRows = [];
for (const e of events)
    if (e.type.startsWith("Canary") || e.type.startsWith("Rollout"))
        canaryRows.push([new Date(e.t).toISOString().slice(17, 23), e.type, e.msg]);
print(table([["time", "event", "detail"], ...canaryRows]));
print(
    table([
        ["canary", "weight", "n stable/canary", "errors stable/canary", "p", "p95 ratio", "> stable p99", "verdict"],
        ...ctrl.analyses.map((a) => [
            a.version,
            `${a.weight}%`,
            `${a.stable}/${a.canary}`,
            `${(a.errStable * 100).toFixed(1)}%/${(a.errCanary * 100).toFixed(1)}%`,
            a.p.toExponential(1),
            a.latencyRatio.toFixed(2),
            `${(a.slowShare * 100).toFixed(0)}%`,
            a.pass ? "pass" : "FAIL",
        ]),
    ]),
);
print(
    table([
        ["revision", "version", "config", "status", "diff"],
        ...revisions.map((h) => [
            String(h.revision),
            h.version,
            h.hash,
            h.status,
            h.diff
                .filter((l) => l.startsWith("+"))
                .join(" ")
                .truncate(60),
        ]),
    ]),
);
for (const p of ctrl.pods) p.stop();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
