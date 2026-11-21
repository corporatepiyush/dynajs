// Process scheduler — FCFS, round robin, MLFQ and a CFS-style fair scheduler driven by one discrete-event simulator
// over the same mixed interactive/batch workload, with Linux's nice-to-weight table as the share oracle.
//
// WHAT IT IS
//   The policy core of an OS scheduler, measured the way a kernel developer would:
//
//   simulator   an IndexedHeap of event times (arrivals, I/O completions, quantum expiries) drives one CPU; every
//               policy sees the same processes, bursts and I/O waits
//   FCFS        a Deque; shows the convoy effect (interactive jobs stuck behind batch bursts)
//   RR          a Deque with a fixed quantum; context switches counted
//   MLFQ        three Deques (quanta 2/6/18) with demotion on full-quantum use and a periodic priority boost,
//               so CPU-bound work cannot starve and interactive work stays on top
//   CFS         runnable tasks in an IndexedHeap keyed by virtual runtime; vruntime advances by
//               delta * 1024 / weight using the kernel's sched_prio_to_weight table (kernel/sched/core.c);
//               a waking task is placed at max(own vruntime, min_vruntime - latency) so sleepers cannot hoard
//   checks      work conservation for every policy; CFS CPU shares of competing nice levels match weight ratios;
//               MLFQ and RR beat FCFS on interactive response; no task starves under MLFQ
//
// MODULES  dyna:structures (IndexedHeap, Deque, TDigest, Multiset) · dyna:random · dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/systems/process-scheduler.js

import { IndexedHeap, Deque, TDigest, Multiset } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const WEIGHT = [
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916, 9548, 7620, 6100, 4904, 3906, 3121, 2501,
    1991, 1586, 1277, 1024, 820, 655, 526, 423, 335, 272, 215, 172, 137, 110, 87, 70, 56, 45, 36, 29, 23, 18, 15,
];
const weightOf = (nice) => WEIGHT[nice + 20];

// =============================================================================
// Workload: interactive tasks (short bursts, long I/O) and batch tasks (long bursts)
// =============================================================================
function workload(seed, n = 300) {
    const rng = new Random(seed),
        procs = [];
    let t = 0;
    for (let i = 0; i < n; i++) {
        t += rng.exponential(1 / 55);
        const interactive = rng.nextFloat() < 0.7;
        const bursts = [];
        const k = interactive ? 3 + rng.nextBounded(6) : 1 + rng.nextBounded(3);
        for (let j = 0; j < k; j++) {
            bursts.push(Math.max(1, Math.round(interactive ? rng.exponential(1 / 2) : 20 + rng.exponential(1 / 60))));
            if (j < k - 1)
                bursts.push(
                    -Math.max(1, Math.round(interactive ? 10 + rng.exponential(1 / 30) : rng.exponential(1 / 5))),
                );
        }
        procs.push({ id: i, arrival: Math.round(t), bursts, interactive, nice: interactive ? 0 : [0, 5, 10][i % 3] });
    }
    return procs;
}

// =============================================================================
// Simulator: one CPU, policy-supplied ready queue
// =============================================================================
function simulate(procs, policy) {
    const events = new IndexedHeap();
    const P = procs.map((p) => ({
        ...p,
        step: 0,
        left: p.bursts[0],
        ready: false,
        firstRun: -1,
        readyAt: p.arrival,
        waits: [],
        cpu: 0,
        done: -1,
    }));
    for (const p of P) events.push(p.id, p.arrival);
    let now = 0,
        running = null,
        sliceLen = 0,
        switches = 0,
        busy = 0;
    const sliceId = P.length;
    const schedule = () => {
        if (running) return;
        const next = policy.pick(now);
        if (!next) return;
        running = next;
        if (next.firstRun < 0) next.firstRun = now;
        next.waits.push(now - next.readyAt);
        sliceLen = Math.min(policy.quantum(next), next.left);
        events.push(sliceId, now + sliceLen);
        switches++;
    };
    const makeReady = (p) => {
        p.readyAt = now;
        policy.add(p, now);
    };
    for (let e; (e = events.popEntry());) {
        const [id, t] = e;
        now = t;
        if (id === sliceId) {
            const p = running;
            p.left -= sliceLen;
            p.cpu += sliceLen;
            busy += sliceLen;
            policy.charge(p, sliceLen, now);
            running = null;
            if (p.left === 0) {
                p.step++;
                if (p.step >= p.bursts.length) p.done = now;
                else {
                    events.push(p.id, now - p.bursts[p.step]);
                    p.step++;
                    p.left = p.bursts[p.step];
                }
            } else makeReady(p);
        } else makeReady(P[id]);
        schedule();
    }
    const total = (p) => p.bursts.filter((b) => b > 0).sum();
    return { P, switches, busy, end: now, conserved: P.every((p) => p.cpu === total(p) && p.done >= 0) };
}

// =============================================================================
// Policies
// =============================================================================
const fcfs = () => {
    const q = new Deque();
    return { name: "FCFS", add: (p) => q.pushBack(p), pick: () => q.popFront(), quantum: () => Infinity, charge() {} };
};
const roundRobin = (quantum) => {
    const q = new Deque();
    return {
        name: `RR q=${quantum}`,
        add: (p) => q.pushBack(p),
        pick: () => q.popFront(),
        quantum: () => quantum,
        charge() {},
    };
};
function mlfq() {
    const QUANTA = [2, 6, 18],
        queues = QUANTA.map(() => new Deque()),
        level = new Map(),
        BOOST = 400;
    let lastBoost = 0;
    return {
        name: "MLFQ",
        add(p, now) {
            if (now - lastBoost >= BOOST) {
                lastBoost = now;
                for (let l = 1; l < queues.length; l++) {
                    let x;
                    while ((x = queues[l].popFront()) !== undefined) {
                        level.set(x.id, 0);
                        queues[0].pushBack(x);
                    }
                }
                for (const k of level.keys()) level.set(k, 0);
            }
            queues[level.get(p.id) ?? 0].pushBack(p);
        },
        pick() {
            for (const q of queues) {
                const p = q.popFront();
                if (p) return p;
            }
            return undefined;
        },
        quantum: (p) => QUANTA[level.get(p.id) ?? 0],
        charge(p, used) {
            const l = level.get(p.id) ?? 0;
            if (used >= QUANTA[l] && l < QUANTA.length - 1) level.set(p.id, l + 1);
        },
    };
}
function cfs({ latency = 24, minGranularity = 3 } = {}) {
    const heap = new IndexedHeap(),
        vr = new Map(),
        byId = new Map();
    let minV = 0,
        runnableCount = 0;
    return {
        name: "CFS",
        vruntime: vr,
        add(p) {
            const placed = Math.max(vr.get(p.id) ?? minV, minV - latency / 2);
            vr.set(p.id, placed);
            byId.set(p.id, p);
            heap.push(p.id, placed);
            runnableCount++;
        },
        pick() {
            const id = heap.pop();
            if (id === undefined) return undefined;
            runnableCount--;
            minV = Math.max(minV, vr.get(id));
            return byId.get(id);
        },
        quantum(p) {
            return Math.max(
                minGranularity,
                Math.round((latency * weightOf(p.nice)) / (weightOf(p.nice) + 1024 * Math.max(1, this.runnable()))),
            );
        },
        runnable: () => runnableCount,
        charge(p, used) {
            vr.set(p.id, vr.get(p.id) + (used * 1024) / weightOf(p.nice));
        },
    };
}

// =============================================================================
// The comparison
// =============================================================================
const procs = workload(2026);
const policies = [fcfs(), roundRobin(4), mlfq(), cfs()];
const table = [],
    runs = {};
for (const pol of policies) {
    const r = simulate(procs, pol);
    runs[pol.name] = r;
    const inter = r.P.filter((p) => p.interactive),
        batch = r.P.filter((p) => !p.interactive);
    const resp = new TDigest(100);
    for (const p of inter) for (const w of p.waits) resp.add(w);
    const turnaround = (ps) => ps.map((p) => p.done - p.arrival).average();
    table.push([
        pol.name,
        r.conserved ? "yes" : "NO",
        r.switches.format(),
        resp.quantile(0.5).toFixed(1),
        resp.quantile(0.99).toFixed(1),
        turnaround(inter).toFixed(1),
        turnaround(batch).toFixed(1),
        ((100 * r.busy) / r.end).toFixed(1) + "%",
    ]);
    r.p99 = resp.quantile(0.99);
    r.p50 = resp.quantile(0.5);
    r.interTurn = turnaround(inter);
    r.batchTurn = turnaround(batch);
    r.worstWait = Math.max(...r.P.map((p) => Math.max(0, ...p.waits)));
}
print(
    Table(table, {
        head: [
            "policy",
            "work conserved",
            "switches",
            "inter p50 wait",
            "inter p99 wait",
            "inter turnaround",
            "batch turnaround",
            "CPU busy",
        ],
    }),
);
results["every policy conserves work and finishes every task"] = Object.values(runs).every((r) => r.conserved);
results["every policy keeps the CPU equally busy (same total work)"] =
    new Set(Object.values(runs).map((r) => r.busy)).size === 1;
results["FCFS shows the convoy effect on interactive turnaround"] =
    runs.FCFS.interTurn > runs["RR q=4"].interTurn && runs.FCFS.interTurn > runs.MLFQ.interTurn;
results["MLFQ beats RR and FCFS on the interactive median wait"] =
    runs.MLFQ.p50 < runs["RR q=4"].p50 && runs.MLFQ.p50 < runs.FCFS.p50;
results["MLFQ and RR cut the FCFS interactive tail by 4x"] =
    runs.MLFQ.p99 * 4 < runs.FCFS.p99 && runs["RR q=4"].p99 * 4 < runs.FCFS.p99;
results["CFS has the lowest interactive tail"] = Object.values(runs).every((r) => runs.CFS.p99 <= r.p99);
results["RR switches more than FCFS"] = runs["RR q=4"].switches > runs.FCFS.switches;
results["priority boost bounds the worst MLFQ wait"] = runs.MLFQ.worstWait < 2000;

// =============================================================================
// CFS shares: three CPU hogs at nice -5, 0 and 5 for 30 000 ticks
// =============================================================================
{
    const hogs = [-5, 0, 5].map((nice, i) => ({ id: i, arrival: 0, bursts: [30_000], interactive: false, nice }));
    const pol = cfs({ latency: 12, minGranularity: 1 });
    const cpu = new Multiset();
    const original = pol.charge;
    let elapsed = 0;
    pol.charge = function (p, used, now) {
        if (elapsed < 30_000) cpu.add(String(p.nice), Math.min(used, 30_000 - elapsed));
        elapsed += used;
        original.call(this, p, used, now);
    };
    simulate(hogs, pol);
    const total = [-5, 0, 5].map((n) => cpu.count(String(n))).sum();
    const wsum = [-5, 0, 5].map(weightOf).sum();
    const shares = [-5, 0, 5].map((n) => ({ nice: n, got: cpu.count(String(n)) / total, want: weightOf(n) / wsum }));
    print(
        Table(
            shares.map((s) => [s.nice, (100 * s.got).toFixed(2) + "%", (100 * s.want).toFixed(2) + "%"]),
            { head: ["nice", "CPU share", "weight share"] },
        ),
    );
    results["CFS shares match sched_prio_to_weight within 1 point"] = shares.every(
        (s) => Math.abs(s.got - s.want) < 0.01,
    );
    results["nice -5 gets about 9.5x the CPU of nice 5 (3121 / 335)"] =
        Math.abs(cpu.count("-5") / cpu.count("5") / (3121 / 335) - 1) < 0.1;
}

// =============================================================================
// CFS sleeper placement: a task waking from a long sleep cannot monopolise the CPU
// =============================================================================
{
    const pol = cfs({ latency: 12, minGranularity: 1 });
    const procs2 = [
        { id: 0, arrival: 0, bursts: [5000], interactive: false, nice: 0 },
        { id: 1, arrival: 0, bursts: [1, -3000, 2000], interactive: true, nice: 0 },
    ];
    const r = simulate(procs2, pol);
    const sleeper = r.P[1];
    const longest = Math.max(...sleeper.waits);
    results["a woken sleeper shares the CPU instead of running its whole burst"] =
        r.conserved && sleeper.done - 3001 > 2000 + 500;
    results["the CPU hog is not starved by the sleeper"] = longest < 50 && r.P[0].done < 7100;
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
