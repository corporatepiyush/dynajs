// flags: --std
// Job orchestrator — DAG workflows with cron schedules, backfill, a worker-thread pool, per-pool concurrency,
// retries with exponential backoff and jitter, timeouts, result passing, crash recovery and SLA tracking.
//
// WHAT IT IS
//   A small Airflow/Temporal: workflows are declared in YAML, executed on real OS threads, and survive a crash.
//
//   definitions  a workflow is a DAG of tasks with dependencies, a pool, a retry policy, a timeout and a trigger
//                rule (all_success / all_done / one_failed for cleanup and alert tasks); cycles and unknown
//                dependencies are rejected at load time with the offending path
//   scheduling   each workflow has an RRULE schedule; on start the scheduler computes every run that SHOULD have
//                happened since the last recorded one and backfills them (catch-up), oldest first
//   execution    ready tasks are dispatched to os.Worker threads (real parallelism) through per-pool slots, e.g.
//                at most 2 tasks in the "db" pool at a time; a task's output (XCom) is passed to its dependants
//   retries      failures retry with exponential backoff and full jitter; a task exceeding its timeout is
//                abandoned and counted as a failure; an idempotency key per (run, task, attempt) lets a re-delivered
//                task be recognised
//   durability   every state transition is written to SQLite in a transaction; after a simulated crash the
//                orchestrator restarts, finds RUNNING tasks of dead workers, and re-queues them — no task that
//                had succeeded runs twice
//   SLAs         a run finishing past its SLA is flagged; the report shows durations and the critical path — the
//                longest chain of task durations through the DAG, found as a shortest path with NEGATED durations
//                by the native Bellman-Ford (exact on a DAG, which has no negative cycles)
//
// MODULES  os (Worker) · dyna:yaml · dyna:time (RRule) · dyna:net (SQLite) · dyna:structures (Graph)
//          · dyna:random · dyna:hash · dyna:cli (Table) · dyna:file · dyna:async (sleep)
//
// RUN      dynajs --std examples/ops/job-orchestrator.js     loads two workflows, backfills three days, crashes once and resumes

import * as os from "os";
import * as YAML from "dyna:yaml";
import { RRule } from "dyna:time";
import { SQLite } from "dyna:net";
import { Graph } from "dyna:structures";
import { Random } from "dyna:random";
import { XXH3_64 } from "dyna:hash";
import { Table } from "dyna:cli";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
import { sleep } from "dyna:async";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("orchestrator");

// =============================================================================
// Task implementations run inside worker threads
// =============================================================================
const workerPath = work.join("task-worker.js");
writeFile(
    workerPath,
    `
import * as os from "os";
import { SHA256Hex } from "dyna:hash";
import { gzip } from "dyna:compress";
import { Random } from "dyna:random";
const parent = os.Worker.parent;
const busy = (ms) => { const end = Date.now() + ms; let x = 0; while (Date.now() < end) x++; return x; };
const tasks = {
    extract: (a) => { const rng = new Random(a.seed); const rows = Array.from({ length: a.rows }, (_, i) => ({ id: i, amount: rng.nextBounded(10000) / 100, region: ["eu", "us", "apac"][rng.nextBounded(3)] })); busy(a.ms); return { rows: rows.length, total: rows.reduce((s, r) => s + r.amount, 0), sample: rows.slice(0, 3) }; },
    transform: (a, inputs) => { busy(a.ms); const total = Object.values(inputs).reduce((s, x) => s + (x.total || 0), 0); return { total: Math.round(total * 100) / 100, sources: Object.keys(inputs).length }; },
    load: (a, inputs) => { busy(a.ms); return { loaded: Object.values(inputs)[0].total, digest: SHA256Hex(JSON.stringify(inputs)).slice(0, 12) }; },
    compress: (a) => { const rng = new Random(a.seed); const data = rng.bytes(a.bytes); for (let i = 0; i < data.length; i += 3) data[i] = 0; return { in: data.length, out: gzip(data).length }; },
    flaky: (a) => { busy(a.ms); if (a.attempt <= a.failUntil) throw new Error("upstream 503 (attempt " + a.attempt + ")"); return { ok: true, attempts: a.attempt }; },
    slow: (a) => { busy(a.ms); return { done: true }; },
    notify: (a, inputs) => ({ sent: true, about: Object.keys(inputs) }),
};
parent.onmessage = (e) => {
    const m = e.data;
    if (m.type === "stop") { parent.onmessage = null; return; }
    try { parent.postMessage({ type: "done", id: m.id, output: tasks[m.task](m.args, m.inputs) }); }
    catch (err) { parent.postMessage({ type: "failed", id: m.id, error: String(err.message || err) }); }
};
`,
);

// =============================================================================
// Definitions
// =============================================================================
const DEFS = YAML.Parse(`
- name: nightly-sales
  schedule: { freq: DAILY, dtstart: "2026-03-01T02:00:00Z" }
  slaMinutes: 1
  tasks:
    extract_eu:   { run: extract, args: { seed: 1, rows: 3000, ms: 40 }, pool: db }
    extract_us:   { run: extract, args: { seed: 2, rows: 3000, ms: 40 }, pool: db }
    extract_apac: { run: extract, args: { seed: 3, rows: 3000, ms: 40 }, pool: db }
    fx_rates:     { run: flaky, args: { ms: 10, failUntil: 2 }, retries: 4, backoffMs: 20 }
    transform:    { run: transform, args: { ms: 30 }, after: [extract_eu, extract_us, extract_apac, fx_rates] }
    archive:      { run: compress, args: { seed: 9, bytes: 400000 }, after: [extract_eu] }
    load:         { run: load, args: { ms: 20 }, after: [transform], pool: db }
    alert:        { run: notify, after: [load, archive], trigger: one_failed }
- name: hourly-reindex
  schedule: { freq: HOURLY, interval: 8, dtstart: "2026-03-01T00:00:00Z" }
  slaMinutes: 0.005
  tasks:
    snapshot:  { run: slow, args: { ms: 30 } }
    reindex:   { run: slow, args: { ms: 400 }, timeoutMs: 150, retries: 1, backoffMs: 10, after: [snapshot] }
    cleanup:   { run: notify, after: [reindex], trigger: all_done }
`);
const POOLS = { default: 4, db: 2 };
function validate(def) {
    const names = Object.keys(def.tasks),
        g = new Graph({ directed: true }),
        ids = new Map(names.map((n) => [n, g.addNode()]));
    for (const [n, t] of Object.entries(def.tasks))
        for (const d of t.after || []) {
            if (!ids.has(d)) throw new Error(`${def.name}.${n} depends on unknown task ${d}`);
            g.addEdge(ids.get(d), ids.get(n));
        }
    const order = g.topologicalSort();
    if (!order || order.length !== names.length) throw new Error(`${def.name} has a dependency cycle`);
    return order.map((i) => names[i]);
}
for (const d of DEFS) d.order = validate(d);
let cycleError = null;
try {
    validate({ name: "broken", tasks: { a: { after: ["c"] }, b: { after: ["a"] }, c: { after: ["b"] } } });
} catch (e) {
    cycleError = e.message;
}

// =============================================================================
// Durable state
// =============================================================================
const dbPath = work.join("state.db");
function openDb() {
    const db = new SQLite(dbPath);
    db.exec(
        "CREATE TABLE IF NOT EXISTS runs (id TEXT PRIMARY KEY, workflow TEXT, logical INTEGER, state TEXT, started INTEGER, finished INTEGER)",
    );
    db.exec(
        "CREATE TABLE IF NOT EXISTS tasks (run TEXT, task TEXT, state TEXT, attempt INTEGER, not_before INTEGER, output TEXT, error TEXT, idem TEXT, started INTEGER, finished INTEGER, PRIMARY KEY (run, task))",
    );
    db.exec("CREATE TABLE IF NOT EXISTS executions (idem TEXT PRIMARY KEY, run TEXT, task TEXT, at INTEGER)");
    return db;
}

// =============================================================================
// Orchestrator
// =============================================================================
class Orchestrator {
    constructor(threads, now, { crashAfter = Infinity } = {}) {
        this.db = openDb();
        this.now = now;
        this.crashAfter = crashAfter;
        this.completions = 0;
        this.rng = new Random(11);
        this.workers = Array.from({ length: threads }, () => ({ w: new os.Worker(workerPath.toString()), busy: null }));
        this.inflight = new Map();
        this.poolUse = { default: 0, db: 0 };
        for (const slot of this.workers) slot.w.onmessage = (e) => this.onResult(slot, e.data);
    }
    tx(fn) {
        this.db.exec("BEGIN");
        try {
            const r = fn();
            this.db.exec("COMMIT");
            return r;
        } catch (e) {
            this.db.exec("ROLLBACK");
            throw e;
        }
    }
    schedule() {
        let created = 0;
        for (const def of DEFS) {
            const last = this.db.query("SELECT MAX(logical) AS t FROM runs WHERE workflow = ?", [def.name])[0].t;
            const rule = new RRule({ ...def.schedule, until: new Date(this.now) });
            for (const d of rule.all()) {
                const t = d.getTime();
                if (last !== null && t <= last) continue;
                const id = `${def.name}@${d.toISOString().slice(0, 16)}`;
                this.tx(() => {
                    this.db.exec("INSERT INTO runs VALUES (?, ?, ?, 'running', ?, NULL)", [
                        id,
                        def.name,
                        t,
                        Date.now(),
                    ]);
                    for (const name of def.order)
                        this.db.exec("INSERT INTO tasks VALUES (?, ?, 'pending', 0, 0, NULL, NULL, NULL, NULL, NULL)", [
                            id,
                            name,
                        ]);
                });
                created++;
            }
        }
        return created;
    }
    recover() {
        // an attempt that never reported is re-delivered, not re-counted
        const orphans = this.db.query("SELECT run, task FROM tasks WHERE state = 'running'");
        this.tx(() => {
            for (const o of orphans)
                this.db.exec("UPDATE tasks SET state = 'pending', attempt = attempt - 1 WHERE run = ? AND task = ?", [
                    o.run,
                    o.task,
                ]);
        });
        return orphans.length;
    }
    ready() {
        const out = [];
        for (const run of this.db.query("SELECT * FROM runs WHERE state = 'running' ORDER BY logical")) {
            const def = DEFS.find((d) => d.name === run.workflow);
            const states = new Map(
                this.db.query("SELECT task, state FROM tasks WHERE run = ?", [run.id]).map((r) => [r.task, r.state]),
            );
            for (const name of def.order) {
                if (states.get(name) !== "pending") continue;
                const t = def.tasks[name],
                    deps = (t.after || []).map((d) => states.get(d));
                if (deps.some((s) => s === "pending" || s === "running" || s === "retrying")) continue;
                const trigger = t.trigger || "all_success";
                const fire =
                    trigger === "all_success"
                        ? deps.every((s) => s === "success")
                        : trigger === "all_done"
                          ? true
                          : deps.some((s) => s === "failed" || s === "upstream_failed");
                if (!fire) {
                    this.db.exec("UPDATE tasks SET state = ? WHERE run = ? AND task = ?", [
                        trigger === "one_failed" ? "skipped" : "upstream_failed",
                        run.id,
                        name,
                    ]);
                    continue;
                }
                out.push({ run, def, name, t });
            }
            for (const r of this.db.query(
                "SELECT task FROM tasks WHERE run = ? AND state = 'retrying' AND not_before <= ?",
                [run.id, Date.now()],
            ))
                this.db.exec("UPDATE tasks SET state = 'pending' WHERE run = ? AND task = ?", [run.id, r.task]);
        }
        return out;
    }
    dispatch() {
        for (const job of this.ready()) {
            const pool = job.t.pool || "default",
                slot = this.workers.find((s) => !s.busy);
            if (!slot || this.poolUse[pool] >= POOLS[pool]) continue;
            const row = this.db.query("SELECT attempt FROM tasks WHERE run = ? AND task = ?", [
                job.run.id,
                job.name,
            ])[0];
            const attempt = row.attempt + 1,
                idem = XXH3_64(`${job.run.id}|${job.name}|${attempt}`, 0, { as: "hex" });
            const inputs = {};
            for (const d of job.t.after || []) {
                const o = this.db.query("SELECT output FROM tasks WHERE run = ? AND task = ?", [job.run.id, d])[0]
                    .output;
                if (o) inputs[d] = JSON.parse(o);
            }
            this.db.exec(
                "UPDATE tasks SET state = 'running', attempt = ?, idem = ?, started = ? WHERE run = ? AND task = ?",
                [attempt, idem, Date.now(), job.run.id, job.name],
            );
            const id = `${job.run.id}/${job.name}`;
            slot.busy = id;
            this.poolUse[pool]++;
            const timer = job.t.timeoutMs ? setTimeout(() => this.onTimeout(slot, id), job.t.timeoutMs) : null;
            this.inflight.set(id, { job, slot, pool, attempt, idem, timer });
            slot.w.postMessage({ id, task: job.t.run, args: { ...(job.t.args || {}), attempt }, inputs });
        }
    }
    onTimeout(slot, id) {
        const f = this.inflight.get(id);
        if (!f) return;
        slot.w.onmessage = null;
        slot.w.postMessage({ type: "stop" }); // abandon the thread, start a fresh one
        slot.w = new os.Worker(workerPath.toString());
        slot.w.onmessage = (e) => this.onResult(slot, e.data);
        this.finish(f, null, `timed out after ${f.job.t.timeoutMs} ms`);
    }
    onResult(slot, m) {
        const f = this.inflight.get(m.id);
        if (!f) return;
        this.finish(f, m.type === "done" ? m.output : null, m.type === "failed" ? m.error : null);
    }
    finish(f, output, error) {
        clearTimeout(f.timer);
        this.inflight.delete(`${f.job.run.id}/${f.job.name}`);
        f.slot.busy = null;
        this.poolUse[f.pool]--;
        if (++this.completions > this.crashAfter) {
            this.crashed = true;
            return;
        } // the process "dies" before recording
        const { run, name, t } = f.job;
        this.tx(() => {
            if (error === null) {
                const dup = this.db.query("SELECT 1 FROM executions WHERE idem = ?", [f.idem]).length;
                if (!dup)
                    this.db.exec("INSERT INTO executions VALUES (?, ?, ?, ?)", [f.idem, run.id, name, Date.now()]);
                this.db.exec(
                    "UPDATE tasks SET state = 'success', output = ?, finished = ? WHERE run = ? AND task = ?",
                    [JSON.stringify(output), Date.now(), run.id, name],
                );
            } else if (f.attempt <= (t.retries || 0)) {
                const backoff = this.rng.nextBounded((t.backoffMs || 50) * 2 ** (f.attempt - 1) + 1); // full jitter
                this.db.exec(
                    "UPDATE tasks SET state = 'retrying', error = ?, not_before = ? WHERE run = ? AND task = ?",
                    [error, Date.now() + backoff, run.id, name],
                );
            } else
                this.db.exec("UPDATE tasks SET state = 'failed', error = ?, finished = ? WHERE run = ? AND task = ?", [
                    error,
                    Date.now(),
                    run.id,
                    name,
                ]);
            const open = this.db.query(
                "SELECT COUNT(*) AS n FROM tasks WHERE run = ? AND state IN ('pending', 'running', 'retrying')",
                [run.id],
            )[0].n;
            if (!open) {
                const failed = this.db.query(
                    "SELECT COUNT(*) AS n FROM tasks WHERE run = ? AND state IN ('failed', 'upstream_failed')",
                    [run.id],
                )[0].n;
                this.db.exec("UPDATE runs SET state = ?, finished = ? WHERE id = ?", [
                    failed ? "failed" : "success",
                    Date.now(),
                    run.id,
                ]);
            }
        });
    }
    closeRuns() {
        // a run whose last tasks were skipped finishes without a completion event
        for (const run of this.db.query("SELECT id FROM runs WHERE state = 'running'")) {
            if (
                this.db.query(
                    "SELECT COUNT(*) AS n FROM tasks WHERE run = ? AND state IN ('pending', 'running', 'retrying')",
                    [run.id],
                )[0].n
            )
                continue;
            const failed = this.db.query(
                "SELECT COUNT(*) AS n FROM tasks WHERE run = ? AND state IN ('failed', 'upstream_failed')",
                [run.id],
            )[0].n;
            this.db.exec("UPDATE runs SET state = ?, finished = ? WHERE id = ?", [
                failed ? "failed" : "success",
                Date.now(),
                run.id,
            ]);
        }
    }
    async runUntilIdle(maxMs = 20000) {
        const deadline = Date.now() + maxMs;
        while (Date.now() < deadline && !this.crashed) {
            this.dispatch();
            this.closeRuns();
            const open = this.db.query(
                "SELECT COUNT(*) AS n FROM tasks WHERE state IN ('pending', 'running', 'retrying')",
            )[0].n;
            if (!open && !this.inflight.size) return true;
            await sleep(5);
        }
        return false;
    }
    shutdown() {
        for (const s of this.workers) {
            s.w.onmessage = null;
            s.w.postMessage({ type: "stop" });
        }
        for (const f of this.inflight.values()) clearTimeout(f.timer);
        this.db.close();
    }
}

// Critical path of a finished run: longest duration chain = shortest path with negated durations from a super-source.
function criticalPath(db, run) {
    const def = DEFS.find((d) => d.name === run.workflow),
        names = Object.keys(def.tasks);
    const dur = new Map(
        db
            .query("SELECT task, started, finished FROM tasks WHERE run = ?", [run.id])
            .map((t) => [t.task, Math.max(0, (t.finished ?? t.started ?? 0) - (t.started ?? 0))]),
    );
    const g = new Graph({ directed: true, weighted: true }),
        src = g.addNode(),
        id = new Map(names.map((n) => [n, g.addNode()]));
    for (const n of names) {
        const deps = def.tasks[n].after || [];
        if (!deps.length) g.addEdge(src, id.get(n), -dur.get(n));
        for (const d of deps) g.addEdge(id.get(d), id.get(n), -dur.get(n));
    }
    const dist = g.bellmanFord(src);
    let end = names.reduce((a, b) => (dist[id.get(b)] < dist[id.get(a)] ? b : a));
    const total = -dist[id.get(end)],
        path = [end];
    while ((def.tasks[end].after || []).length) {
        // walk back along a tight edge
        end = def.tasks[end].after.find((d) => Math.abs(dist[id.get(d)] - dur.get(end) - dist[id.get(end)]) < 1e-9);
        path.unshift(end);
    }
    return { path, total };
}

// =============================================================================
// Scenario: backfill three days, crash mid-way, restart, finish
// =============================================================================
const results = {};
const NOW = Date.UTC(2026, 2, 3, 12);
const started = Date.now();
const first = new Orchestrator(4, NOW, { crashAfter: 14 });
const created = first.schedule();
await first.runUntilIdle();
const crashState = first.db
    .query("SELECT state, COUNT(*) AS n FROM tasks GROUP BY state")
    .map((r) => `${r.state}:${r.n}`)
    .join(" ");
const successBefore = new Set(
    first.db.query("SELECT run || '/' || task AS k FROM tasks WHERE state = 'success'").map((r) => r.k),
);
first.shutdown();

const second = new Orchestrator(4, NOW);
const recovered = second.recover();
const createdAgain = second.schedule();
const finished = await second.runUntilIdle();
const db = second.db;
const runs = db.query("SELECT * FROM runs ORDER BY logical");
const tasks = db.query("SELECT * FROM tasks");
const execs = db.query("SELECT run || '/' || task AS k, COUNT(*) AS n FROM executions GROUP BY k");

results["a cyclic workflow is rejected at load time"] = /cycle/.test(cycleError || "");
results[`backfill created ${created} runs (3 daily + 8 eight-hourly) and the restart created none twice`] =
    created === 3 + 8 && createdAgain === 0;
results[`after the crash ${recovered} orphaned RUNNING task(s) were re-queued and everything completed`] =
    recovered > 0 && finished;
results["no task that had succeeded ran twice"] =
    execs.every((e) => e.n === 1) &&
    [...successBefore].every((k) => tasks.find((t) => `${t.run}/${t.task}` === k).state === "success");
const sales = runs.filter((r) => r.workflow === "nightly-sales");
results["every nightly-sales run succeeded"] = sales.every((r) => r.state === "success");
const fx = tasks.filter((t) => t.task === "fx_rates");
results["the flaky task succeeded on its third attempt with backoff"] = fx.every(
    (t) => t.state === "success" && t.attempt === 3 && JSON.parse(t.output).attempts === 3,
);
const tf = tasks.find((t) => t.task === "transform" && t.state === "success");
results["outputs flow downstream (XCom)"] =
    JSON.parse(tf.output).sources === 4 &&
    JSON.parse(tasks.find((t) => t.run === tf.run && t.task === "load").output).loaded === JSON.parse(tf.output).total;
results["the one_failed alert task is skipped when nothing failed"] = tasks
    .filter((t) => t.task === "alert")
    .every((t) => t.state === "skipped");
const reindex = runs.filter((r) => r.workflow === "hourly-reindex");
results["the slow reindex times out, retries once, and fails its run"] =
    reindex.every((r) => r.state === "failed") &&
    tasks.filter((t) => t.task === "reindex").every((t) => /timed out/.test(t.error) && t.attempt === 2);
results["an all_done cleanup still runs after a failure"] = tasks
    .filter((t) => t.task === "cleanup")
    .every((t) => t.state === "success");
const order = runs.map((r) => r.logical);
const slaMs = (r) => DEFS.find((d) => d.name === r.workflow).slaMinutes * 60_000;
const late = runs.filter((r) => r.finished - r.started > slaMs(r));
results[`SLA misses are exactly the reindex runs (${late.length})`] =
    late.length === reindex.length && late.every((r) => r.workflow === "hourly-reindex");
const cp = criticalPath(db, sales.last());
const lastSales = sales.last(),
    salesDef = DEFS.find((d) => d.name === "nightly-sales");
results[
    `the critical path (${cp.path.join(" > ")}) is a dependency chain no shorter than any task and no longer than the run`
] =
    cp.path.every((n, i) => i === 0 || (salesDef.tasks[n].after || []).includes(cp.path[i - 1])) &&
    tasks.filter((t) => t.run === lastSales.id).every((t) => (t.finished ?? 0) - (t.started ?? 0) <= cp.total) &&
    cp.total <= lastSales.finished - lastSales.started;
results["backfill runs execute oldest first"] = order.every((t, i) => i === 0 || t >= order[i - 1]);

print("job-orchestrator");
print(
    `  workers    4 OS threads; pools ${Object.entries(POOLS)
        .map(([k, v]) => `${k}=${v}`)
        .join(", ")}`,
);
print(`  crash      after 14 completions: ${crashState}; restart re-queued ${recovered}`);
print(
    table([
        ["run", "state", "tasks", "duration", "SLA"],
        ...runs.map((r) => {
            const ts = tasks.filter((t) => t.run === r.id);
            return [
                r.id,
                r.state,
                ts
                    .map(
                        (t) =>
                            `${t.task}:${{ success: "ok", failed: "FAIL", skipped: "skip", upstream_failed: "up-fail" }[t.state] || t.state}${t.attempt > 1 ? `(x${t.attempt})` : ""}`,
                    )
                    .join(" "),
                `${r.finished - r.started} ms`,
                r.finished - r.started > slaMs(r) ? "MISSED" : "ok",
            ];
        }),
    ]),
);
print(`  critical   ${sales.last().id}: ${cp.path.join(" > ")} = ${cp.total} ms`);
print(`  total      ${Date.now() - started} ms wall, ${execs.length} task executions recorded`);
second.shutdown();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
