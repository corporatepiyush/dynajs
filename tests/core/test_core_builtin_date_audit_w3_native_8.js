// flags: --std
// covers: c:src/builtins/builtin-date.c
// Split from tests/test_audit_w3_native_8.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_date_audit_w3_native_8");
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

await __kitS.section("control: setTimeout works", async () => {
    let n = 0;
    ok(kind(() => clearTimeout(setTimeout(() => {}, 0))) === "none", "control: setTimeout works");
    const ids = [];
    for (let k = 0; k < 500; k++) ids.push(setInterval(() => { n++; }, 1));
    const t0 = Date.now();
    while (n < 101000 && Date.now() - t0 < 20000) await sleep(5);
    const r = kind(() => clearTimeout(setTimeout(() => {}, 0)));
    for (const i of ids) clearInterval(i);
    ok(n >= 101000, "the intervals fired more than the 100000 cap (" + n + ")");
    ok(r === "none", "[red] C1-11: interval firings do not consume the live-timer cap (" + r + ")");
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_core_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_core_audit_w3_native_8: " + fail + " failures");
