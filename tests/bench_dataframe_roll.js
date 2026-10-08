import { DataFrame } from "dyna:dataframe";

const N = 20000;
const f = new Float64Array(N), q = new Int32Array(N);
for (let i = 0; i < N; i++) {
    f[i] = ((i * 37) % 1021) - 510 + i * 0.125;
    q[i] = ((i * 37) % 1021) - 510;
}
const df = new DataFrame({ f, q });
const mask = new Uint8Array(N);
for (let i = 0; i < N; i++) mask[i] = i % 3 ? 1 : 0;

let sink = 0;

function time(label, fn, reps) {
    fn();
    let best = Infinity;
    for (let t = 0; t < 5; t++) {
        const t0 = performance.now();
        for (let r = 0; r < reps; r++) sink += fn()[N - 1];
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    const per = best / reps;
    console.log("  " + label.padEnd(34) + per.toFixed(4) + " ms/call");
    return per;
}

const ROWS = [];
function row(label, fn, reps) { ROWS.push([label, time(label, fn, reps)]); }

console.log("rows=" + N);
for (const w of [16, 256]) {
    console.log("window=" + w);
    row("CONTROL ROLLING_MEAN f64 w=" + w, () => df.ROLLING_MEAN("f", w), w > 64 ? 3 : 20);
    row("CONTROL ROLLING_VAR  f64 w=" + w, () => df.ROLLING_VAR("f", w), w > 64 ? 2 : 12);
    row("        ROLLING_VAR  i32 w=" + w, () => df.ROLLING_VAR("q", w), w > 64 ? 2 : 12);
    row("        ROLLING_STD  i32 w=" + w, () => df.ROLLING_STD("q", w), w > 64 ? 2 : 12);
    row("        ROLLING_VAR  i32 masked w=" + w, () => df.ROLLING_VAR("q", w, mask), w > 64 ? 2 : 12);
}

console.log("--- BENCHDATA ---");
for (const [label, per] of ROWS) console.log("BENCH\t" + label.trim() + "\t" + per.toFixed(6));
if (!Number.isFinite(sink)) throw new Error("sink went non-finite: the loop was folded away");
