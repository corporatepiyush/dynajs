// covers: dyna:csv.parseDataFrame
// Owner ask E, wave 4 step 3: of the parsers (JSON, MessagePack, csv.parse, XML, YAML, csv.parseDataFrame) only
// parseDataFrame is native-dominated (profile: 40-80% of the others is JS value building, interleaved with the scan;
// parseDataFrame builds typed columns and a string dictionary in C and wraps them at the end). Its scan, on both the
// csvx fast path and the dfk fallback (ragged rows), is marked DYN_OFFLOAD_CPU, so under offload() it runs on the
// shared io pool and only the DataFrame wrap runs on the loop. Per path: the offloaded frame equals the direct call
// (dtypes and every column), errors reject with the direct call's error, and the promise is still pending when
// offload() returns (a mechanism, not a timer race: see tests/lib/offload_check.js). A ragged input runs both stretches
// (the fast scan, which gives up, then the fallback reader), so that check alone cannot see the second one: the longest
// loop stall during the call (best of 5 tries) must stay under half the direct call (about 0.4 ms against 46 ms; with
// the fallback mark removed it is about 0.9 of the direct call). Mutation: removing the fast-scan mark makes "loop ticks" red,
// removing the fallback mark makes the stall check red.
import { suite } from "../lib/kit.js";
import { same, loopTicks } from "../lib/offload_check.js";
import { offload } from "dyna:async";
import { parseDataFrame } from "dyna:csv";

const t = suite("csv.parseDataFrame.offload");
const ROWS = 200000;
const lines = ["id,region,qty,price,note"];
for (let i = 0; i < ROWS; i++) lines.push(`${i},r${i % 37},${i % 11},${(i * 0.25).toFixed(2)},note ${i % 101}`);
const FAST = lines.join("\n") + "\n";
const RAGGED = lines.map((l, i) => (i % 5000 === 7 ? l + ",extra" : l)).join("\n") + "\n";
const RAGGED_BIG = RAGGED + lines.slice(1).map((l, i) => (i % 5000 === 7 ? l + ",extra" : l)).join("\n") + "\n";
const view = (df) => JSON.stringify([df.DTYPES(), Object.entries(df.TO_COLUMNS()).map(([k, v]) => [k, Array.from(v, (x) => (x !== x ? "NaN" : x))])]);
const eqFrame = (x, y) => view(x) === view(y);

t.test("fast path: the offloaded frame equals the direct call, with options", async (a) => {
    const df = await same(a, "parseDataFrame", parseDataFrame, [FAST], eqFrame);
    a.eq(JSON.stringify(df.DTYPES()), JSON.stringify({ id: "f64", region: "str", qty: "f64", price: "f64", note: "str" }), "dtypes");
    await same(a, "columns + dtypes", parseDataFrame, [FAST, { columns: ["region", "qty"], dtypes: { qty: "i32" } }], eqFrame);
    await same(a, "offset + limit", parseDataFrame, [FAST, { offset: 1000, limit: 5000 }], eqFrame);
});

t.test("fallback path (ragged rows): the offloaded frame equals the direct call", async (a) => {
    await same(a, "ragged", parseDataFrame, [RAGGED], eqFrame);
});

t.test("errors reject with the direct call's error", async (a) => {
    for (const [label, args] of [["unterminated quote, strict", ["a,b\n1,\"x\n", { strict: true }]], ["i32 forced on text", [FAST, { dtypes: { note: "i32" } }]]]) {
        let direct;
        try { parseDataFrame(...args); } catch (e) { direct = e; }
        a.ok(direct, `${label}: the direct call throws`);
        const e = await a.rejects(offload(parseDataFrame, ...args), direct.constructor, `${label}: rejects`);
        a.eq(e && e.message, direct && direct.message, `${label}: same message`);
    }
});

t.test("loop ticks", async (a) => {
    await loopTicks(a, "fast path", parseDataFrame, [FAST]);
    await loopTicks(a, "fallback path", parseDataFrame, [RAGGED]);
});

async function maxGap(args) {
    let last = performance.now(), gap = 0, done = false;
    const tick = () => { const now = performance.now(); gap = Math.max(gap, now - last); last = now; if (!done) setTimeout(tick, 0); };
    setTimeout(tick, 0);
    await offload(parseDataFrame, ...args);
    done = true;
    return Math.max(gap, performance.now() - last);
}

t.test("the loop stays responsive through both stretches (ragged input runs the fast scan, then the fallback reader)", async (a) => {
    for (const [label, input] of [["fast path", FAST], ["fallback path", RAGGED_BIG]]) {
        let direct = Infinity, best = Infinity;
        for (let k = 0; k < 5; k++) {
            const t0 = performance.now();
            parseDataFrame(input);
            direct = Math.min(direct, performance.now() - t0);
            best = Math.min(best, await maxGap([input]));
        }
        a.ok(best < direct / 2, `${label}: longest loop stall ${best.toFixed(1)} ms is under half the direct call's ${direct.toFixed(1)} ms`);
    }
});

await t.run();
