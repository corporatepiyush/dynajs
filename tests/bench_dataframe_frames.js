import { DataFrame } from "dyna:dataframe";

const SINK = { rows: 0, n: 0, cells: 0 };

function time(fn, reps) {
    const t0 = performance.now();
    for (let r = 0; r < reps; r++) fn();
    return (performance.now() - t0) / reps;
}

function row(name, n, ms) {
    SINK.rows += ms;
    console.log("row " + name + " " + n + " " + ms.toFixed(4) +
                " " + ((ms * 1e6) / n).toFixed(2));
}

function frameAt(n) {
    const id = new Int32Array(n), v = new Float64Array(n);
    const g = new Array(n);
    for (let i = 0; i < n; i++) {
        id[i] = i % 1000;
        v[i] = (i * 37) % 1021;
        g[i] = "g" + (i % 50);
    }
    return new DataFrame({ id, v, g });
}

const N = 200000;
const df = frameAt(N);
const other = new DataFrame({
    id: Int32Array.from([0, 250, 500, 750, 999]),
    code: Int32Array.from([10, 20, 30, 40, 50]),
});
const otherSame = new DataFrame({
    id: new Int32Array(N).fill(0),
    v: new Float64Array(N).fill(0),
    g: new Array(N).fill("g0"),
});
const mask = new Uint8Array(N);
for (let i = 0; i < N; i++) mask[i] = i % 2 ? 1 : 0;
const half = [];
for (let i = 0; i < N; i += 2) half.push(i);

df.SUM("v"); df.COPY(); df.FILTER(mask); df.JOIN(other, "id", "id");
df.PIVOT("id", "g", "v"); df.TO_RECORDS();

const REPS = 5;

console.log("row name n ms ns_per_row");
row("CONTROL_SUM", N, time(() => { SINK.n += df.SUM("v"); }, REPS));
row("CONTROL_COPY", N, time(() => { SINK.n += df.COPY().ROWS; }, REPS));
row("SELECT", N, time(() => { SINK.n += df.SELECT(["id", "v"]).ROWS; }, REPS));
row("DROP_COLUMNS", N, time(() => { SINK.n += df.DROP_COLUMNS(["g"]).ROWS; }, REPS));
row("RENAME", N, time(() => { SINK.n += df.RENAME({ v: "vv" }).ROWS; }, REPS));
row("FILTER", N, time(() => { SINK.n += df.FILTER(mask).ROWS; }, REPS));
row("ISIN", N, time(() => { SINK.n += df.ISIN("id", half).length; }, REPS));
row("SLICE", N, time(() => { SINK.n += df.SLICE(0, N).ROWS; }, REPS));
row("SAMPLE", N, time(() => { SINK.n += df.SAMPLE(1000, 42).ROWS; }, REPS));
row("MASK", N, time(() => { SINK.n += df.MASK(mask, 0).ROWS; }, REPS));
row("CONCAT", N, time(() => { SINK.n += df.CONCAT(otherSame).ROWS; }, REPS));
row("COPY_TO_RECORDS", N, time(() => { SINK.n += df.TO_RECORDS().length; }, REPS));
row("COPY_TO_CSV", N, time(() => { SINK.n += df.TO_CSV().length; }, REPS));

row("JOIN_200kx5", N, time(() => { SINK.n += df.JOIN(other, "id", "id").ROWS; }, REPS));

row("PIVOT_200kx50", N, time(() => { SINK.n += df.PIVOT("id", "g", "v").ROWS; }, REPS));

{
    const t = new Float64Array(N);
    for (let i = 0; i < N; i++) t[i] = i * 1.0;
    const ts = new DataFrame({ t, v: new Float64Array(N).fill(1) });
    row("RESAMPLE_200k", N, time(() => { SINK.n += ts.RESAMPLE("t", 100).ROWS; }, REPS));
}

{
    const lt = new Int32Array(N), rt = new Int32Array(N);
    for (let i = 0; i < N; i++) { lt[i] = i; rt[i] = i; }
    const L = new DataFrame({ t: lt });
    const R = new DataFrame({ t: rt, v: new Float64Array(N).fill(1) });
    row("ASOF_JOIN_200k", N, time(() => { SINK.n += L.ASOF_JOIN(R, "t", "t").ROWS; }, REPS));
}

row("JSON_AGG_200k", N, time(() => { SINK.n += df.JSON_AGG("id", "v").length; }, REPS));

console.log("sink " + SINK.rows.toFixed(2) + "ms " + SINK.n + " cells " + SINK.cells);
