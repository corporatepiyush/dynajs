import { Logger } from "dyna:log";
import { monotonicNano } from "dyna:time";

const TARGET_MS = 140;
let sink = 0;

function measure(fn) {
    let r = 1, ms = 0;
    for (;;) {
        const t0 = monotonicNano();
        for (let i = 0; i < r; i++) sink += (fn(), 1);
        ms = Number(monotonicNano() - t0) / 1e6;
        if (ms >= TARGET_MS || r >= (1 << 24)) break;
        r = Math.max(r * 2, Math.ceil(r * (TARGET_MS / Math.max(ms, 0.01))));
    }
    let best = ms;
    for (let k = 0; k < 3; k++) {
        const t0 = monotonicNano();
        for (let i = 0; i < r; i++) sink += (fn(), 1);
        const p = Number(monotonicNano() - t0) / 1e6;
        if (p < best) best = p;
    }
    return { ns: (best / r) * 1e6, reps: r };
}

const SHORT_MSG = "request completed";
const LONG_MSG = "request completed for user=" + "9".repeat(400);
const SMALL_FIELDS = { userId: 42, ok: true, ms: 3.5 };
const FLOAT_FIELDS = { price: 12.34, ratio: 0.001, big: 1e21 };
const BIG_FIELDS = (() => {
    const o = {};
    for (let i = 0; i < 40; i++) o["field_number_" + i] = "value-" + i;
    return o;
})();
const ERR = new Error("boom");
ERR.code = "ENOENT";

const log = new Logger({ level: "trace", timestamp: false, name: "bench" });
const isoLog = new Logger({ level: "trace", timestamp: "iso", name: "bench" });
const gated = new Logger({ level: "silent", timestamp: false, name: "bench" });

console.log("case\tdetail\tns\treps");

{
    const r = measure(() => {});
    console.log(`control\tempty-loop\t${r.ns.toFixed(1)}\t${r.reps}`);
}

{
    const r = measure(() => gated.info(SHORT_MSG));
    console.log(`control\tsilent-gate\t${r.ns.toFixed(1)}\t${r.reps}`);
}

for (const [name, l] of [["json", log], ["text", isoLog]]) {
    let r = measure(() => l.info(SHORT_MSG));
    console.log(`emit\t${name}-short-msg\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(LONG_MSG));
    console.log(`emit\t${name}-long-msg\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(SMALL_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-small-fields\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(FLOAT_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-float-fields\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(BIG_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-40-fields\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.error(ERR, SMALL_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-err-fields-msg\t${r.ns.toFixed(1)}\t${r.reps}`);
}

{
    const small = new Logger({ level: "trace", timestamp: false });
    const big = new Logger({ level: "trace", timestamp: false });
    let r = measure(() => small.info("x".repeat(100)));
    console.log(`alloc\tinline-100b\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => big.info("x".repeat(4000)));
    console.log(`alloc\theap-4k\t${r.ns.toFixed(1)}\t${r.reps}`);
}

if (sink === 0)
    throw new Error("bench_log: sink never written -- the loop was elided");
