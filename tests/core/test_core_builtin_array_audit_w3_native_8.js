// flags: --std
// covers: c:src/builtins/builtin-array.c
// Split from tests/test_audit_w3_native_8.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_core_builtin_array_audit_w3_native_8");
import * as os from "os";
import { Path, makeTempDir, removeAll } from "dyna:file";
import { cpuNow } from "../lib/cpuclock.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- C1-11: JS timers ---------------------------------------------------
// ORACLE: a timer queue is a priority queue; set and clear are O(log n), so
// the cost per operation cannot grow tenfold when n grows tenfold. The old
// sorted list was linear per operation (measured 2.7 us at 2k, 28 us at 20k).
// CONTROL: firing order is by deadline, ties in creation order; a cleared
// timer never fires.

await __kitS.section("[red] C1-11: timer set+clear cost per op is flat", async () => {
    const per = (n) => {
        const ids = [];
        const t = cpuNow();
        for (let i = 0; i < n; i++) ids.push(setTimeout(() => {}, 200000 - i));
        for (const i of ids) clearTimeout(i);
        return (cpuNow() - t) / n;
    };
    per(500);
    const small = per(2000), big = per(20000);
    perfCheck(ok, big < small * 4, "[red] C1-11: timer set+clear cost per op is flat (" + (small * 1e6).toFixed(0) + " ns at 2k, " + (big * 1e6).toFixed(0) + " ns at 20k)");

    // A deadline is creation time + delay, and a slow build (ASan) spreads the creations over
    // milliseconds; so equal delays must keep creation order, and deadlines 2+ ms apart must not swap.
    const order = [], due = [];
    const delays = [7, 3, 9, 3, 1, 5, 9, 1, 4];
    delays.forEach((d, i) => { due[i] = performance.now() + d; setTimeout(() => order.push(i), d); });
    const dead = setTimeout(() => order.push(-1), 2);
    clearTimeout(dead);
    await sleep(40);
    const bad = [];
    for (let x = 0; x < order.length; x++)
        for (let y = x + 1; y < order.length; y++) {
            const i = order[x], j = order[y];
            if ((delays[i] === delays[j] && i > j) || due[i] - due[j] >= 2) bad.push(i + " before " + j);
        }
    ok(order.length === delays.length && !order.includes(-1) && bad.length === 0,
        "control: timers fire by deadline, ties in creation order (" + order.map((i) => delays[i] * 100 + i).join() + (bad.length ? "; " + bad.join(", ") : "") + ")");
    ok(!order.includes(-1), "control: a cleared timer never fires");
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_core_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_core_audit_w3_native_8: " + fail + " failures");
