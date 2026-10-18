// flags: --std
// Split from tests/test_audit_w3_native_8.js: the bench sections.
import { Path, makeTempDir, removeAll } from "dyna:file";
import { bench } from "dyna:bench";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- P1-24: bench -------------------------------------------------------
// ORACLE: a timing shorter than the clock's tick is not a measurement. With
// one clock pair per call, half the samples of an empty body are exactly 0,
// so the median is 0. Batching until a sample spans 0.1 ms makes the median a
// positive time per call.
// CONTROL: a body of about a millisecond is reported near a millisecond.
{
    const r = bench("empty", () => {}, { timeMs: 120, warmupMs: 10 });
    ok(r.p50Ms > 0, "[red] P1-24: the median of an empty body is a positive time, not a clock artefact (" + r.p50Ms + ")");
    ok(r.iters > 1000 && r.opsPerSec > 1000, "control: the empty body ran many times (" + r.iters + ")");
    const slow = bench("slow", () => { const e = performance.now() + 1; while (performance.now() < e); }, { timeMs: 60, warmupMs: 5 });
    ok(slow.p50Ms > 0.9 && slow.p50Ms < 5, "control: a 1 ms body is reported as about 1 ms (" + slow.p50Ms.toFixed(3) + ")");
}
removeAll(new Path(String(T)));
console.log("test_bench_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_bench_audit_w3_native_8: " + fail + " failures");
